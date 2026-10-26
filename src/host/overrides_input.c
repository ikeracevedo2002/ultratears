/* nn::hid: one full-key controller (docked) or the handheld controller, fed by the keyboard and game controllers.
 * Ported from MacNativeGames games/totk/runtime/input.c @b75be8c (MIT); state ABI follows libnx HID. Changes: key codes
 * stay MacNativeGames' (Mac virtual key codes) and the window layer translates its own keys into them; the game
 * controller source is runtime_gamepad_read (none until the window / SDL3 layer of milestone 3.2); the diagnostic input
 * script / file commands are not ported. */
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "host.h"
#include "../runtime/hle.h"

typedef struct { int64_t sampling; uint64_t buttons; int32_t lx, ly, rx, ry; uint32_t attributes, pad; } NpadState;
_Static_assert(sizeof(NpadState) == 40, "Npad full-key ABI");

int runtime_handheld_mode(void);

static pthread_mutex_t input_lock = PTHREAD_MUTEX_INITIALIZER;
static unsigned char keys[128];
static uint64_t pending;
static uint32_t styles = 1;
static bool enabled, allowed = true;
static int64_t sample;

/* Mac virtual key codes -> Switch buttons (MacNativeGames' layout: J=A, K/Space=B, I=X, U=Y, Z/C=L/R ...). */
static uint64_t button(unsigned code) {
    switch (code) {
    case 38: return 1; case 40: case 49: return 2; case 34: return 4; case 32: return 8; case 6: return 16; case 8: return 32;
    case 12: return 64; case 14: return 128; case 58: return 256; case 56: return 512; case 36: return 1024;
    case 48: case 53: return 2048; case 3: return 4096; case 17: return 8192; case 4: return 16384; case 5: return 32768;
    default: return 0;
    }
}

/* Game controllers: filled by the window layer (SDL3, milestone 3.2). */
static RuntimeGamepadState g_pad;
void runtime_gamepad_set(const RuntimeGamepadState *s) { pthread_mutex_lock(&input_lock); g_pad = *s; pthread_mutex_unlock(&input_lock); }
static void gamepad_read(RuntimeGamepadState *out) { *out = g_pad; }

void runtime_input_key(unsigned code, int down) {
    if (code >= 128) return;
    pthread_mutex_lock(&input_lock);
    if (down && !keys[code]) pending |= button(code);
    keys[code] = (unsigned char)!!down;
    pthread_mutex_unlock(&input_lock);
}
void runtime_input_clear(void) {
    pthread_mutex_lock(&input_lock);
    memset(keys, 0, sizeof keys);
    pending = 0;
    memset(&g_pad, 0, sizeof g_pad);
    pthread_mutex_unlock(&input_lock);
}

/* TOTK_INPUT_SCRIPT=<frame>:<key>[*<frames held>],... (development: drive the game without a person, e.g.
   "520:J,700:J*30,900:W*120"). Frames are the graphics frontend's frames; keys are named as on the keyboard
   (A-Z, RETURN, SPACE, TAB, ESCAPE, UP, DOWN, LEFT, RIGHT, LSHIFT, LALT) and go through the same layout as typing.
   An entry starting with t is in seconds of real time since the first frame instead: "t70:J,t95:W*2.5" presses J at
   70 s and holds W from 95 s for 2.5 s, the same wall-clock input at any frame rate (for game-speed checks). */
static int script_code(const char *k, size_t n) {
    static const struct { const char *name; int code; } named[] = {
        {"RETURN", 36}, {"SPACE", 49}, {"TAB", 48}, {"ESCAPE", 53}, {"UP", 126}, {"DOWN", 125}, {"LEFT", 123},
        {"RIGHT", 124}, {"LSHIFT", 56}, {"LALT", 58}};
    static const int letters[26] = {0, 11, 8, 2, 14, 3, 5, 4, 34, 38, 40, 37, 46, 45, 31, 35, 12, 15, 1, 17, 32, 9, 13, 7, 16, 6};
    if (n == 1 && k[0] >= 'A' && k[0] <= 'Z') return letters[k[0] - 'A'];
    for (unsigned i = 0; i < sizeof named / sizeof *named; i++)
        if (strlen(named[i].name) == n && !strncmp(named[i].name, k, n)) return named[i].code;
    return -1;
}
void runtime_input_script_tick(uint64_t frame) {
    static const char *script;
    static int init;
    static uint64_t t0, prev_ns;
    if (!init) { init = 1; script = getenv("TOTK_INPUT_SCRIPT"); t0 = plat_now_ns(); }
    uint64_t now_ns = plat_now_ns() - t0;
    for (const char *p = script; p && *p;) {
        char *end;
        if (*p == 't') {   // real-time entry: press when the clock passes its time, release after its hold in seconds
            double at_s = strtod(p + 1, &end);
            const char *k = *end == ':' ? end + 1 : end;
            size_t n = strcspn(k, "*,");
            double hold_s = k[n] == '*' ? strtod(k + n + 1, NULL) : 0.2;
            uint64_t on = (uint64_t)(at_s * 1e9), off = (uint64_t)((at_s + hold_s) * 1e9);
            int code = script_code(k, n);
            if (code >= 0 && prev_ns < on && now_ns >= on) { fprintf(stderr, "[totk] input script: %.3f s (frame %llu) press %.*s\n", now_ns / 1e9, (unsigned long long)frame, (int)n, k); runtime_input_key((unsigned)code, 1); }
            if (code >= 0 && prev_ns < off && now_ns >= off) { fprintf(stderr, "[totk] input script: %.3f s (frame %llu) release %.*s\n", now_ns / 1e9, (unsigned long long)frame, (int)n, k); runtime_input_key((unsigned)code, 0); }
            p = strchr(k, ',');
            if (p) p++;
            continue;
        }
        uint64_t at = strtoull(p, &end, 10);
        const char *k = *end == ':' ? end + 1 : end;
        size_t n = strcspn(k, "*,");
        uint64_t hold = k[n] == '*' ? strtoull(k + n + 1, NULL, 10) : 6;
        int code = script_code(k, n);
        if (code >= 0 && frame == at) { fprintf(stderr, "[totk] input script: frame %llu press %.*s\n", (unsigned long long)frame, (int)n, k); runtime_input_key((unsigned)code, 1); }
        if (code >= 0 && frame == at + hold) runtime_input_key((unsigned)code, 0);
        p = strchr(k, ',');
        if (p) p++;
    }
    prev_ns = now_ns;
}

