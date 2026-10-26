// Vulkan presentation (see present.h). The functions are loaded through SDL's vkGetInstanceProcAddr, so the exe does
// not link vulkan-1 and starts (without a picture) on a machine with no Vulkan driver.
// The present thread waits for a frame from the game, acquires a swapchain image, fits the newest game frame into it
// (the replayer's output image) and presents it. The game never waits on the display here; its pacing comes from the vsync events (front_win.c).
// Vsync on: FIFO. Vsync off (the default): mailbox when the surface offers it (the newest frame wins, no tearing).
#include "present.h"
#include "vk.h"
#include "upscale/upscale_spv.h"

#include <SDL3/SDL.h>
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LOG(...) (fprintf(stderr, "[totk-vk] " __VA_ARGS__), fputc('\n', stderr))
#define FRAMES 2          // frames in flight
#define MAX_IMAGES 8

static SDL_Window *g_window;
static int linear_only;   // TOTK_LINEAR_BLIT=1: skip EASU/RCAS and blit the game frame as is (a reference for timing)
static VkSwapchainKHR swapchain;
static VkFormat format;
static VkExtent2D extent;
static VkImage images[MAX_IMAGES];
static uint32_t nimages;
static VkSemaphore image_ready[FRAMES], render_done[MAX_IMAGES];
static VkFence in_flight[FRAMES];
static VkCommandPool pool;
static VkCommandBuffer cmd[FRAMES];
static VkPresentModeKHR mode;

static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cv = PTHREAD_COND_INITIALIZER;
static uint64_t frames_in;           // under mu
static _Atomic uint64_t presented;
static _Atomic int resized;
static _Atomic int want_vsync = 0, want_mailbox = 1;   // set by vkp_set_present; the swapchain is rebuilt in that mode
static _Atomic int want_scale = 100;                   // render_scale percent, set by vkp_set_render_scale

// Command buffers and sync objects of the present thread (the device is vk.c's).
static int create_present_objects(void) {
    VkCommandPoolCreateInfo pi = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                  .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, .queueFamilyIndex = vkc.family};
    if (!vk_check(vk.vkCreateCommandPool(vkc.device, &pi, NULL, &pool), "vkCreateCommandPool")) return 0;
    VkCommandBufferAllocateInfo ai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = pool,
                                      .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = FRAMES};
    if (!vk_check(vk.vkAllocateCommandBuffers(vkc.device, &ai, cmd), "vkAllocateCommandBuffers")) return 0;
    VkSemaphoreCreateInfo si = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    VkFenceCreateInfo fi = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, .flags = VK_FENCE_CREATE_SIGNALED_BIT};
    for (int i = 0; i < FRAMES; i++)
        if (!vk_check(vk.vkCreateSemaphore(vkc.device, &si, NULL, &image_ready[i]), "vkCreateSemaphore") ||
            !vk_check(vk.vkCreateFence(vkc.device, &fi, NULL, &in_flight[i]), "vkCreateFence")) return 0;
    return 1;
}

// Upscale compute pipelines: EASU (the window's upscaler) and RCAS (its sharpening pass). Built once; each frame's
// descriptor sets are written in present_one. A failure leaves them unbuilt and the window gets the linear blit.
static VkDescriptorSetLayout easu_dsl, rcas_dsl;
static VkPipelineLayout easu_pl, rcas_pl;
static VkPipeline easu_pipe, rcas_pipe;
static VkDescriptorPool upscale_pool;
static VkDescriptorSet easu_set[FRAMES], rcas_set[FRAMES];
static int upscale_ready;

// Per frame slot, the EASU output (index 0) and the RCAS output (1): rgba8 storage images the size of the swapchain.
// create_swapchain builds them; targets_ready = 0 sends the frames to the linear blit.
static VkImage up_img[FRAMES][2];
static VkDeviceMemory up_mem[FRAMES][2];
static VkImageView up_view[FRAMES][2];
static int targets_ready;
static int create_upscale_targets(void);

static int make_shader(const uint32_t *code, size_t bytes, VkShaderModule *out) {
    VkShaderModuleCreateInfo si = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = bytes, .pCode = code};
    return vk_check(vk.vkCreateShaderModule(vkc.device, &si, NULL, out), "vkCreateShaderModule");
}

