// Graphics settings: settings.cfg in the game's folder (beside ultratears.cfg), edited with the F1 overlay (overlay.c).
// Lines "name=value"; a missing file or key keeps the default.
#pragma once

typedef struct {
    int width, height;   // the window's size when windowed
    int fullscreen;      // 1 = borderless desktop fullscreen (F11 or Alt+Enter too)
    int vsync;           // 1 = wait for the display (FIFO); 0 = present at once
    int mailbox;         // with vsync off: MAILBOX (newest frame, no tearing) when the surface has it, else IMMEDIATE
    int render_scale;    // the game's scene resolution in percent of its own 1600x896: 100 or 120 (1920x1080); TOTK_SCENE_SCALE overrides
} settings_t;

void settings_load(settings_t *s);         // the file in the working directory (the game's folder)
void settings_save(const settings_t *s);   // rewrites the file; a failure is logged and the game goes on