static void initialize(void) {
    pthread_mutex_lock(&input_lock); enabled = true; pthread_mutex_unlock(&input_lock);
    fprintf(stderr, "[totk] keyboard / game controller input initialized\n");
}
/* Docked: one full-key controller as player 1 (id 0, style bit 0). Handheld (TOTK_OPERATION_MODE=handheld): the same
   input is the handheld controller (id 0x20, style bit 1) and player 1 is not connected. */
static uint32_t pad_id(void) { return runtime_handheld_mode() ? 0x20u : 0u; }
static uint32_t pad_style(void) { return runtime_handheld_mode() ? 2u : 1u; }
static void supported_ids(const uint32_t *ids, uint64_t n) {
    if (n > 10 || (!ids && n)) runtime_fail("invalid controller ids");
    pthread_mutex_lock(&input_lock);
    allowed = false;
    for (uint64_t i = 0; i < n; i++) if (ids[i] == pad_id()) allowed = true;
    pthread_mutex_unlock(&input_lock);
}
static void supported_styles(uint32_t value) { pthread_mutex_lock(&input_lock); styles = value; pthread_mutex_unlock(&input_lock); }
static uint32_t style(const uint32_t *id) {
    if (!id) runtime_fail("controller id pointer");
    pthread_mutex_lock(&input_lock);
    uint32_t v = enabled && allowed && *id == pad_id() ? (styles & pad_style()) : 0;
    pthread_mutex_unlock(&input_lock);
    return v;
}
static int pad_states(NpadState *out, int max, const uint32_t *id, uint32_t want_style) {
    if (max <= 0) return 0;
    if (!out || !id) runtime_fail("controller output pointer");
    pthread_mutex_lock(&input_lock);
    NpadState state = {.sampling = ++sample};
    if (enabled && allowed && *id == pad_id() && want_style == pad_style() && (styles & want_style)) {
        state.attributes = 1;
        for (unsigned i = 0; i < 128; i++) if (keys[i]) state.buttons |= button(i);
        state.buttons |= pending;
        pending = 0;
        state.lx = 32767 * ((int)keys[2] - (int)keys[0]);       /* D / A */
        state.ly = 32767 * ((int)keys[13] - (int)keys[1]);      /* W / S */
        state.rx = 32767 * ((int)keys[124] - (int)keys[123]);   /* arrow right / left */
        state.ry = 32767 * ((int)keys[126] - (int)keys[125]);   /* arrow up / down */
        RuntimeGamepadState pad;
        gamepad_read(&pad);
        state.buttons |= pad.buttons;
        if (abs(pad.lx) > abs(state.lx)) state.lx = pad.lx;
        if (abs(pad.ly) > abs(state.ly)) state.ly = pad.ly;
        if (abs(pad.rx) > abs(state.rx)) state.rx = pad.rx;
        if (abs(pad.ry) > abs(state.ry)) state.ry = pad.ry;
    }
    *out = state;
    pthread_mutex_unlock(&input_lock);
    return 1;
}
static int full_states(NpadState *out, int max, const uint32_t *id) { return pad_states(out, max, id, 1); }
static int handheld_states(NpadState *out, int max, const uint32_t *id) { return pad_states(out, max, id, 2); }
static int disconnected_states(NpadState *out, int max, const uint32_t *id) {
    (void)id;
    if (max <= 0) return 0;
    if (!out) runtime_fail("controller output pointer");
    pthread_mutex_lock(&input_lock);
    *out = (NpadState){.sampling = ++sample};
    pthread_mutex_unlock(&input_lock);
    return 1;
}
static void hold_type(int type) { if (type < 0 || type > 1) runtime_fail("invalid joy hold type"); }
static void disconnect(const uint32_t *id) {
    if (!id) runtime_fail("disconnect id");
    if (*id == pad_id()) {   /* only the pad in use: in handheld mode TOTK disconnects player 1 */
        pthread_mutex_lock(&input_lock); allowed = false; pthread_mutex_unlock(&input_lock);
        runtime_input_clear();
    }
}