// Both passes take two bindings on set 0; push_bytes is the size of the pass's push constants.
static int make_layout(VkDescriptorType a, VkDescriptorType b, uint32_t push_bytes, VkDescriptorSetLayout *dsl,
                       VkPipelineLayout *pl) {
    VkDescriptorSetLayoutBinding bind[2] = {
        {0, a, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL},
        {1, b, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL}};
    VkDescriptorSetLayoutCreateInfo dl = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 2, .pBindings = bind};
    if (!vk_check(vk.vkCreateDescriptorSetLayout(vkc.device, &dl, NULL, dsl), "vkCreateDescriptorSetLayout")) return 0;
    VkPushConstantRange pr = {VK_SHADER_STAGE_COMPUTE_BIT, 0, push_bytes};
    VkPipelineLayoutCreateInfo pli = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1, .pSetLayouts = dsl,
                                      .pushConstantRangeCount = 1, .pPushConstantRanges = &pr};
    return vk_check(vk.vkCreatePipelineLayout(vkc.device, &pli, NULL, pl), "vkCreatePipelineLayout");
}

static int make_compute(VkShaderModule mod, VkPipelineLayout layout, VkPipeline *out) {
    VkComputePipelineCreateInfo ci = {VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        .stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_COMPUTE_BIT, mod, "main", NULL},
        .layout = layout};
    return vk_check(vk.vkCreateComputePipelines(vkc.device, VK_NULL_HANDLE, 1, &ci, NULL, out), "vkCreateComputePipelines");
}

static void destroy_upscale(void) {
    if (upscale_pool) vk.vkDestroyDescriptorPool(vkc.device, upscale_pool, NULL);   // frees the sets too
    if (easu_pipe) vk.vkDestroyPipeline(vkc.device, easu_pipe, NULL);
    if (rcas_pipe) vk.vkDestroyPipeline(vkc.device, rcas_pipe, NULL);
    if (easu_pl) vk.vkDestroyPipelineLayout(vkc.device, easu_pl, NULL);
    if (rcas_pl) vk.vkDestroyPipelineLayout(vkc.device, rcas_pl, NULL);
    if (easu_dsl) vk.vkDestroyDescriptorSetLayout(vkc.device, easu_dsl, NULL);
    if (rcas_dsl) vk.vkDestroyDescriptorSetLayout(vkc.device, rcas_dsl, NULL);
    upscale_pool = VK_NULL_HANDLE;
    easu_pipe = rcas_pipe = VK_NULL_HANDLE;
    easu_pl = rcas_pl = VK_NULL_HANDLE;
    easu_dsl = rcas_dsl = VK_NULL_HANDLE;
}

// 1 = the upscale pipelines and their descriptor sets exist; 0 = logged, and the caller falls back to linear.
static int create_upscale(void) {
    VkShaderModule easu_mod = VK_NULL_HANDLE, rcas_mod = VK_NULL_HANDLE;
    int ok = make_shader(upscale_easu_spv, upscale_easu_spv_size, &easu_mod) &&
             make_shader(upscale_rcas_spv, upscale_rcas_spv_size, &rcas_mod) &&
             make_layout(VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 28, &easu_dsl, &easu_pl) &&
             make_layout(VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 12, &rcas_dsl, &rcas_pl) &&
             make_compute(easu_mod, easu_pl, &easu_pipe) &&
             make_compute(rcas_mod, rcas_pl, &rcas_pipe);
    if (easu_mod) vk.vkDestroyShaderModule(vkc.device, easu_mod, NULL);
    if (rcas_mod) vk.vkDestroyShaderModule(vkc.device, rcas_mod, NULL);
    if (ok) {
        // One EASU set (sampled + storage) and one RCAS set (two storage) per frame in flight.
        VkDescriptorPoolSize sizes[] = {{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, FRAMES}, {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 3 * FRAMES}};
        VkDescriptorPoolCreateInfo pi = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = 2 * FRAMES,
                                         .poolSizeCount = 2, .pPoolSizes = sizes};
        VkDescriptorSetLayout easu_l[FRAMES], rcas_l[FRAMES];
        for (int i = 0; i < FRAMES; i++) {
            easu_l[i] = easu_dsl;
            rcas_l[i] = rcas_dsl;
        }
        VkDescriptorSetAllocateInfo ai = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .descriptorSetCount = FRAMES};
        ok = vk_check(vk.vkCreateDescriptorPool(vkc.device, &pi, NULL, &upscale_pool), "vkCreateDescriptorPool");
        if (ok) {
            ai.descriptorPool = upscale_pool;
            ai.pSetLayouts = easu_l;
            ok = vk_check(vk.vkAllocateDescriptorSets(vkc.device, &ai, easu_set), "vkAllocateDescriptorSets");
        }
        if (ok) {
            ai.pSetLayouts = rcas_l;
            ok = vk_check(vk.vkAllocateDescriptorSets(vkc.device, &ai, rcas_set), "vkAllocateDescriptorSets");
        }
    }
    if (!ok) {
        LOG("upscale pipelines unavailable; the window uses the linear blit");
        destroy_upscale();
    }
    return ok;
}

