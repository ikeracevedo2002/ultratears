"""Per-instruction replacements applied by gen.py: guest address -> function(gen, addr) returning C code.

Every patch checks the original instruction word first, so lifting another game version (or a different build)
fails loudly instead of changing the wrong code. Runtime switches live in src/gfx/nvn/hle_nvn.c.
"""


def expect(gen, addr, word, what):
    got = gen.word(addr)
    if got != word:
        raise SystemExit(f'patch {what} at {addr:#x}: expected {word:#010x}, found {got:#010x} (not TOTK 1.4.3?)')


def drs_off(gen, addr):
    """Dynamic resolution off (1.4.3). main+0x584300 is the scene resolution controller: each frame it fits the
    recent frame times and returns {scale (float), state} to the renderer (main+0x581968), which turns the scale into
    the scene size (+0xccc). Its first test is the controller's enable bit (+0x110 bit 0); with the bit clear it
    takes its own disabled path (main+0x584c3c): scale 1.0, history untouched. The patch takes that path unless
    TOTK_DRS=1. Why: the game flips the scene targets between 1600x896 and 1432x808 over the same memory and our
    per-size images then hand stale content to the other size (terrain gone, clouds over the scene);
    image quality is better at a fixed size and upscaling will come from FSR."""
    expect(gen, addr, 0x360042e8, 'drs_off')   # tbz w8, #0, 0x800584c3c
    return ('{ extern int totk_drs_enabled(void); '
            'if (!totk_drs_enabled() || ((c->x[8] >> 0) & 1) == 0) { goto L_800584c3c; } }')


def frame_dt(gen, addr):
    """Real-time frame step (1.4.3). main+0x3e67f0 sets the game's time step: s0 is the frame interval in 60 Hz ticks
    (2.0 at 30 fps; its caller main+0x3e55e0 makes it from 60 / the frame rate at +0x250), x0 the timing object. Its
    first use is here (fmul s1, s0, s2 with s2 = 0.5: the step in 30 Hz frames, stored to +0x10). totk_frame_dt
    (src/host/frame_step.c) puts the real elapsed time in 60 Hz ticks into s0 when TOTK_FPS60 or TOTK_PACE_FPS is set,
    so game time follows the wall clock at any frame rate; otherwise it only counts the step for the frame-time dump.
    The same hook site as flabanabba8/totk-60fps-1.4.3 (dynamic/timing.s), written from scratch here."""
    expect(gen, addr, 0x1e220801, 'frame_dt')   # fmul s1, s0, s2
    return '{ extern void totk_frame_dt(Cpu *c); totk_frame_dt(c); } fp_op_s(c, FOP_MUL, 1, 0, 2);'


def scene_scale(gen, addr):
    """Scene resolution scale (1.4.3). On the controller's disabled path (main+0x584c3c, see drs_off) the scale it
    returns is this constant 1.0 (mov w23, #1.0). totk_scene_scale_bits (src/gfx/nvn/hle_nvn.c) returns render_scale
    (settings.cfg / F1) or TOTK_SCENE_SCALE as float bits instead (default 1.0): the renderer (main+0x581a0c) turns it
    into the scene size at +0xccc."""
    expect(gen, addr, 0x52a7f017, 'scene_scale')   # mov w23, #0x3f800000
    return '{ extern uint32_t totk_scene_scale_bits(void); c->x[23] = (uint32_t)totk_scene_scale_bits(); }'


PATCHES = {
    0x8003e6818: frame_dt,
    0x800584c44: scene_scale,
    0x8005843e0: drs_off,
}
