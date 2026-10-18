#ifndef BRICKOS_OSDD_PANEL_H
#define BRICKOS_OSDD_PANEL_H

#include "music.h"

#include <stdint.h>

enum osdd_panel_focus {
    OSDD_PANEL_LED,
    OSDD_PANEL_MOTOR,
    OSDD_PANEL_MUSIC,
    OSDD_PANEL_WIFI,
    OSDD_PANEL_BLUETOOTH,
    OSDD_PANEL_VOLUME,
    OSDD_PANEL_CPU,
    OSDD_PANEL_BACKLIGHT,
    OSDD_PANEL_TEMPERATURE,
    OSDD_PANEL_FOCUS_COUNT
};

enum osdd_panel_mode {
    OSDD_PANEL_CLOSED,
    OSDD_PANEL_NAVIGATION,
    OSDD_PANEL_MUSIC_CONTROL,
    OSDD_PANEL_SLIDER_CONTROL
};

enum osdd_panel_button {
    OSDD_PANEL_SHOW,  // keymon's /tmp/show_osdd request.
    OSDD_PANEL_HIDE,  // keymon's /tmp/hide_osdd request.
    OSDD_PANEL_UP,
    OSDD_PANEL_DOWN,
    OSDD_PANEL_LEFT,
    OSDD_PANEL_RIGHT,
    OSDD_PANEL_A,
    OSDD_PANEL_B
};

enum osdd_panel_effect_kind {
    OSDD_PANEL_NO_EFFECT,
    OSDD_PANEL_MUSIC_COMMAND,
    OSDD_PANEL_SLIDER_DELTA,
    OSDD_PANEL_TOGGLE
};

// Installed widget directories and scripts under /usr/trimui/osd/widgets, from each
// widget's config.json "launch" and "update" fields as observed on the stock firmware.
struct osdd_panel_widget {
    const char *directory;
    const char *launch; // Runs with an argument on A (toggles) or Right/Left (sliders).
    const char *update; // Runs without arguments to refresh the status file.
    int at_start;       // Toggles and sliders also update once when the daemon starts.
};

struct osdd_panel_effect {
    enum osdd_panel_effect_kind kind;
    enum osdd_music_command music_command;
    enum osdd_panel_focus focus;
    int delta;
};

struct osdd_panel_state {
    enum osdd_panel_mode mode;
    enum osdd_panel_focus focus;
    // A hide request keeps music control or slider adjustment for the next show, as
    // the official daemon does; B from navigation clears it.
    enum osdd_panel_mode hidden_mode;
};

void osdd_panel_init(struct osdd_panel_state *state);
struct osdd_panel_effect osdd_panel_apply(struct osdd_panel_state *state,
                                          enum osdd_panel_button button);
const char *osdd_panel_focus_name(enum osdd_panel_focus focus);
const struct osdd_panel_widget *osdd_panel_widget(enum osdd_panel_focus focus);
// The official daemon runs every widget's update script in layout order every 32 loop
// iterations: every 6.37 s while its panel is closed and no toast is shown, every 1.89 s
// while the panel is open, and every 1.24 s while a toast keeps the closed-panel loop at
// its 33 ms cadence. A partly elapsed period carries over when the state changes, as
// measured on the official daemon after boot and around toasts.
#define OSDD_PANEL_UPDATE_CLOSED_MS 6370
#define OSDD_PANEL_UPDATE_OPEN_MS 1890
#define OSDD_PANEL_UPDATE_TOAST_MS 1240

// Channel 2 visibility and the signal files the official daemon keeps for other
// services, as measured in bounded traces of the official daemon's panel.
#define OSDD_PANEL_FADE_START_MS 31
#define OSDD_PANEL_FADE_STEP_MS 16
#define OSDD_PANEL_FADE_ALPHA_STEP 17
#define OSDD_PANEL_GRAB_DELAY_MS 112
#define OSDD_PANEL_SIGNAL_OFF_DELAY_MS 200

struct osdd_panel_presence {
    int open;
    int alpha;                 // Channel 2 global alpha to publish.
    uint64_t next_step_ms;     // Next fade step, or 0 when the fade is complete.
    uint64_t grab_at_ms;       // When to take the controller, or 0.
    uint64_t signal_off_at_ms; // When to remove osdd_show_up after a close, or 0.
    int grabbed;
    int signal;                // /tmp/trimui_osd/osdd_show_up exists.
};

enum osdd_presence_action {
    OSDD_PRESENCE_ALPHA = 1,      // Publish presence.alpha on channel 2.
    OSDD_PRESENCE_GRAB = 2,       // Take the controller and create the inputd grab file.
    OSDD_PRESENCE_RELEASE = 4,    // Release the controller and remove the grab file.
    OSDD_PRESENCE_SIGNAL_ON = 8,  // Create osdd_show_up.
    OSDD_PRESENCE_SIGNAL_OFF = 16 // Remove osdd_show_up.
};

// Each call returns a set of osdd_presence_action flags to carry out in order.
unsigned osdd_panel_presence_open(struct osdd_panel_presence *presence, uint64_t now_ms);
unsigned osdd_panel_presence_close(struct osdd_panel_presence *presence, uint64_t now_ms);
unsigned osdd_panel_presence_tick(struct osdd_panel_presence *presence, uint64_t now_ms);
// The earliest pending deadline, or 0 when nothing is scheduled.
uint64_t osdd_panel_presence_due(const struct osdd_panel_presence *presence);

#endif
