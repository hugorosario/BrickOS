#ifndef BRICKOS_OSDD_INPUT_H
#define BRICKOS_OSDD_INPUT_H

#include "panel.h"

#include <linux/input.h>
#include <stddef.h>

// The official daemon's signal to trimui_inputd while it holds the controller.
#define OSDD_INPUTD_GRAB_PATH "/tmp/trimui_inputd/grab"

struct osdd_input {
    int fd;
    int grabbed;
    const char *grab_path; // Created while grabbed; NULL leaves no file.
    unsigned pressed;      // A and B presses awaiting their release.
};

void osdd_input_init(struct osdd_input *input);
// Opens the verified TRIMUI Player1 node; an initialized input keeps its grab_path.
int osdd_input_open(struct osdd_input *input, char *error, size_t error_size);
// Forgets A and B presses, so a key already down when the panel opens does not act.
void osdd_input_reset_keys(struct osdd_input *input);
int osdd_input_decode(struct osdd_input *input, const struct input_event *event,
                      enum osdd_panel_button *button);
// Returns 0 with the decoded buttons; when the node has disappeared it is closed and fd
// becomes -1, so the caller reopens it with osdd_input_open.
int osdd_input_poll(struct osdd_input *input, enum osdd_panel_button *buttons,
                    size_t capacity, size_t *count, char *error, size_t error_size);
// Takes or releases the controller and creates or removes the inputd grab file.
int osdd_input_grab(struct osdd_input *input, int enabled,
                    char *error, size_t error_size);
void osdd_input_close(struct osdd_input *input);

#endif