static void destroy_upscale_targets(void) {
    for (int i = 0; i < FRAMES; i++)
        for (int j = 0; j < 2; j++) {
            vk.vkDestroyImageView(vkc.device, up_view[i][j], NULL);
            vk.vkDestroyImage(vkc.device, up_img[i][j], NULL);
            vk.vkFreeMemory(vkc.device, up_mem[i][j], NULL);
            up_view[i][j] = VK_NULL_HANDLE;
            up_img[i][j] = VK_NULL_HANDLE;
            up_mem[i][j] = VK_NULL_HANDLE;
        }
    targets_ready = 0;
}

static void destroy_swapchain(void) {
    destroy_upscale_targets();
    for (uint32_t i = 0; i < nimages; i++) vk.vkDestroySemaphore(vkc.device, render_done[i], NULL);
    if (swapchain) vk.vkDestroySwapchainKHR(vkc.device, swapchain, NULL);
    swapchain = VK_NULL_HANDLE;
    nimages = 0;
}

static VkPresentModeKHR pick_mode(const VkPresentModeKHR *modes, uint32_t nm) {
    int has_mailbox = 0, has_immediate = 0;
    for (uint32_t i = 0; i < nm; i++) {
        if (modes[i] == VK_PRESENT_MODE_MAILBOX_KHR) has_mailbox = 1;
        if (modes[i] == VK_PRESENT_MODE_IMMEDIATE_KHR) has_immediate = 1;
    }
    if (atomic_load(&want_vsync)) return VK_PRESENT_MODE_FIFO_KHR;
    if (atomic_load(&want_mailbox) && has_mailbox) return VK_PRESENT_MODE_MAILBOX_KHR;
    if (has_immediate) return VK_PRESENT_MODE_IMMEDIATE_KHR;
    return VK_PRESENT_MODE_FIFO_KHR;
}

// 0: the window has no area now (minimised) — try again later
static int create_swapchain(void) {
    VkSurfaceCapabilitiesKHR caps;
    if (!vk_check(vk.vkGetPhysicalDeviceSurfaceCapabilitiesKHR(vkc.gpu, vkc.surface, &caps), "surface capabilities")) return 0;
    extent = caps.currentExtent;
    if (extent.width == UINT32_MAX) {
        int w, h;
        SDL_GetWindowSizeInPixels(g_window, &w, &h);
        extent = (VkExtent2D){(uint32_t)w, (uint32_t)h};
    }
    if (!extent.width || !extent.height) return 0;
    uint32_t nf = 0;
    vk.vkGetPhysicalDeviceSurfaceFormatsKHR(vkc.gpu, vkc.surface, &nf, NULL);
    VkSurfaceFormatKHR *fm = calloc(nf ? nf : 1, sizeof *fm);
    vk.vkGetPhysicalDeviceSurfaceFormatsKHR(vkc.gpu, vkc.surface, &nf, fm);
    VkSurfaceFormatKHR pick = fm[0];
    for (uint32_t i = 0; i < nf; i++)
        if ((fm[i].format == VK_FORMAT_B8G8R8A8_UNORM || fm[i].format == VK_FORMAT_R8G8B8A8_UNORM) &&
            fm[i].colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) { pick = fm[i]; break; }
    free(fm);
    format = pick.format;
    uint32_t nm = 0;
    vk.vkGetPhysicalDeviceSurfacePresentModesKHR(vkc.gpu, vkc.surface, &nm, NULL);
    VkPresentModeKHR modes[16];
    if (nm > 16) nm = 16;
    vk.vkGetPhysicalDeviceSurfacePresentModesKHR(vkc.gpu, vkc.surface, &nm, modes);
    mode = pick_mode(modes, nm);
    uint32_t count = caps.minImageCount + 1;
    if (caps.maxImageCount && count > caps.maxImageCount) count = caps.maxImageCount;
    VkSwapchainCreateInfoKHR ci = {VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR, .surface = vkc.surface,
        .minImageCount = count, .imageFormat = format, .imageColorSpace = pick.colorSpace, .imageExtent = extent,
        .imageArrayLayers = 1, .imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
        .imageSharingMode = VK_SHARING_MODE_EXCLUSIVE, .preTransform = caps.currentTransform,
        .compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR, .presentMode = mode, .clipped = VK_TRUE};
    if (!vk_check(vk.vkCreateSwapchainKHR(vkc.device, &ci, NULL, &swapchain), "vkCreateSwapchainKHR")) return 0;
    nimages = MAX_IMAGES;
    vk.vkGetSwapchainImagesKHR(vkc.device, swapchain, &nimages, images);
    VkSemaphoreCreateInfo si = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    for (uint32_t i = 0; i < nimages; i++) vk.vkCreateSemaphore(vkc.device, &si, NULL, &render_done[i]);
    if (upscale_ready) targets_ready = create_upscale_targets();
    LOG("swapchain %ux%u, %u images, %s", extent.width, extent.height, nimages,
        mode == VK_PRESENT_MODE_MAILBOX_KHR ? "mailbox" : mode == VK_PRESENT_MODE_IMMEDIATE_KHR ? "immediate" : "fifo");
    return 1;
}

