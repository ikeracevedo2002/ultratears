// The F1 graphics overlay: a panel drawn over the picture (present.c, vkp_overlay) and driven by the keyboard while it is
// open. Its rows change the settings (settings.h); window_sdl.c applies and saves them.
#pragma once
#include <stdint.h>

#include "settings.h"

typedef enum { OVL_UP, OVL_DOWN, OVL_LEFT, OVL_RIGHT, OVL_OK, OVL_BACK } overlay_key_t;
typedef enum { OVL_NOTHING, OVL_CHANGED, OVL_CLOSED, OVL_QUIT } overlay_event_t;

#define OVERLAY_W 520   // the panel's size in pixels; within VKP_OVERLAY_MAX_W x VKP_OVERLAY_MAX_H
#define OVERLAY_H 320

void overlay_open(void);                                       // shows it on the first row
int overlay_is_open(void);
overlay_event_t overlay_key(overlay_key_t k, settings_t *s);   // OVL_CHANGED: s was edited (apply it), OVL_CLOSED: hidden
uint32_t *overlay_render(const settings_t *s, int *w, int *h); // the panel's BGRA pixels for the current state (static buffer)
