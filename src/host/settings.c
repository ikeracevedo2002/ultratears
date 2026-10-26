// settings.cfg: "name=value" lines like ultratears.cfg (boot.c). The game's folder is the working directory by then.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "settings.h"

#define SETTINGS_FILE "settings.cfg"

static void defaults(settings_t *s) {
    s->width = 1280;
    s->height = 720;
    s->fullscreen = 0;
    s->vsync = 0;
    s->mailbox = 1;
}

void settings_load(settings_t *s) {
    defaults(s);
    FILE *f = fopen(SETTINGS_FILE, "r");
    if (!f) return;
    char line[256];
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        char *eq = strchr(line, '=');
        if (!eq || line[0] == '#') continue;
        *eq = 0;
        long v = strtol(eq + 1, NULL, 10);
        if (!strcmp(line, "width") && v > 0) s->width = (int)v;
        else if (!strcmp(line, "height") && v > 0) s->height = (int)v;
        else if (!strcmp(line, "fullscreen")) s->fullscreen = v != 0;
        else if (!strcmp(line, "vsync")) s->vsync = v != 0;
        else if (!strcmp(line, "mailbox")) s->mailbox = v != 0;
    }
    fclose(f);
}

void settings_save(const settings_t *s) {
    FILE *f = fopen(SETTINGS_FILE, "w");
    if (!f) {
        fprintf(stderr, "[totk] cannot write %s\n", SETTINGS_FILE);
        return;
    }
    fprintf(f, "# UltraTears graphics settings (F1 in the game)\n");
    fprintf(f, "width=%d\nheight=%d\n", s->width, s->height);
    fprintf(f, "fullscreen=%d\nvsync=%d\nmailbox=%d\n", s->fullscreen, s->vsync, s->mailbox);
    fclose(f);
}