static void barrier(VkCommandBuffer cb, VkImage img, VkImageLayout from, VkImageLayout to, VkAccessFlags src,
                    VkAccessFlags dst, VkPipelineStageFlags s0, VkPipelineStageFlags s1) {
    VkImageMemoryBarrier b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .srcAccessMask = src, .dstAccessMask = dst,
        .oldLayout = from, .newLayout = to, .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = img,
        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    vk.vkCmdPipelineBarrier(cb, s0, s1, 0, 0, NULL, 0, NULL, 1, &b);
}

// Until the first game frame (and for the null device): a colour that cycles with the frame number.
static VkClearColorValue frame_colour(uint64_t n) {
    float t = (float)(n % 240) / 240.0f * 6.2831853f;
    return (VkClearColorValue){{0.15f + 0.1f * sinf(t), 0.15f + 0.1f * sinf(t + 2.094f), 0.2f + 0.1f * sinf(t + 4.188f), 1}};
}

// ---- output images: written by the replay thread, shown by the present thread
#define OUTS 3
typedef struct {
    VkImage img; VkDeviceMemory mem; uint32_t w, h;
    VkImageView view;            // sampled by the upscale pass; NULL = that frame uses the linear blit
    int state;                  // 0 free, 1 written (pending), 2 being shown
    uint64_t seq;                // when it was written (newest pending wins)
    VkSemaphore timeline; uint64_t value; int crop[4];
} out_t;
static out_t g_out[OUTS];
static uint64_t g_out_seq;       // under mu

static uint32_t memory_type(uint32_t bits, VkMemoryPropertyFlags want) {
    for (uint32_t i = 0; i < vkc.memory.memoryTypeCount; i++)
        if ((bits & (1u << i)) && (vkc.memory.memoryTypes[i].propertyFlags & want) == want) return i;
    return UINT32_MAX;
}

// The EASU and RCAS outputs for every frame slot, at the swapchain size. A failure frees what was made.
static int create_upscale_targets(void) {
    VkImageCreateInfo ci = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = VK_FORMAT_R8G8B8A8_UNORM,
        .extent = {extent.width, extent.height, 1}, .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
    for (int i = 0; i < FRAMES; i++)
        for (int j = 0; j < 2; j++) {
            VkMemoryRequirements req;
            if (!vk_check(vk.vkCreateImage(vkc.device, &ci, NULL, &up_img[i][j]), "upscale image")) goto fail;
            vk.vkGetImageMemoryRequirements(vkc.device, up_img[i][j], &req);
            VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = req.size,
                                       .memoryTypeIndex = memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)};
            if (!vk_check(vk.vkAllocateMemory(vkc.device, &ai, NULL, &up_mem[i][j]), "upscale memory")) goto fail;
            vk.vkBindImageMemory(vkc.device, up_img[i][j], up_mem[i][j], 0);
            VkImageViewCreateInfo vi = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = up_img[i][j], .viewType = VK_IMAGE_VIEW_TYPE_2D,
                                        .format = VK_FORMAT_R8G8B8A8_UNORM, .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
            if (!vk_check(vk.vkCreateImageView(vkc.device, &vi, NULL, &up_view[i][j]), "upscale view")) goto fail;
        }
    return 1;
fail:
    LOG("upscale targets unavailable; the window uses the linear blit");
    destroy_upscale_targets();
    return 0;
}

int vkp_active(void) { return swapchain != VK_NULL_HANDLE || g_window != NULL; }

