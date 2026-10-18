#ifndef BRICKOS_MAINUI_DEVICE_H
#define BRICKOS_MAINUI_DEVICE_H

#include "effects.h"

// The device side of the effect adapter: keymon's shared memory, sysfs files, the LCD
// brightness ioctl, the mixer through libtinyalsa (no tinymix subprocess, so no inherited
// library path is involved), and shell commands run with the target library path first.

struct mixer;

struct mainui_device {
    int *keymon;         // keymon's 23 integers, or NULL when keymon is not running
    int disp_fd;         // /dev/disp, or -1
    struct mixer *mixer; // card 0, or NULL
};

// Opens what is available; a missing part makes its writes fail. Returns 0.
int mainui_device_open(struct mainui_device *device);
void mainui_device_close(struct mainui_device *device);
// Fills ops with the device's writers.
void mainui_device_ops(struct mainui_device *device, struct mainui_effect_ops *ops);
// The official start (static: LayerConfig in main, before its window): the menu's layer,
// channel 1 layer 0, gets global alpha 255 again, whatever a child left. Returns 0 or -1.
int mainui_device_ui_layer(struct mainui_device *device);

// The framebuffer page on screen, copied before SDL's window clears it (measured on the Brick:
// SDL_CreateWindow blanks both pages, SDL_Init does not).
struct mainui_screen_copy {
    unsigned char *pixels;  // NULL when nothing was copied
    size_t bytes;
    unsigned int xres, yres, bits, line; // the display mode it was copied in
};
// Copies the page on screen. Returns 0, or -1 with nothing held.
int mainui_device_screen_save(struct mainui_screen_copy *copy);
// Writes the copy into the page now on screen, when the display mode is still the one it was
// copied in, and frees it. Returns 0 when written, else -1.
int mainui_device_screen_restore(struct mainui_screen_copy *copy);

#endif
