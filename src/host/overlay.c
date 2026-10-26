// The overlay's panel: a 5x7 bitmap font (enough for the labels), rows of settings and the keys that change them.
#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "overlay.h"

#define SCALE 2
#define ADVANCE (6 * SCALE)   // a glyph is 5 columns and one space wide
#define ROW_H 36

enum { ROW_RES, ROW_WINDOW, ROW_VSYNC, ROW_MAILBOX, ROW_SCALE, ROW_QUIT, ROWS };

static const int presets[][2] = {{1280, 720}, {1600, 900}, {1920, 1080}, {2560, 1440}, {3840, 2160}};
#define PRESETS ((int)(sizeof presets / sizeof *presets))

static const int scales[] = {100, 120};   // the scene at 1600x896 (the game's own) or 1920x1080
#define SCALES ((int)(sizeof scales / sizeof *scales))

static int shown, row;
static uint32_t px[OVERLAY_W * OVERLAY_H];

// The glyphs: 7 rows of 5 bits, the leftmost pixel is the top bit of each row.
static const char glyph_chars[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789 :-></.";
static const uint8_t glyph_rows[][7] = {
    {0x0E, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11},   // A
    {0x1E, 0x11, 0x11, 0x1E, 0x11, 0x11, 0x1E},   // B
    {0x0E, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0E},   // C
    {0x1E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x1E},   // D
    {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x1F},   // E
    {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x10},   // F
    {0x0E, 0x11, 0x10, 0x17, 0x11, 0x11, 0x0F},   // G
    {0x11, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11},   // H
    {0x0E, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0E},   // I
    {0x07, 0x02, 0x02, 0x02, 0x02, 0x12, 0x0C},   // J
    {0x11, 0x12, 0x14, 0x18, 0x14, 0x12, 0x11},   // K
    {0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1F},   // L
    {0x11, 0x1B, 0x15, 0x15, 0x11, 0x11, 0x11},   // M
    {0x11, 0x11, 0x19, 0x15, 0x13, 0x11, 0x11},   // N
    {0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E},   // O
    {0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10, 0x10},   // P
    {0x0E, 0x11, 0x11, 0x11, 0x15, 0x12, 0x0D},   // Q
    {0x1E, 0x11, 0x11, 0x1E, 0x14, 0x12, 0x11},   // R
    {0x0F, 0x10, 0x10, 0x0E, 0x01, 0x01, 0x1E},   // S
    {0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04},   // T
    {0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E},   // U
    {0x11, 0x11, 0x11, 0x11, 0x11, 0x0A, 0x04},   // V
    {0x11, 0x11, 0x11, 0x15, 0x15, 0x15, 0x0A},   // W
    {0x11, 0x11, 0x0A, 0x04, 0x0A, 0x11, 0x11},   // X
    {0x11, 0x11, 0x11, 0x0A, 0x04, 0x04, 0x04},   // Y
    {0x1F, 0x01, 0x02, 0x04, 0x08, 0x10, 0x1F},   // Z
    {0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E},   // 0
    {0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E},   // 1
    {0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F},   // 2
    {0x1F, 0x02, 0x04, 0x02, 0x01, 0x11, 0x0E},   // 3
    {0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02},   // 4
    {0x1F, 0x10, 0x1E, 0x01, 0x01, 0x11, 0x0E},   // 5
    {0x06, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x0E},   // 6
    {0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08},   // 7
    {0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E},   // 8
    {0x0E, 0x11, 0x11, 0x0F, 0x01, 0x02, 0x0C},   // 9
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},   // space
    {0x00, 0x04, 0x04, 0x00, 0x04, 0x04, 0x00},   // :
    {0x00, 0x00, 0x00, 0x1F, 0x00, 0x00, 0x00},   // -
    {0x10, 0x08, 0x04, 0x02, 0x04, 0x08, 0x10},   // >
    {0x01, 0x02, 0x04, 0x08, 0x04, 0x02, 0x01},   // <
    {0x01, 0x01, 0x02, 0x04, 0x08, 0x10, 0x10},   // /
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x0C, 0x0C},   // .
};

static void fill(int x, int y, int w, int h, uint32_t c) {
    for (int j = y; j < y + h && j < OVERLAY_H; j++)
        for (int i = x; i < x + w && i < OVERLAY_W; i++)
            if (i >= 0 && j >= 0) px[j * OVERLAY_W + i] = c;
}