int vkp_out_acquire(uint32_t w, uint32_t h, VkImage *image) {
    pthread_mutex_lock(&mu);
    int pick = -1;
    for (int i = 0; i < OUTS && pick < 0; i++) if (g_out[i].state == 0) pick = i;
    if (pick < 0)   // none free: overwrite the oldest pending one (it was never shown; newer frames win)
        for (int i = 0; i < OUTS; i++) if (g_out[i].state == 1 && (pick < 0 || g_out[i].seq < g_out[pick].seq)) pick = i;
    if (pick >= 0) g_out[pick].state = 3;   // being written
    pthread_mutex_unlock(&mu);
    if (pick < 0) return -1;
    out_t *o = &g_out[pick];
    if (o->img && (o->w != w || o->h != h)) {   // free: no present work uses it
        vk.vkDestroyImageView(vkc.device, o->view, NULL);
        vk.vkDestroyImage(vkc.device, o->img, NULL);
        vk.vkFreeMemory(vkc.device, o->mem, NULL);
        o->img = VK_NULL_HANDLE;
        o->view = VK_NULL_HANDLE;
    }
    if (!o->img) {
        VkImageCreateInfo ci = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = VK_FORMAT_B8G8R8A8_UNORM,
            .extent = {w, h, 1}, .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
            .usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
            .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
        VkMemoryRequirements req;
        if (!vk_check(vk.vkCreateImage(vkc.device, &ci, NULL, &o->img), "output image")) goto fail;
        vk.vkGetImageMemoryRequirements(vkc.device, o->img, &req);
        VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = req.size,
                                   .memoryTypeIndex = memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)};
        if (!vk_check(vk.vkAllocateMemory(vkc.device, &ai, NULL, &o->mem), "output memory")) {
            vk.vkDestroyImage(vkc.device, o->img, NULL);
            o->img = VK_NULL_HANDLE;
            goto fail;
        }
        vk.vkBindImageMemory(vkc.device, o->img, o->mem, 0);
        VkImageViewCreateInfo vi = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = o->img, .viewType = VK_IMAGE_VIEW_TYPE_2D,
                                    .format = VK_FORMAT_B8G8R8A8_UNORM, .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
        if (!vk_check(vk.vkCreateImageView(vkc.device, &vi, NULL, &o->view), "output view")) o->view = VK_NULL_HANDLE;
        o->w = w; o->h = h;
    }
    *image = o->img;
    return pick;
fail:
    pthread_mutex_lock(&mu);
    o->state = 0;
    pthread_mutex_unlock(&mu);
    return -1;
}

void vkp_out_ready(int slot, VkSemaphore timeline, uint64_t value, const int crop[4]) {
    pthread_mutex_lock(&mu);
    out_t *o = &g_out[slot];
    o->timeline = timeline; o->value = value; o->seq = ++g_out_seq; o->state = 1;
    if (crop) memcpy(o->crop, crop, sizeof o->crop); else memset(o->crop, 0, sizeof o->crop);
    for (int i = 0; i < OUTS; i++)   // older pending frames are never shown
        if (i != slot && g_out[i].state == 1 && g_out[i].seq < o->seq) g_out[i].state = 0;
    frames_in++;
    pthread_cond_signal(&cv);
    pthread_mutex_unlock(&mu);
}

// ---- settings overlay: a BGRA panel (vkp_overlay) copied into a host-visible buffer per frame slot, then over the picture
static uint32_t ovl_cpu[VKP_OVERLAY_MAX_W * VKP_OVERLAY_MAX_H];   // under mu
static int ovl_w, ovl_h;                                           // under mu; 0 = hidden
static VkBuffer ovl_buf[FRAMES];
static VkDeviceMemory ovl_mem[FRAMES];
static uint32_t *ovl_map[FRAMES];
#define OVL_BYTES ((VkDeviceSize)VKP_OVERLAY_MAX_W * VKP_OVERLAY_MAX_H * 4)

// Staging buffers, one per frame in flight, mapped for the whole run.
static int create_overlay(void) {
    VkBufferCreateInfo bi = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = OVL_BYTES,
                             .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT, .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
    for (int i = 0; i < FRAMES; i++) {
        VkMemoryRequirements req;
        if (!vk_check(vk.vkCreateBuffer(vkc.device, &bi, NULL, &ovl_buf[i]), "overlay buffer")) return 0;
        vk.vkGetBufferMemoryRequirements(vkc.device, ovl_buf[i], &req);
        uint32_t type = memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        if (type == UINT32_MAX) { LOG("no host-visible memory for the overlay"); return 0; }
        VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = req.size, .memoryTypeIndex = type};
        if (!vk_check(vk.vkAllocateMemory(vkc.device, &ai, NULL, &ovl_mem[i]), "overlay memory")) return 0;
        vk.vkBindBufferMemory(vkc.device, ovl_buf[i], ovl_mem[i], 0);
        if (!vk_check(vk.vkMapMemory(vkc.device, ovl_mem[i], 0, VK_WHOLE_SIZE, 0, (void **)&ovl_map[i]), "overlay map")) return 0;
    }
    return 1;
}

