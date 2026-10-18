#ifndef BRICKOS_OSDD_DAEMON_H
#define BRICKOS_OSDD_DAEMON_H

#include "music.h"
#include "panel.h"
#include "power.h"
#include "state.h"
#include "toast.h"

#include <stddef.h>
#include <stdint.h>

// The daemon's loop without its devices. main.c supplies the evdev node, the display,
// the renderer, and the shared files; tests supply fakes on a simulated clock.

struct osdd_panel_status {
    int led, motor, wifi, bluetooth;
    int volume, volume_max, backlight, backlight_max;
    char cpu[32], temperature[32];
};

struct osdd_daemon_io {
    void *context;
    uint64_t (*now_us)(void *context);
    int (*poll_input)(void *context, enum osdd_panel_button *buttons, size_t capacity,
                      size_t *count, char *error, size_t error_size);
    void (*reset_keys)(void *context);
    // Consumes keymon's /tmp/show_osdd (hide 0) or /tmp/hide_osdd (hide 1).
    int (*consume_request)(void *context, int hide);
    int (*consume_power_request)(void *context, char *error, size_t error_size);
    // Returns 1 while /tmp/poweroff_flag exists, 0 when absent, -1 on an inspection error.
    int (*poweroff_flag)(void *context);
    // Starts the one worker for the chosen action: reboot or poweroff.
    int (*start_power)(void *context, enum osdd_power_action action, char *error, size_t error_size);
    // Returns 0 while pending, 1 on command success, -1 on command failure.
    int (*poll_poweroff)(void *context, char *error, size_t error_size);
    enum osdd_poll_result (*poll_toast)(void *context, struct osdd_toast *toast,
                                        char *error, size_t error_size);
    enum osdd_music_result (*poll_music)(void *context, char *error, size_t error_size);
    int (*read_status)(void *context, struct osdd_panel_status *status);
    // Renders the panel, or with music_only just the music tile over the previous panel
    // frame, and publishes it at the given alpha.
    int (*publish_panel)(void *context, const struct osdd_panel_state *panel,
                         const struct osdd_panel_status *status, int music_only,
                         unsigned char alpha, uint64_t now_ms, char *error, size_t error_size);
    int (*panel_alpha)(void *context, unsigned char alpha, char *error, size_t error_size);
    // Channel 3 composition: current toasts plus a modal power dialog, if present.
    int (*publish_overlay)(void *context, const struct osdd_toast_state *toasts,
                          const struct osdd_power_dialog *power, int toast_visible,
                          uint64_t now_ms, char *error, size_t error_size);
    int (*hide_overlay)(void *context, char *error, size_t error_size);
    int (*grab)(void *context, int enabled, char *error, size_t error_size);
    int (*grab_modal)(void *context, int enabled, char *error, size_t error_size);
    void (*panel_signal)(void *context, int enabled);
    int (*music_command)(void *context, enum osdd_music_command command,
                         char *error, size_t error_size);
    int (*widget_queue)(void *context, const char *directory, const char *script,
                        const char *argument);
    int (*widget_run)(void *context, uint64_t now_ms); // 1 when a script ended.
    int (*widget_idle)(void *context);
    // The widget scripts read keymon's shared memory with shmvar; until keymon has
    // created it, early in boot, no script may run.
    int (*widgets_ready)(void *context);
    // 1 while GameSwitcher's layer is open on channel 2. The daemon then writes nothing to
    // channel 2 and leaves a panel request waiting; toasts on channel 3 go on. May be NULL.
    int (*gameswitcher_display)(void *context);
};

struct osdd_daemon {
    const struct osdd_daemon_io *io;
    struct osdd_toast_state toasts;
    struct osdd_power_dialog power;
    struct osdd_panel_state panel;
    struct osdd_panel_presence presence;
    struct osdd_panel_status status;
    int status_valid;
    int status_pending;       // Status is read at open and after each widget script.
    int panel_dirty;
    int music_dirty;          // Only the canvas changed since the last panel frame.
    int music_has_frame;
    int music_error_logged;   // A persistent read error is logged once, not per iteration.
    int panel_frame_current;  // The renderer still holds the last panel frame.
    int panel_frame_published;
    int toast_dirty;
    int toast_visible;
    int power_dirty;
    int power_logged;         // The power dialog's phase, focus and action last logged.
    int overlay_published;
    uint64_t panel_last_publish_ms;
    uint64_t toast_last_publish_ms;
    // Time left before the next widget update batch, in closed-panel milliseconds; it
    // runs 6370/1890 times faster while the panel is open.
    int64_t update_remaining_ms;
    uint64_t update_checked_ms;
    int widgets_ready;        // keymon's shared memory exists; the start updates ran.
    uint64_t next_toast_us;
    int channel_shared;       // GameSwitcher holds channel 2 this iteration.
    uint64_t start_ms;
    int trace;                // Diagnostic lines for comparisons; not a stock behavior.
};

void osdd_daemon_init(struct osdd_daemon *daemon, const struct osdd_daemon_io *io, int trace);
// Runs one loop iteration. Returns 0 with the time to sleep before the next one, or -1
// when a device failed and the daemon must stop.
int osdd_daemon_step(struct osdd_daemon *daemon, uint64_t *wait_us);

#endif
