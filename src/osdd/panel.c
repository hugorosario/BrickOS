#include "panel.h"

#include <stddef.h>

// Measured with 50 ms presses on the official daemon's open /dev/input/event3.
// Columns: Up, Down, Left, Right.
static const enum osdd_panel_focus neighbors[OSDD_PANEL_FOCUS_COUNT][4] = {
    {OSDD_PANEL_BACKLIGHT, OSDD_PANEL_WIFI, OSDD_PANEL_MUSIC, OSDD_PANEL_MOTOR},
    {OSDD_PANEL_BACKLIGHT, OSDD_PANEL_BLUETOOTH, OSDD_PANEL_LED, OSDD_PANEL_MUSIC},
    {OSDD_PANEL_BACKLIGHT, OSDD_PANEL_VOLUME, OSDD_PANEL_MOTOR, OSDD_PANEL_LED},
    {OSDD_PANEL_LED, OSDD_PANEL_VOLUME, OSDD_PANEL_MUSIC, OSDD_PANEL_BLUETOOTH},
    {OSDD_PANEL_MOTOR, OSDD_PANEL_VOLUME, OSDD_PANEL_WIFI, OSDD_PANEL_MUSIC},
    {OSDD_PANEL_WIFI, OSDD_PANEL_BACKLIGHT, OSDD_PANEL_CPU, OSDD_PANEL_CPU},
    {OSDD_PANEL_MUSIC, OSDD_PANEL_TEMPERATURE, OSDD_PANEL_VOLUME, OSDD_PANEL_VOLUME},
    {OSDD_PANEL_VOLUME, OSDD_PANEL_LED, OSDD_PANEL_TEMPERATURE, OSDD_PANEL_TEMPERATURE},
    {OSDD_PANEL_CPU, OSDD_PANEL_MUSIC, OSDD_PANEL_BACKLIGHT, OSDD_PANEL_BACKLIGHT},
};

// In osdlayout.json order, which is also the official update order. The temperature
// config names a set.sh that is not installed, and A on it had no effect.
static const struct osdd_panel_widget widgets[OSDD_PANEL_FOCUS_COUNT] = {
    [OSDD_PANEL_LED] = {"toggle_led", "set.sh", "update.sh", 1},
    [OSDD_PANEL_MOTOR] = {"toggle_rumble", "set.sh", "update.sh", 1},
    [OSDD_PANEL_MUSIC] = {"app_music", NULL, NULL, 0},
    [OSDD_PANEL_WIFI] = {"toggle_wifi", "set.sh", "update.sh", 1},
    [OSDD_PANEL_BLUETOOTH] = {"toggle_bt", "set.sh", "update.sh", 1},
    [OSDD_PANEL_VOLUME] = {"slider_volume_global", "set.sh", "set.sh", 1},
    [OSDD_PANEL_CPU] = {"static_cpu_freq", NULL, "refresh.sh", 0},
    [OSDD_PANEL_BACKLIGHT] = {"slider_backlight", "set.sh", "set.sh", 1},
    [OSDD_PANEL_TEMPERATURE] = {"static_temperature", NULL, "refresh.sh", 0},
};

const struct osdd_panel_widget *osdd_panel_widget(enum osdd_panel_focus focus) {
    return focus >= 0 && focus < OSDD_PANEL_FOCUS_COUNT ? &widgets[focus] : NULL;
}

void osdd_panel_init(struct osdd_panel_state *state) {
    if (state == NULL) return;
    state->mode = OSDD_PANEL_CLOSED;
    state->focus = OSDD_PANEL_LED;
    state->hidden_mode = OSDD_PANEL_NAVIGATION;
}

const char *osdd_panel_focus_name(enum osdd_panel_focus focus) {
    static const char *const names[] = {
        "LED", "Motor", "Music", "Wi-Fi", "Bluetooth", "Volume", "CPU",
        "Backlight", "Temperature"
    };
    return focus >= 0 && focus < OSDD_PANEL_FOCUS_COUNT ? names[focus] : NULL;
}