// The panel for this frame, copied into the slot's buffer (R and B swapped when the swapchain is RGBA). 0 = none, or
// it does not fit the window.
static int overlay_upload(unsigned slot, int *w, int *h) {
    pthread_mutex_lock(&mu);
    *w = ovl_w; *h = ovl_h;
    if (*w > 0) memcpy(ovl_map[slot], ovl_cpu, (size_t)*w * *h * 4);
    pthread_mutex_unlock(&mu);
    if (*w <= 0 || *w > (int)extent.width || *h > (int)extent.height) return 0;
    if (format == VK_FORMAT_R8G8B8A8_UNORM)
        for (size_t i = 0, n = (size_t)*w * *h; i < n; i++) {
            uint32_t p = ovl_map[slot][i];
            ovl_map[slot][i] = (p & 0xff00ff00u) | ((p & 0xffu) << 16) | ((p >> 16) & 0xffu);
        }
    return 1;
}

// A panel larger than the maximum (or NULL) hides the overlay.
void vkp_overlay(const uint32_t *bgra, int w, int h) {
    pthread_mutex_lock(&mu);
    if (bgra && w > 0 && h > 0 && w <= VKP_OVERLAY_MAX_W && h <= VKP_OVERLAY_MAX_H) {
        memcpy(ovl_cpu, bgra, (size_t)w * h * 4);
        ovl_w = w; ovl_h = h;
    } else {
        ovl_w = ovl_h = 0;
    }
    pthread_mutex_unlock(&mu);
}

// The push constants of the two passes, laid out as the shaders' Push blocks.
typedef struct { float src_origin[2], src_extent[2]; int32_t dst_extent[2], mode; } easu_push_t;
typedef struct { int32_t extent[2]; float sharp; } rcas_push_t;
#define UPSCALE_MODE_EASU 2      // the FSR1-path clamp (1 is plain Catmull-Rom)
#define UPSCALE_SHARP 0.2f       // RCAS strength