/* Mouse and keyboard devices: the keyboard drives the controller, so the game sees no mouse and a keyboard with no keys. */
typedef struct { int64_t sampling; int32_t x, y, dx, dy, wx, wy; uint32_t buttons, attributes; } MouseState;
typedef struct { int64_t sampling; uint32_t modifiers, attributes; uint64_t keys[4]; } KeyboardState;
_Static_assert(sizeof(MouseState) == 40 && sizeof(KeyboardState) == 48, "mouse/keyboard ABI");
static bool mouse_ready, keyboard_ready;
static void initialize_mouse(void) { pthread_mutex_lock(&input_lock); mouse_ready = true; pthread_mutex_unlock(&input_lock); }
static void initialize_keyboard(void) { pthread_mutex_lock(&input_lock); keyboard_ready = true; pthread_mutex_unlock(&input_lock); }
static int mouse_states(MouseState *out, int max) {
    if (max <= 0) return 0;
    if (!out) runtime_fail("mouse output pointer");
    pthread_mutex_lock(&input_lock);
    if (!mouse_ready) runtime_fail("mouse used before InitializeMouse");
    *out = (MouseState){.sampling = ++sample};
    pthread_mutex_unlock(&input_lock);
    return 1;
}
static void keyboard_state(KeyboardState *out) {
    if (!out) runtime_fail("keyboard output pointer");
    pthread_mutex_lock(&input_lock);
    if (!keyboard_ready) runtime_fail("keyboard used before InitializeKeyboard");
    *out = (KeyboardState){.sampling = ++sample};
    pthread_mutex_unlock(&input_lock);
}
/* Controller support applet: exactly one controller here, so it returns at once (one player, on the controller in use). */
typedef struct { int8_t players; uint8_t pad[3]; uint32_t selected; } ControllerSupportResult;
_Static_assert(sizeof(ControllerSupportResult) == 8, "controller support result ABI");
static uint32_t show_controller_support(ControllerSupportResult *out, const void *arg) {
    (void)arg;
    if (out) *out = (ControllerSupportResult){.players = 1, .selected = pad_id()};
    return 0;
}

HLE_NATIVE("_ZN2nn3hid14InitializeNpadEv", initialize, 'v')
HLE_NATIVE("_ZN2nn3hid22SetSupportedNpadIdTypeEPKjm", supported_ids, 'v')
HLE_NATIVE("_ZN2nn3hid24SetSupportedNpadStyleSetENS_4util10BitFlagSetILi32ENS0_12NpadStyleTagEEE", supported_styles, 'v')
HLE_NATIVE("_ZN2nn3hid15GetNpadStyleSetERKj", style, 'w')
HLE_NATIVE("_ZN2nn3hid13GetNpadStatesEPNS0_16NpadFullKeyStateEiRKj", full_states, 'w')
HLE_NATIVE("_ZN2nn3hid13GetNpadStatesEPNS0_16NpadJoyDualStateEiRKj", disconnected_states, 'w')
HLE_NATIVE("_ZN2nn3hid13GetNpadStatesEPNS0_16NpadJoyLeftStateEiRKj", disconnected_states, 'w')
HLE_NATIVE("_ZN2nn3hid13GetNpadStatesEPNS0_17NpadJoyRightStateEiRKj", disconnected_states, 'w')
HLE_NATIVE("_ZN2nn3hid13GetNpadStatesEPNS0_17NpadHandheldStateEiRKj", handheld_states, 'w')
HLE_NATIVE("_ZN2nn3hid18SetNpadJoyHoldTypeENS0_15NpadJoyHoldTypeE", hold_type, 'v')
HLE_NATIVE("_ZN2nn3hid14DisconnectNpadERKj", disconnect, 'v')
HLE_NATIVE("_ZN2nn3hid21ShowControllerSupportEPNS0_27ControllerSupportResultInfoERKNS0_20ControllerSupportArgE", show_controller_support, 'w')
HLE_NATIVE("_ZN2nn3hid15InitializeMouseEv", initialize_mouse, 'v')
HLE_NATIVE("_ZN2nn3hid18InitializeKeyboardEv", initialize_keyboard, 'v')
HLE_NATIVE("_ZN2nn3hid14GetMouseStatesEPNS0_10MouseStateEi", mouse_states, 'w')
HLE_NATIVE("_ZN2nn3hid16GetKeyboardStateEPNS0_13KeyboardStateE", keyboard_state, 'v')