struct osdd_panel_effect osdd_panel_apply(struct osdd_panel_state *state,
                                          enum osdd_panel_button button) {
    struct osdd_panel_effect result = {.kind = OSDD_PANEL_NO_EFFECT};
    if (state == NULL || button < OSDD_PANEL_SHOW || button > OSDD_PANEL_B ||
        state->focus < 0 || state->focus >= OSDD_PANEL_FOCUS_COUNT) return result;
    result.focus = state->focus;
    // Neither request sends a music command; musicserver stays enabled while hidden.
    if (button == OSDD_PANEL_SHOW) {
        if (state->mode == OSDD_PANEL_CLOSED)
            state->mode = state->hidden_mode == OSDD_PANEL_CLOSED ? OSDD_PANEL_NAVIGATION
                                                                  : state->hidden_mode;
        return result;
    }
    if (button == OSDD_PANEL_HIDE) {
        if (state->mode != OSDD_PANEL_CLOSED) state->hidden_mode = state->mode;
        state->mode = OSDD_PANEL_CLOSED;
        return result;
    }
    if (state->mode == OSDD_PANEL_CLOSED) return result;
    if (button == OSDD_PANEL_B) {
        if (state->mode == OSDD_PANEL_MUSIC_CONTROL) {
            result.kind = OSDD_PANEL_MUSIC_COMMAND;
            result.music_command = OSDD_MUSIC_DISABLE;
            state->mode = OSDD_PANEL_NAVIGATION;
        } else if (state->mode == OSDD_PANEL_SLIDER_CONTROL) {
            state->mode = OSDD_PANEL_NAVIGATION;
        } else {
            state->mode = OSDD_PANEL_CLOSED;
            state->hidden_mode = OSDD_PANEL_NAVIGATION;
        }
        return result;
    }
    if (state->mode == OSDD_PANEL_MUSIC_CONTROL) {
        result.kind = OSDD_PANEL_MUSIC_COMMAND;
        switch (button) {
            case OSDD_PANEL_UP: result.music_command = OSDD_MUSIC_UP; break;
            case OSDD_PANEL_DOWN: result.music_command = OSDD_MUSIC_DOWN; break;
            case OSDD_PANEL_LEFT: result.music_command = OSDD_MUSIC_LEFT; break;
            case OSDD_PANEL_RIGHT: result.music_command = OSDD_MUSIC_RIGHT; break;
            case OSDD_PANEL_A: result.music_command = OSDD_MUSIC_KEY_A; break;
            default: result.kind = OSDD_PANEL_NO_EFFECT; break;
        }
        return result;
    }
    if (state->mode == OSDD_PANEL_SLIDER_CONTROL) {
        if (button == OSDD_PANEL_LEFT || button == OSDD_PANEL_RIGHT) {
            result.kind = OSDD_PANEL_SLIDER_DELTA;
            result.delta = button == OSDD_PANEL_RIGHT ? 1 : -1;
        }
        return result;
    }
    if (button >= OSDD_PANEL_UP && button <= OSDD_PANEL_RIGHT) {
        state->focus = neighbors[state->focus][button - OSDD_PANEL_UP];
        result.focus = state->focus;
    } else if (button == OSDD_PANEL_A) {
        if (state->focus == OSDD_PANEL_MUSIC) {
            state->mode = OSDD_PANEL_MUSIC_CONTROL;
            result.kind = OSDD_PANEL_MUSIC_COMMAND;
            result.music_command = OSDD_MUSIC_ENABLE;
        } else if (state->focus == OSDD_PANEL_VOLUME ||
                   state->focus == OSDD_PANEL_BACKLIGHT) {
            state->mode = OSDD_PANEL_SLIDER_CONTROL;
        } else if (state->focus == OSDD_PANEL_LED || state->focus == OSDD_PANEL_MOTOR ||
                   state->focus == OSDD_PANEL_WIFI || state->focus == OSDD_PANEL_BLUETOOTH) {
            result.kind = OSDD_PANEL_TOGGLE;
        }
    }
    return result;
}

unsigned osdd_panel_presence_open(struct osdd_panel_presence *presence, uint64_t now_ms) {
    if (presence == NULL) return 0;
    unsigned actions = 0;
    // A request while open drops to alpha 0 and restarts the fade.
    if (presence->alpha != 0) actions |= OSDD_PRESENCE_ALPHA;
    presence->open = 1;
    presence->alpha = 0;
    presence->next_step_ms = now_ms + OSDD_PANEL_FADE_START_MS;
    presence->signal_off_at_ms = 0;
    if (!presence->signal) {
        presence->signal = 1;
        actions |= OSDD_PRESENCE_SIGNAL_ON;
    }
    if (!presence->grabbed && presence->grab_at_ms == 0)
        presence->grab_at_ms = now_ms + OSDD_PANEL_GRAB_DELAY_MS;
    return actions;
}

unsigned osdd_panel_presence_close(struct osdd_panel_presence *presence, uint64_t now_ms) {
    if (presence == NULL || !presence->open) return 0;
    unsigned actions = OSDD_PRESENCE_ALPHA;
    presence->open = 0;
    presence->alpha = 0;
    presence->next_step_ms = 0;
    presence->grab_at_ms = 0;
    if (presence->grabbed) {
        presence->grabbed = 0;
        actions |= OSDD_PRESENCE_RELEASE;
    }
    if (presence->signal) presence->signal_off_at_ms = now_ms + OSDD_PANEL_SIGNAL_OFF_DELAY_MS;
    return actions;
}

unsigned osdd_panel_presence_tick(struct osdd_panel_presence *presence, uint64_t now_ms) {
    if (presence == NULL) return 0;
    unsigned actions = 0;
    if (presence->next_step_ms != 0 && now_ms >= presence->next_step_ms) {
        presence->alpha += OSDD_PANEL_FADE_ALPHA_STEP;
        if (presence->alpha >= 255) {
            presence->alpha = 255;
            presence->next_step_ms = 0;
        } else {
            presence->next_step_ms += OSDD_PANEL_FADE_STEP_MS;
        }
        actions |= OSDD_PRESENCE_ALPHA;
    }
    if (presence->grab_at_ms != 0 && now_ms >= presence->grab_at_ms) {
        presence->grab_at_ms = 0;
        presence->grabbed = 1;
        actions |= OSDD_PRESENCE_GRAB;
    }
    if (presence->signal_off_at_ms != 0 && now_ms >= presence->signal_off_at_ms) {
        presence->signal_off_at_ms = 0;
        presence->signal = 0;
        actions |= OSDD_PRESENCE_SIGNAL_OFF;
    }
    return actions;
}

uint64_t osdd_panel_presence_due(const struct osdd_panel_presence *presence) {
    if (presence == NULL) return 0;
    uint64_t due = 0;
    const uint64_t deadlines[] = {presence->next_step_ms, presence->grab_at_ms,
                                  presence->signal_off_at_ms};
    for (size_t index = 0; index < sizeof(deadlines) / sizeof(deadlines[0]); ++index)
        if (deadlines[index] != 0 && (due == 0 || deadlines[index] < due)) due = deadlines[index];
    return due;
}