static void text(int x, int y, const char *s, uint32_t c) {
    for (; *s; s++, x += ADVANCE) {
        const char *p = strchr(glyph_chars, toupper((unsigned char)*s));
        const uint8_t *g = glyph_rows[p ? p - glyph_chars : strchr(glyph_chars, ' ') - glyph_chars];   // unknown: space
        for (int r = 0; r < 7; r++)
            for (int col = 0; col < 5; col++)
                if (g[r] >> (4 - col) & 1) fill(x + col * SCALE, y + r * SCALE, SCALE, SCALE, c);
    }
}

void overlay_open(void) {
    shown = 1;
    row = 0;
}

int overlay_is_open(void) { return shown; }

overlay_event_t overlay_key(overlay_key_t k, settings_t *s) {
    int dir = k == OVL_LEFT ? -1 : 1;
    switch (k) {
    case OVL_UP: if (row > 0) row--; return OVL_NOTHING;
    case OVL_DOWN: if (row < ROWS - 1) row++; return OVL_NOTHING;
    case OVL_BACK: shown = 0; return OVL_CLOSED;
    case OVL_OK: if (row == ROW_QUIT) return OVL_QUIT; break;
    case OVL_LEFT: case OVL_RIGHT: if (row == ROW_QUIT) return OVL_NOTHING; break;
    }
    switch (row) {
    case ROW_RES: {
        int i = 0;
        for (int p = 0; p < PRESETS; p++)
            if (presets[p][0] == s->width && presets[p][1] == s->height) i = p;
        i = (i + dir + PRESETS) % PRESETS;
        s->width = presets[i][0];
        s->height = presets[i][1];
        break;
    }
    case ROW_WINDOW: s->fullscreen = !s->fullscreen; break;
    case ROW_VSYNC: s->vsync = !s->vsync; break;
    case ROW_MAILBOX: s->mailbox = !s->mailbox; break;
    case ROW_SCALE: {
        int i = 0;
        for (int p = 0; p < SCALES; p++)
            if (scales[p] == s->render_scale) i = p;
        i = (i + dir + SCALES) % SCALES;
        s->render_scale = scales[i];
        break;
    }
    }
    return OVL_CHANGED;
}

uint32_t *overlay_render(const settings_t *s, int *w, int *h) {
    char label[48];
    const uint32_t bg = 0xFF202428, edge = 0xFF5A6A7A, sel = 0xFF2F5D8A, fg = 0xFFD0D0D0, dim = 0xFF6B7280;
    *w = OVERLAY_W;
    *h = OVERLAY_H;
    fill(0, 0, OVERLAY_W, OVERLAY_H, bg);
    fill(0, 0, OVERLAY_W, 2, edge);
    fill(0, OVERLAY_H - 2, OVERLAY_W, 2, edge);
    fill(0, 0, 2, OVERLAY_H, edge);
    fill(OVERLAY_W - 2, 0, 2, OVERLAY_H, edge);
    text(20, 16, "GRAPHICS SETTINGS", 0xFFFFFFFF);
    for (int r = 0; r < ROWS; r++) {
        int y = 52 + r * ROW_H;
        uint32_t c = fg;
        if (r == row) {
            fill(20, y, OVERLAY_W - 40, ROW_H - 6, sel);
            c = 0xFFFFFFFF;
        } else if (r == ROW_MAILBOX && s->vsync) {
            c = dim;   // no effect with vsync on
        }
        switch (r) {
        case ROW_RES: snprintf(label, sizeof label, "RESOLUTION  %dX%d", s->width, s->height); break;
        case ROW_WINDOW: snprintf(label, sizeof label, "WINDOW MODE  %s", s->fullscreen ? "FULLSCREEN" : "WINDOWED"); break;
        case ROW_VSYNC: snprintf(label, sizeof label, "VSYNC  %s", s->vsync ? "ON" : "OFF"); break;
        case ROW_MAILBOX: snprintf(label, sizeof label, "MAILBOX  %s", s->mailbox ? "ON" : "OFF"); break;
        case ROW_SCALE: snprintf(label, sizeof label, "RENDER SCALE  %d PERCENT  %s", s->render_scale, s->render_scale == 120 ? "1920X1080" : "1600X896"); break;
        default: snprintf(label, sizeof label, "QUIT TO DESKTOP"); break;
        }
        text(32, y + 8, label, c);
    }
    text(20, 280, "ARROWS CHANGE  F1 CLOSE", 0xFF9AA4AE);
    return px;
}
