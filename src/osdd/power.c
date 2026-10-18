#include "power.h"

#include <stdio.h>
#include <string.h>

void osdd_power_init(struct osdd_power_dialog *dialog) {
    if (dialog == NULL) return;
    memset(dialog, 0, sizeof(*dialog));
    dialog->phase = OSDD_POWER_CLOSED;
    dialog->focus = OSDD_POWER_OFF;
    dialog->action = OSDD_POWER_ACTION_OFF;
}

int osdd_power_visible(const struct osdd_power_dialog *dialog) {
    return dialog != NULL && dialog->phase != OSDD_POWER_CLOSED;
}

int osdd_power_hold_request(struct osdd_power_dialog *dialog) {
    if (dialog == NULL || dialog->phase != OSDD_POWER_CLOSED) return 0;
    dialog->phase = OSDD_POWER_CONFIRM;
    dialog->focus = OSDD_POWER_OFF;
    dialog->action = OSDD_POWER_ACTION_OFF;
    dialog->flag_source = 0;
    dialog->command_started = 0;
    dialog->command_dispatched = 0;
    dialog->command_finished = 0;
    dialog->command_failed = 0;
    dialog->diagnostic[0] = 0;
    return 1;
}

int osdd_power_flag_request(struct osdd_power_dialog *dialog) {
    if (dialog == NULL) return 0;
    if (dialog->phase == OSDD_POWER_PROGRESS) {
        if (!dialog->command_started || dialog->command_dispatched) return 0;
        // The service that wrote the flag is powering off: a choice not yet run, Reboot
        // included, becomes that power off.
        dialog->flag_source = 1;
        dialog->action = OSDD_POWER_ACTION_OFF;
        dialog->command_started = 0;
        return 1;
    }
    dialog->phase = OSDD_POWER_PROGRESS;
    dialog->focus = OSDD_POWER_OFF;
    dialog->action = OSDD_POWER_ACTION_OFF;
    dialog->flag_source = 1;
    dialog->command_started = 0;
    dialog->command_dispatched = 0;
    dialog->command_finished = 0;
    dialog->command_failed = 0;
    dialog->diagnostic[0] = 0;
    return 1;
}

void osdd_power_worker_dispatch(struct osdd_power_dialog *dialog) {
    if (dialog != NULL && dialog->phase == OSDD_POWER_PROGRESS && dialog->command_started)
        dialog->command_dispatched = 1;
}

int osdd_power_hide_request(struct osdd_power_dialog *dialog) {
    if (dialog == NULL || dialog->phase != OSDD_POWER_CONFIRM) return 0;
    dialog->phase = OSDD_POWER_CLOSED;
    return 1;
}

int osdd_power_input(struct osdd_power_dialog *dialog, enum osdd_panel_button button) {
    if (dialog == NULL || dialog->phase != OSDD_POWER_CONFIRM) return 0;
    if (button == OSDD_PANEL_LEFT) {
        if (dialog->focus == OSDD_POWER_OFF) return 0;
        dialog->focus = dialog->focus == OSDD_POWER_CANCEL ? OSDD_POWER_REBOOT : OSDD_POWER_OFF;
        return 2;
    }
    if (button == OSDD_PANEL_RIGHT) {
        if (dialog->focus == OSDD_POWER_CANCEL) return 0;
        dialog->focus = dialog->focus == OSDD_POWER_OFF ? OSDD_POWER_REBOOT : OSDD_POWER_CANCEL;
        return 2;
    }
    if (button == OSDD_PANEL_B) return osdd_power_hide_request(dialog) ? -1 : 0;
    if (button != OSDD_PANEL_A) return 0;
    if (dialog->focus == OSDD_POWER_CANCEL)
        return osdd_power_hide_request(dialog) ? -1 : 0;
    dialog->action = dialog->focus == OSDD_POWER_REBOOT ? OSDD_POWER_ACTION_REBOOT : OSDD_POWER_ACTION_OFF;
    dialog->phase = OSDD_POWER_PROGRESS;
    dialog->command_started = 1;
    return 1;
}

void osdd_power_worker_result(struct osdd_power_dialog *dialog, int result,
                              const char *diagnostic) {
    if (dialog == NULL || dialog->phase != OSDD_POWER_PROGRESS ||
        !dialog->command_dispatched || dialog->command_finished) return;
    dialog->command_finished = 1;
    dialog->command_failed = result < 0;
    if (result < 0)
        snprintf(dialog->diagnostic, sizeof(dialog->diagnostic), "%s",
                 diagnostic != NULL && diagnostic[0] != 0 ? diagnostic
                 : dialog->action == OSDD_POWER_ACTION_REBOOT ? "reboot failed" : "poweroff failed");
}
