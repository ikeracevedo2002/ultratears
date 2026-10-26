// Window presentation through Vulkan: the swapchain and a present thread. The replayer (src/gfx/backend_vk.c) copies
// each frame the game presents into one of three output images; the present thread shows the newest one, scaled to
// the window with the game's aspect ratio. Until the first game frame, a colour cycling per frame shows that frames
// arrive.
#pragma once
#include <stdint.h>

#include "vk.h"

typedef struct SDL_Window SDL_Window;

int vkp_init(SDL_Window *window);   // 1 = ready; on failure logs why and returns 0 (the game runs without a picture)
void vkp_frame(void);               // the game presented a frame without a picture (null device): count it, cycle colour
void vkp_resized(void);             // the window changed size: the swapchain is rebuilt before the next present
uint64_t vkp_presented(void);       // frames shown so far
uint64_t vkp_frames(void);          // frames the game presented so far
int vkp_active(void);               // a window with a swapchain is there (else the replayer skips the copy)

// Output images (replay thread): acquire one of width x height (B8G8R8A8_UNORM, created / resized here), record the
// copy of the game's frame into it (layout UNDEFINED -> TRANSFER_DST -> TRANSFER_SRC), then hand it over with the
// timeline value after which the copy is done. -1 = none free (the frame is dropped).
int vkp_out_acquire(uint32_t width, uint32_t height, VkImage *image);
void vkp_out_ready(int slot, VkSemaphore timeline, uint64_t value, const int crop[4]);

// Settings overlay (present thread): BGRA panel drawn centred over the next presented frame. NULL hides it. The pixels
// are copied at call time, so the caller may reuse its buffer straight away.
#define VKP_OVERLAY_MAX_W 640
#define VKP_OVERLAY_MAX_H 480
void vkp_overlay(const uint32_t *bgra, int w, int h);
// Present mode for the next frame: vsync on = FIFO, vsync off with mailbox = MAILBOX, else IMMEDIATE when the surface has it.
void vkp_set_present(int vsync, int mailbox);
// Render scale in percent (1..100) for the EASU/RCAS upscale: the passes run at this share of the window's fitted size,
// then the blit stretches them to it. The linear fallback ignores it.
void vkp_set_render_scale(int percent);