// The game's crop (sx, sy, sw, sh) scaled to dw x dh by EASU and sharpened by RCAS. The result is the slot's RCAS
// output, left in TRANSFER_SRC layout for the blit into the window. The caller checks targets_ready first.
static void upscale_record(VkCommandBuffer cb, unsigned slot, out_t *out, int sx, int sy, int sw, int sh, int dw, int dh) {
    VkDescriptorImageInfo src = {VK_NULL_HANDLE, out->view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkDescriptorImageInfo easu_dst = {VK_NULL_HANDLE, up_view[slot][0], VK_IMAGE_LAYOUT_GENERAL};
    VkDescriptorImageInfo rcas_dst = {VK_NULL_HANDLE, up_view[slot][1], VK_IMAGE_LAYOUT_GENERAL};
    VkWriteDescriptorSet w[4] = {
        {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = easu_set[slot], .dstBinding = 0, .descriptorCount = 1,
         .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, .pImageInfo = &src},
        {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = easu_set[slot], .dstBinding = 1, .descriptorCount = 1,
         .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, .pImageInfo = &easu_dst},
        {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = rcas_set[slot], .dstBinding = 0, .descriptorCount = 1,
         .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, .pImageInfo = &easu_dst},
        {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = rcas_set[slot], .dstBinding = 1, .descriptorCount = 1,
         .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, .pImageInfo = &rcas_dst}};
    vk.vkUpdateDescriptorSets(vkc.device, 4, w, 0, NULL);   // the slot's fence has passed: its sets are free

    VkImage easu_img = up_img[slot][0], rcas_img = up_img[slot][1];
    barrier(cb, easu_img, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, 0, VK_ACCESS_SHADER_WRITE_BIT,
            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
    barrier(cb, out->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
    vk.vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, easu_pipe);
    vk.vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, easu_pl, 0, 1, &easu_set[slot], 0, NULL);
    easu_push_t ep = {{(float)sx, (float)sy}, {(float)sw, (float)sh}, {dw, dh}, UPSCALE_MODE_EASU};
    vk.vkCmdPushConstants(cb, easu_pl, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof ep, &ep);
    vk.vkCmdDispatch(cb, (uint32_t)(dw + 7) / 8, (uint32_t)(dh + 7) / 8, 1);

    barrier(cb, easu_img, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
    barrier(cb, out->img, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_SHADER_READ_BIT,
            VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    barrier(cb, rcas_img, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, 0, VK_ACCESS_SHADER_WRITE_BIT,
            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
    vk.vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, rcas_pipe);
    vk.vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, rcas_pl, 0, 1, &rcas_set[slot], 0, NULL);
    rcas_push_t rp = {{dw, dh}, UPSCALE_SHARP};
    vk.vkCmdPushConstants(cb, rcas_pl, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof rp, &rp);
    vk.vkCmdDispatch(cb, (uint32_t)(dw + 7) / 8, (uint32_t)(dh + 7) / 8, 1);

    barrier(cb, rcas_img, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_SHADER_WRITE_BIT,
            VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
}

// One window frame: the newest game frame (out, may be NULL: the cycling colour) fitted with its aspect ratio.
static int present_one(uint64_t n, unsigned slot, out_t *out) {
    if (atomic_exchange(&resized, 0) || !swapchain) {
        vk.vkWaitForFences(vkc.device, FRAMES, in_flight, VK_TRUE, UINT64_MAX);   // the present thread's own work only
        destroy_swapchain();
        if (!create_swapchain()) return 0;
    }
    vk.vkWaitForFences(vkc.device, 1, &in_flight[slot], VK_TRUE, UINT64_MAX);
    uint32_t idx;
    VkResult r = vk.vkAcquireNextImageKHR(vkc.device, swapchain, UINT64_MAX, image_ready[slot], VK_NULL_HANDLE, &idx);
    if (r == VK_ERROR_OUT_OF_DATE_KHR) { atomic_store(&resized, 1); return 0; }
    if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) { vk_check(r, "vkAcquireNextImageKHR"); return 0; }
    vk.vkResetFences(vkc.device, 1, &in_flight[slot]);
    VkCommandBuffer cb = cmd[slot];
    vk.vkResetCommandBuffer(cb, 0);
    VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                   .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    vk.vkBeginCommandBuffer(cb, &bi);
    barrier(cb, images[idx], VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
            VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    VkClearColorValue c = out ? (VkClearColorValue){{0, 0, 0, 1}} : frame_colour(n);   // bars around the game picture
    VkImageSubresourceRange range = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vk.vkCmdClearColorImage(cb, images[idx], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &c, 1, &range);
    if (out) {
        // source rectangle: the game's crop (NVN WindowCrop; 0 size = the whole texture)
        int sx = out->crop[0], sy = out->crop[1], sw = out->crop[2], sh = out->crop[3];
        if (sw <= 0 || sh <= 0 || sx + sw > (int)out->w || sy + sh > (int)out->h) { sx = sy = 0; sw = (int)out->w; sh = (int)out->h; }
        double scale = (double)extent.width / sw < (double)extent.height / sh ? (double)extent.width / sw : (double)extent.height / sh;
        int dw = (int)(sw * scale + 0.5), dh = (int)(sh * scale + 0.5), dx = ((int)extent.width - dw) / 2, dy = ((int)extent.height - dh) / 2;
        // render_scale: the upscale passes run at this share of the window's size; the blit below scales them back up
        int pct = atomic_load(&want_scale);
        int rw = dw * pct / 100 > 0 ? dw * pct / 100 : 1, rh = dh * pct / 100 > 0 ? dh * pct / 100 : 1;
        barrier(cb, images[idx], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        VkImage src_img = out->img;
        VkImageBlit b = {{VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {{sx, sy, 0}, {sx + sw, sy + sh, 1}},
                         {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {{dx, dy, 0}, {dx + dw, dy + dh, 1}}};
        VkFilter filter = VK_FILTER_LINEAR;
        int upscale = !linear_only && targets_ready && out->view && dw > 0 && dh > 0;
        static int path_logged;   // one line per run: which present path the game frames take
        if (!path_logged) {
            path_logged = 1;
            LOG("present: %s, crop %dx%d -> %dx%d (render %dx%d, %d%%) in a %ux%u window", upscale ? "EASU + RCAS" : "linear blit",
                sw, sh, dw, dh, rw, rh, pct, extent.width, extent.height);
        }
        if (upscale) {   // the upscale passes; else the linear blit of the crop
            upscale_record(cb, slot, out, sx, sy, sw, sh, rw, rh);
            // the sharpened picture sits at the origin of the slot's RCAS output; NEAREST stretches it to the window
            src_img = up_img[slot][1];
            b.srcOffsets[0] = (VkOffset3D){0, 0, 0};
            b.srcOffsets[1] = (VkOffset3D){rw, rh, 1};
            filter = VK_FILTER_NEAREST;
        }
        vk.vkCmdBlitImage(cb, src_img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, images[idx], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                          1, &b, filter);
    }
    int ow, oh;
    if (overlay_upload(slot, &ow, &oh)) {   // the settings panel, centred over the picture
        barrier(cb, images[idx], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        VkBufferImageCopy rg = {0, (uint32_t)ow, (uint32_t)oh, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                                {((int)extent.width - ow) / 2, ((int)extent.height - oh) / 2, 0}, {(uint32_t)ow, (uint32_t)oh, 1}};
        vk.vkCmdCopyBufferToImage(cb, ovl_buf[slot], images[idx], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &rg);
    }
    barrier(cb, images[idx], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
            VK_ACCESS_TRANSFER_WRITE_BIT, 0, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
    vk.vkEndCommandBuffer(cb);
    // wait for the swapchain image and, with a game frame, for the replayer's copy into the output image
    VkSemaphoreSubmitInfo waits[2] = {
        {VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO, .semaphore = image_ready[slot], .stageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT},
        {VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO, .semaphore = out ? out->timeline : VK_NULL_HANDLE, .value = out ? out->value : 0,
         .stageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT}};
    VkSemaphoreSubmitInfo sig = {VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO, .semaphore = render_done[idx],
                                 .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT};
    VkCommandBufferSubmitInfo cbi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO, .commandBuffer = cb};
    VkSubmitInfo2 si = {VK_STRUCTURE_TYPE_SUBMIT_INFO_2, .waitSemaphoreInfoCount = out ? 2u : 1u, .pWaitSemaphoreInfos = waits,
                        .commandBufferInfoCount = 1, .pCommandBufferInfos = &cbi, .signalSemaphoreInfoCount = 1,
                        .pSignalSemaphoreInfos = &sig};
    VkPresentInfoKHR pi = {VK_STRUCTURE_TYPE_PRESENT_INFO_KHR, .waitSemaphoreCount = 1,
                           .pWaitSemaphores = &render_done[idx], .swapchainCount = 1, .pSwapchains = &swapchain,
                           .pImageIndices = &idx};
    vk_queue_lock();   // the replayer submits on the same queue
    VkResult s = vk.vkQueueSubmit2(vkc.queue, 1, &si, in_flight[slot]);
    r = s == VK_SUCCESS ? vk.vkQueuePresentKHR(vkc.queue, &pi) : s;
    vk_queue_unlock();
    if (!vk_check(s, "vkQueueSubmit2")) return 0;
    if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR) atomic_store(&resized, 1);
    else if (r != VK_SUCCESS) { vk_check(r, "vkQueuePresentKHR"); return 0; }
    if (out) vk.vkWaitForFences(vkc.device, 1, &in_flight[slot], VK_TRUE, UINT64_MAX);   // the output image is free again after this
    atomic_fetch_add(&presented, 1);
    return 1;
}

static void *present_thread(void *unused) {
    (void)unused;
    uint64_t shown = 0;
    unsigned slot = 0;
    for (;;) {
        pthread_mutex_lock(&mu);
        while (frames_in == shown) pthread_cond_wait(&cv, &mu);
        shown = frames_in;   // frames that came while the last one was shown are skipped: only the newest counts
        int pick = -1;
        for (int i = 0; i < OUTS; i++) if (g_out[i].state == 1 && (pick < 0 || g_out[i].seq > g_out[pick].seq)) pick = i;
        if (pick >= 0) g_out[pick].state = 2;
        pthread_mutex_unlock(&mu);
        if (present_one(shown, slot, pick >= 0 ? &g_out[pick] : NULL)) slot = (slot + 1) % FRAMES;
        if (pick >= 0) {
            pthread_mutex_lock(&mu);
            g_out[pick].state = 0;
            pthread_mutex_unlock(&mu);
        }
    }
    return NULL;
}

int vkp_init(SDL_Window *window) {
    g_window = window;
    const char *lb = getenv("TOTK_LINEAR_BLIT");
    linear_only = lb && lb[0] == '1';
    if (!vk_init(window) || !vkc.surface || !create_present_objects() || !create_overlay()) return 0;
    upscale_ready = create_upscale();   // not fatal: without it the window keeps the linear blit
    if (!create_swapchain()) LOG("window has no area yet; swapchain deferred");
    pthread_t t;
    if (pthread_create(&t, NULL, present_thread, NULL)) { LOG("present thread"); return 0; }
    return 1;
}

void vkp_frame(void) {
    pthread_mutex_lock(&mu);
    frames_in++;
    pthread_cond_signal(&cv);
    pthread_mutex_unlock(&mu);
}

void vkp_resized(void) { atomic_store(&resized, 1); }

void vkp_set_present(int vsync, int mailbox) {
    atomic_store(&want_vsync, vsync);
    atomic_store(&want_mailbox, mailbox);
    atomic_store(&resized, 1);   // the swapchain is rebuilt in the new mode before the next present
}
void vkp_set_render_scale(int percent) {
    if (percent < 1) percent = 1;
    if (percent > 100) percent = 100;
    atomic_store(&want_scale, percent);   // read by the next present; the targets stay window-sized
}
uint64_t vkp_presented(void) { return atomic_load(&presented); }
uint64_t vkp_frames(void) {
    pthread_mutex_lock(&mu);
    uint64_t n = frames_in;
    pthread_mutex_unlock(&mu);
    return n;
}
