#ifndef BRICKOS_OSDD_POWER_H
#define BRICKOS_OSDD_POWER_H

#include "panel.h"

#define OSDD_POWER_DIAGNOSTIC_BYTES 160

enum osdd_power_phase {
    OSDD_POWER_CLOSED,
    OSDD_POWER_CONFIRM,
    OSDD_POWER_PROGRESS
};

// The confirmation's actions, left to right; Power off is focused when it opens.
enum osdd_power_focus {
    OSDD_POWER_OFF,
    OSDD_POWER_REBOOT,
    OSDD_POWER_CANCEL
};

// What the progress phase runs. A flag request is always a power off.
enum osdd_power_action {
    OSDD_POWER_ACTION_OFF,
    OSDD_POWER_ACTION_REBOOT
};

struct osdd_power_dialog {
    enum osdd_power_phase phase;
    enum osdd_power_focus focus;
    enum osdd_power_action action;
    int flag_source;
    int command_started;
    int command_dispatched;
    int command_finished;
    int command_failed;
    char diagnostic[OSDD_POWER_DIAGNOSTIC_BYTES];
};

void osdd_power_init(struct osdd_power_dialog *dialog);
int osdd_power_visible(const struct osdd_power_dialog *dialog);
// A flag request goes directly to progress; a physical hold opens confirmation.
int osdd_power_hold_request(struct osdd_power_dialog *dialog);
int osdd_power_flag_request(struct osdd_power_dialog *dialog);
// Hide and B close confirmation only. Progress remains until the system shuts down.
int osdd_power_hide_request(struct osdd_power_dialog *dialog);
// Left and right step the focus and stop at either end; up and down do nothing. Returns 1 when A
// chooses Power off or Reboot and the caller must start the worker for the dialog's action, 2 for
// a focus change, -1 when the dialog closes, or 0 when nothing changed.
int osdd_power_input(struct osdd_power_dialog *dialog, enum osdd_panel_button button);
void osdd_power_worker_dispatch(struct osdd_power_dialog *dialog);
void osdd_power_worker_result(struct osdd_power_dialog *dialog, int result,
                              const char *diagnostic);

#endif
