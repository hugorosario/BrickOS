#ifndef BRICKOS_OSDD_DISPLAY_H
#define BRICKOS_OSDD_DISPLAY_H

#include <linux/fb.h>
#include <stddef.h>
#include <stdint.h>

#define OSDD_WIDTH 1024
#define OSDD_HEIGHT 768
#define OSDD_STRIDE 4096
#define OSDD_FRAME_BYTES (OSDD_STRIDE * OSDD_HEIGHT)
#define OSDD_SLOT_BYTES 0x400000
#define OSDD_PANEL_SLOT0_OFFSET 0x600000
#define OSDD_TOAST_SLOT0_OFFSET 0xa00000
#define OSDD_TOAST_SLOT1_OFFSET 0xe00000
#define OSDD_PANEL_SLOT1_OFFSET 0x1200000
#define OSDD_LAYER_BYTES 256
// While its layer is open on channel 2, GameSwitcher keeps its pid in this file. The OSD then
// leaves channel 2 alone: it sets, fades and hides nothing there, and keeps its panel waiting.
#define OSDD_GAMESWITCHER_DISPLAY_MARKER "/tmp/brickos-gameswitcher-display"

struct osdd_display_profile {
    uint64_t physical_base;
    size_t map_bytes;
    unsigned char saved[2][OSDD_LAYER_BYTES];
    // No OSD has run since boot: channels 2 and 3 read as all zeros and channel 1 still
    // has its boot z of 16 and alpha mode 0. saved then holds the stock idle layers.
    int first_osd;
    // An earlier OSD ended without hiding its layers: channel 2 or 3 is shown, or points at
    // its second slot. saved then holds the stock idle layers, which the open sets, as the
    // official daemon does when it starts over that state.
    int left_shown;
};

struct osdd_display {
    int fb_fd;
    int disp_fd;
    unsigned char *mapping;
    struct osdd_display_profile profile;
    int front_slot;
    int visible;
    uint64_t last_publish_ms;
    int panel_front_slot;
    int panel_visible;
    uint64_t panel_last_publish_ms;
    unsigned set_retries; // SETs repeated because the read-back did not match.
    int initialized;
};

int osdd_display_validate(const struct fb_var_screeninfo *variable,
                          const struct fb_fix_screeninfo *fixed,
                          const unsigned char layers[3][OSDD_LAYER_BYTES],
                          struct osdd_display_profile *profile,
                          char *error, size_t error_size);
void osdd_display_layer_config(unsigned char config[OSDD_LAYER_BYTES],
                               uint64_t physical_address, unsigned char alpha);
void osdd_display_layer_config_channel(unsigned char config[OSDD_LAYER_BYTES],
                                       uint32_t channel, uint64_t physical_address,
                                       unsigned char alpha);
// Channel 1 as the official daemon leaves it when it is the first OSD after boot: the
// boot configuration with z 4 and alpha mode 2. MainUI then sets its own alpha mode.
void osdd_display_frontend_first_osd(const unsigned char boot[OSDD_LAYER_BYTES],
                                     unsigned char config[OSDD_LAYER_BYTES]);
int osdd_display_open(struct osdd_display *display, char *error, size_t error_size);
int osdd_display_stage(struct osdd_display *display, const void *frame,
                       size_t frame_bytes, uint64_t now_ms, size_t *offset,
                       char *error, size_t error_size);
int osdd_display_present(struct osdd_display *display, const void *frame,
                         size_t frame_bytes, uint64_t now_ms,
                         char *error, size_t error_size);
int osdd_display_hide(struct osdd_display *display, char *error, size_t error_size);
int osdd_display_stage_panel(struct osdd_display *display, const void *frame,
                             size_t frame_bytes, uint64_t now_ms, size_t *offset,
                             char *error, size_t error_size);
int osdd_display_present_panel(struct osdd_display *display, const void *frame,
                               size_t frame_bytes, uint64_t now_ms, unsigned char alpha,
                               char *error, size_t error_size);
// Changes the visible panel slot's global alpha without staging a new frame.
int osdd_display_panel_alpha(struct osdd_display *display, unsigned char alpha,
                             char *error, size_t error_size);
int osdd_display_hide_panel(struct osdd_display *display, char *error, size_t error_size);
// Leaves channel 2 as it is while GameSwitcher's marker is live.
void osdd_display_close(struct osdd_display *display);
// 1 when the marker at path names a running GameSwitcher process, known by its executable's
// name; 0 when the file is absent or invalid, or names a process that is gone or another program.
int osdd_display_gameswitcher_marker(const char *path);
// The marker osdd_display_close checks; tests point it elsewhere.
extern const char *osdd_display_marker_path;

#endif
