/* Approximate real-time frame step for the 60 fps controller (TOTK_FPS60=1). This is not MacNativeGames'
   game_patches_v143.c: that file is not on this PC or in the repository, so nothing here is a port. It only paces the
   presents: a present that arrives before its 1/60 s slot is held until the slot, so the game's frame clock stays near
   60 Hz. The frame deltas are clamped to 1/120..1/15 s and averaged over 5 presents for the log only. Off unless
   TOTK_FPS60=1.
   TOTK_PACE_FPS=<n>: slot rate for the pacer (default 60; 144 for the 144 fps test).
   TOTK_FRAMELOG=<file>: one line per present with the present's monotonic time in ns, for the frame-time percentiles. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include "host.h"
#include "../runtime/platform.h"

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
        LOG("[frame-step] approximate 1/%llu s pacing %s (TOTK_FPS60=1); frame log %s", (unsigned long long)(1000000000ull / slot_ns), on ? "on" : "off", log ? f : "off");
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
        if (++logged % 600 == 0) LOG("[frame-step] mean frame delta %.2f ms over %u presents", (double)sum / k / 1e6, k);
    }
    prev = now;

    /* Hold an early present until its slot. A present more than one slot late restarts the schedule, so the
       pacing never catches up with a burst of frames. */
    slot = slot ? slot + slot_ns : now + slot_ns;
    if (slot + slot_ns < now) slot = now;
    if (now < slot) plat_sleep_until_ns(slot);
}
