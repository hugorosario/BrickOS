#ifndef BRICKOS_OSDD_PANEL_RENDER_H
#define BRICKOS_OSDD_PANEL_RENDER_H

#include "panel.h"
#include "render.h"

struct osdd_panel_visual {
    int led_on;
    int motor_on;
    int wifi_on;
    int bluetooth_on;
    int volume;
    int volume_max;
    int backlight;
    int backlight_max;
    const char *cpu_text;
    const char *temperature_text;
    const unsigned char *music_frame; // 540x260 BGRA, or NULL for installed default.
    // The BrickOS look while BrickOS is selected, or NULL for the stock panel.
    const struct osdd_look *look;
};

int osdd_renderer_panel(struct osdd_renderer *renderer,
                        const struct osdd_panel_state *state,
                        const struct osdd_panel_visual *values,
                        const char *asset_root,
                        char *error, size_t error_size);
// The music tile with its selection frame: the only pixels a canvas change affects.
SDL_Rect osdd_panel_music_rect(void);

#endif
