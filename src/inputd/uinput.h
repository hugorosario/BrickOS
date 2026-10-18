#ifndef BRICKOS_INPUTD_UINPUT_H
#define BRICKOS_INPUTD_UINPUT_H

#include <stddef.h>

#include "engine.h"

// The stock TRIMUI Player1 controller, set up in the stock ioctl order: EV_KEY, EV_ABS, EV_SW;
// keys 304 305 307 308 310 311 315 314 316 317 318 115 114 59 60; switch 1; axes 16 17 0 1 2
// 3 4 5 with their ranges; bus 3, vendor 0x045e, product 0x028e, version 0x0114.
#define INPUTD_DEVICE_NAME "TRIMUI Player1"

// Returns the uinput descriptor, or -1 with the failing step in error.
int inputd_uinput_create(char *error, size_t size);
// Writes each event followed by its own SYN_REPORT, as the stock daemon does. Returns 0, or
// -1 when a write fails or is short.
int inputd_uinput_write(int fd, const struct inputd_events *events);
void inputd_uinput_destroy(int fd);

// The key, switch, and axis setup in order, for host tests of the declaration.
extern const int inputd_uinput_keys[15];
struct inputd_uinput_axis {
    int code, minimum, maximum;
};
extern const struct inputd_uinput_axis inputd_uinput_axes[8];

#endif
