/* Approximate real-time frame step for the 60 fps controller (TOTK_FPS60=1). This is not MacNativeGames'
   game_patches_v143.c: that file is not on this PC or in the repository, so nothing here is a port. It only paces the
   presents: a present that arrives before its 1/60 s slot is held until the slot, so the game's frame clock stays near
   60 Hz. The frame deltas are clamped to 1/120..1/15 s and averaged over 5 presents for the log only. Off unless
   TOTK_FPS60=1.
   TOTK_PACE_FPS=<n>: slot rate for the pacer (default 60; 144 for the 144 fps test).
   TOTK_FRAMELOG=<file>: one line per present with the present's monotonic time in ns, for the frame-time percentiles. */
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "host.h"
#include "../runtime/platform.h"

/* Real-time frame step (tools/lifter/patches.py, frame_dt): called at main+0x3e6818 with s0 = the game's frame interval
   in 60 Hz ticks and x0 = the timing object. With TOTK_FPS60 or TOTK_PACE_FPS set (TOTK_REALTIME_STEP=0/1 overrides),
   s0 becomes the real time since that object's previous step, in 60 Hz ticks, clamped to 1/360..1/20 s: the game then
   runs at 1x at any frame rate (below 20 fps it slows down, as on the console). The first step of an object, and one
   after a pause longer than 1/4 s (loading, a breakpoint), keeps the game's own interval. Off: s0 is left as is.
   Both values are summed for the per-frame dump (DM_NVN_TSV: @step_in_ticks, @step_ticks, @step_calls). */
#define STEP_MIN_NS (1000000000ull / 360)
#define STEP_MAX_NS (1000000000ull / 20)
#define STEP_RESET_NS (1000000000ull / 4)
#define STEP_OBJS 4

static _Atomic uint64_t step_in_uticks, step_out_uticks, step_calls;   // micro-ticks (1e-6 of a 60 Hz tick)

static int realtime_step_on(void) {
    static int on = -1;
    if (on < 0) {
        const char *r = getenv("TOTK_REALTIME_STEP"), *f = getenv("TOTK_FPS60"), *p = getenv("TOTK_PACE_FPS");
        on = r && *r ? atoi(r) > 0 : (f && atoi(f) > 0) || (p && atoi(p) > 0);
        LOG("[frame-step] real-time game step %s (TOTK_FPS60 / TOTK_PACE_FPS; TOTK_REALTIME_STEP=0/1 overrides)\n", on ? "on" : "off");
    }
    return on;
}

void totk_frame_dt(Cpu *c) {
    float in;
    memcpy(&in, &c->v[0], sizeof in);
    float out = in;
    if (realtime_step_on()) {
        static struct { uint64_t obj, last; } objs[STEP_OBJS];
        uint64_t now = plat_now_ns();
        unsigned i = 0;
        while (i < STEP_OBJS && objs[i].obj && objs[i].obj != c->x[0]) i++;
        if (i == STEP_OBJS) { i = 0; memset(objs, 0, sizeof objs); }   // more objects than expected: start over
        if (objs[i].obj == c->x[0] && now - objs[i].last <= STEP_RESET_NS) {
            uint64_t dt = now - objs[i].last;
            if (dt < STEP_MIN_NS) dt = STEP_MIN_NS;
            if (dt > STEP_MAX_NS) dt = STEP_MAX_NS;
            out = (float)((double)dt * 60.0 / 1e9);
            memcpy(&c->v[0], &out, sizeof out);
        }
        objs[i].obj = c->x[0];
        objs[i].last = now;
    }
    atomic_fetch_add(&step_in_uticks, (uint64_t)(in > 0 ? in * 1e6 : 0));
    atomic_fetch_add(&step_out_uticks, (uint64_t)(out > 0 ? out * 1e6 : 0));
    atomic_fetch_add(&step_calls, 1);
}

/* The steps since the previous call, for the per-frame dump: the game's own interval and the one it used, in ticks. */
void totk_frame_dt_take(double *in_ticks, double *out_ticks, unsigned *calls) {
    *in_ticks = (double)atomic_exchange(&step_in_uticks, 0) / 1e6;
    *out_ticks = (double)atomic_exchange(&step_out_uticks, 0) / 1e6;
    *calls = (unsigned)atomic_exchange(&step_calls, 0);
}

#define DT_MIN_NS (1000000000ull / 120)
#define DT_MAX_NS (1000000000ull / 15)
#define AVG_N 5

void runtime_frame_step_update(uint64_t now) {
    static int on = -1;
    static FILE *log;
    static uint64_t prev, slot, slot_ns;
    static uint64_t dts[AVG_N];
    static unsigned n;
    if (on < 0) {
        const char *e = getenv("TOTK_FPS60"), *f = getenv("TOTK_FRAMELOG"), *p = getenv("TOTK_PACE_FPS");
        on = e && atoi(e) > 0;
        slot_ns = 1000000000ull / (p && atoi(p) > 0 ? (unsigned)atoi(p) : 60);
        if (f && *f) log = fopen(f, "w");
        LOG("[frame-step] approximate 1/%llu s pacing %s (TOTK_FPS60=1); frame log %s\n", (unsigned long long)(1000000000ull / slot_ns), on ? "on" : "off", log ? f : "off");
    }
    if (log) { fprintf(log, "%llu\n", (unsigned long long)now); fflush(log); }
    if (!on) return;

    if (prev) {
        uint64_t dt = now - prev;
        if (dt < DT_MIN_NS) dt = DT_MIN_NS;
        if (dt > DT_MAX_NS) dt = DT_MAX_NS;
        dts[n++ % AVG_N] = dt;
        uint64_t sum = 0;
        unsigned k = n < AVG_N ? n : AVG_N;
        for (unsigned i = 0; i < k; i++) sum += dts[i];
        static unsigned logged;
        if (++logged % 600 == 0) LOG("[frame-step] mean frame delta %.2f ms over %u presents\n", (double)sum / k / 1e6, k);
    }
    prev = now;

    /* Hold an early present until its slot. A present more than one slot late restarts the schedule, so the
       pacing never catches up with a burst of frames. */
    slot = slot ? slot + slot_ns : now + slot_ns;
    if (slot + slot_ns < now) slot = now;
    if (now < slot) plat_sleep_until_ns(slot);
}
