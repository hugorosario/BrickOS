#ifndef BRICKOS_MAINUI_ACTIONS_H
#define BRICKOS_MAINUI_ACTIONS_H

#include "model.h"

#include <stddef.h>

// The shell commands the official MainUI runs for high-impact settings and system actions,
// from static evidence (its strings), for the model's effects. They go to a runner, so tests
// record them instead of running them. The model only emits these effects after the stock
// confirmation, and a cancelled dialog emits nothing.

struct mainui_runner {
    void *context;
    // Runs one shell command; returns its exit status (0 for success).
    int (*run)(void *context, const char *command);
    // Runs one shell command and keeps its output in out (cut to size); returns 0, or -1 when
    // it could not run. May be NULL. Dry runs keep it for read-only queries (wpa_cli status).
    int (*capture)(void *context, const char *command, char *out, size_t size);
};

// Runs the commands an effect needs, in order, stopping at a failure. Returns the number of
// commands run, 0 for an effect without commands, or -1 when one failed.
int mainui_actions_apply(const struct mainui_runner *runner, const struct mainui_effect *effect);

// The commands an effect needs, without running them; returns their count (at most max).
int mainui_actions_commands(const struct mainui_effect *effect, const char **commands, int max);

#endif
