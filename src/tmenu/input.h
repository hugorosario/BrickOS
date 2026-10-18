#ifndef BRICKOS_TMENU_INPUT_H
#define BRICKOS_TMENU_INPUT_H

#include "menu.h"

#include <linux/input.h>
#include <stddef.h>

// The official library's controller reader: every evdev node under /dev/input, no
// fixed eventN and no grab, polled for 100 ms, one event read per ready node.
#define TMENU_INPUT_DEVICES 16
#define TMENU_INPUT_POLL_MS 100

struct tmenu_input {
    int fds[TMENU_INPUT_DEVICES];
    size_t count;
};

// The last hat values per device position. The official library keeps them across
// calls, so a hat still held when a call ends suppresses the same value next call.
struct tmenu_hats {
    int x[TMENU_INPUT_DEVICES];
    int y[TMENU_INPUT_DEVICES];
};

// Maps one event from the device at position to a menu key. Sets *stop when the
// library would stop reading the other devices for this poll (an unchanged hat).
enum tmenu_key tmenu_input_decode(struct tmenu_hats *hats, size_t position, const struct input_event *event,
                                  int *stop);

// Opens every node of directory that answers EVIOCGVERSION. Returns the count, or -1
// when the directory cannot be read.
int tmenu_input_open(struct tmenu_input *input, const char *directory);
// Waits up to TMENU_INPUT_POLL_MS for input and returns the first menu key, or
// TMENU_KEY_NONE.
enum tmenu_key tmenu_input_read(struct tmenu_input *input, struct tmenu_hats *hats);
void tmenu_input_close(struct tmenu_input *input);

#endif
