#ifndef BRICKOS_OSDD_WIDGETS_H
#define BRICKOS_OSDD_WIDGETS_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

// Runs installed panel widget scripts one at a time, as the official daemon does: a
// widget's launch script with an argument for an action, or its update script. The
// scripts, not the OSD, own every hardware and service request.
#define OSDD_WIDGET_ROOT "/usr/trimui/osd/widgets"
#define OSDD_WIDGET_QUEUE 32
#define OSDD_WIDGET_TIMEOUT_MS 5000

struct osdd_widget_job {
    char directory[64];
    char script[32];
    char argument[4]; // Empty for an update without arguments.
};

struct osdd_widget_runner {
    const char *root;
    struct osdd_widget_job queue[OSDD_WIDGET_QUEUE];
    size_t head, count;
    pid_t pid;
    uint64_t started_ms;
    struct osdd_widget_job current;
    unsigned timeouts;
    uint64_t timeout_ms; // OSDD_WIDGET_TIMEOUT_MS unless a test shortens it.
};

void osdd_widget_runner_init(struct osdd_widget_runner *runner, const char *root);
// Returns -1 when the queue is full or the names are invalid.
int osdd_widget_queue(struct osdd_widget_runner *runner, const char *directory,
                      const char *script, const char *argument);
// Reaps a finished script, stops one past its timeout, and starts the next. Returns 1
// when a script ended, so its status file can be read as the official daemon does.
int osdd_widget_run(struct osdd_widget_runner *runner, uint64_t now_ms);
int osdd_widget_idle(const struct osdd_widget_runner *runner);
// Waits briefly for a running script at exit, then stops it.
void osdd_widget_runner_close(struct osdd_widget_runner *runner);

#endif
