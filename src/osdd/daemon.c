#define _POSIX_C_SOURCE 200809L
#include "daemon.h"

#include <stdio.h>
#include <string.h>

void osdd_daemon_init(struct osdd_daemon *daemon, const struct osdd_daemon_io *io, int trace) {
    memset(daemon, 0, sizeof(*daemon));
    daemon->io = io;
    osdd_panel_init(&daemon->panel);
    osdd_power_init(&daemon->power);
    daemon->status_pending = 1;
    daemon->trace = trace;
    daemon->start_ms = io->now_us(io->context) / 1000;
}

static int sync_power_presence(struct osdd_daemon *daemon, int was_visible,
                               char *error, size_t error_size) {
    int visible = osdd_power_visible(&daemon->power);
    if (visible == was_visible) return 0;
    if (visible) {
        daemon->io->reset_keys(daemon->io->context);
        if (daemon->io->grab_modal(daemon->io->context, 1, error, error_size) < 0) return -1;
    } else if (daemon->io->grab_modal(daemon->io->context, 0, error, error_size) < 0) {
        return -1;
    }
    daemon->power_dirty = 1;
    if (!visible && daemon->toast_visible) daemon->toast_dirty = 1;
    return 0;
}

// At start the official daemon updates its toggles and sliders once, in layout order, so
// their status files exist before the panel first opens. Started from the boot hook,
// BrickOS comes before keymon, so this waits for keymon's shared memory.
static void start_widgets(struct osdd_daemon *daemon, uint64_t now) {
    const struct osdd_daemon_io *io = daemon->io;
    for (int focus = 0; focus < OSDD_PANEL_FOCUS_COUNT; ++focus) {
        const struct osdd_panel_widget *widget = osdd_panel_widget((enum osdd_panel_focus)focus);
        if (widget->at_start && widget->update != NULL)
            io->widget_queue(io->context, widget->directory, widget->update, NULL);
    }
    daemon->widgets_ready = 1;
    daemon->update_remaining_ms = OSDD_PANEL_UPDATE_CLOSED_MS;
    daemon->update_checked_ms = now;
}

// Applies one input or request to the panel and returns the presence actions it causes.
// Toggles and slider steps run the widget's installed launch script, as stock does.
static unsigned panel_input(struct osdd_daemon *daemon, enum osdd_panel_button button,
                            uint64_t now) {
    const struct osdd_daemon_io *io = daemon->io;
    struct osdd_panel_state *panel = &daemon->panel;
    char error[160];
    enum osdd_panel_mode before_mode = panel->mode;
    enum osdd_panel_focus before_focus = panel->focus;
    struct osdd_panel_effect effect = osdd_panel_apply(panel, button);
    if (effect.kind == OSDD_PANEL_MUSIC_COMMAND &&
        io->music_command(io->context, effect.music_command, error, sizeof(error)) < 0)
        fprintf(stderr, "music command: %s\n", error);
    const struct osdd_panel_widget *widget = osdd_panel_widget(effect.focus);
    if ((effect.kind == OSDD_PANEL_TOGGLE || effect.kind == OSDD_PANEL_SLIDER_DELTA) &&
        daemon->widgets_ready && widget != NULL && widget->launch != NULL &&
        io->widget_queue(io->context, widget->directory, widget->launch,
                         effect.kind == OSDD_PANEL_SLIDER_DELTA && effect.delta < 0 ? "0" : "1") < 0)
        fprintf(stderr, "widget action queue full: %s\n", widget->directory);
    if (panel->mode != before_mode || panel->focus != before_focus) daemon->panel_dirty = 1;
    if (button == OSDD_PANEL_SHOW) {
        daemon->panel_dirty = 1;
        return osdd_panel_presence_open(&daemon->presence, now);
    }
    if (before_mode != OSDD_PANEL_CLOSED && panel->mode == OSDD_PANEL_CLOSED)
        return osdd_panel_presence_close(&daemon->presence, now);
    return 0;
}

// Carries out presence actions. Alpha changes wait for the first frame after an open,
// which is then published at the current fade alpha.
static int apply_presence(struct osdd_daemon *daemon, unsigned actions,
                          char *error, size_t error_size) {
    const struct osdd_daemon_io *io = daemon->io;
    if ((actions & OSDD_PRESENCE_ALPHA) && daemon->panel_frame_published && !daemon->channel_shared &&
        io->panel_alpha(io->context, (unsigned char)daemon->presence.alpha, error, error_size) < 0)
        return -1;
    if (actions & OSDD_PRESENCE_SIGNAL_ON) io->panel_signal(io->context, 1);
    error[0] = 0;
    if ((actions & OSDD_PRESENCE_GRAB) && io->grab(io->context, 1, error, error_size) < 0) return -1;
    if ((actions & OSDD_PRESENCE_RELEASE) && io->grab(io->context, 0, error, error_size) < 0) return -1;
    if (error[0] != 0) fprintf(stderr, "input grab signal: %s\n", error);
    if (actions & OSDD_PRESENCE_SIGNAL_OFF) io->panel_signal(io->context, 0);
    return 0;
}

int osdd_daemon_step(struct osdd_daemon *daemon, uint64_t *wait_us) {
    const struct osdd_daemon_io *io = daemon->io;
    char error[256];
    uint64_t now_us = io->now_us(io->context);
    uint64_t now = now_us / 1000;
    int toast_iterated = now_us >= daemon->next_toast_us;
    enum osdd_panel_button buttons[32];
    size_t button_count = 0;
    if (io->poll_input(io->context, buttons, 32, &button_count, error, sizeof(error)) < 0) {
        fprintf(stderr, "OSD input: %s\n", error);
        return -1;
    }
    // While GameSwitcher holds channel 2 nothing is written there. When it lets go, the panel,
    // if open, is drawn again in full at its current alpha.
    int shared = io->gameswitcher_display != NULL && io->gameswitcher_display(io->context) > 0;
    if (daemon->channel_shared && !shared && daemon->panel.mode != OSDD_PANEL_CLOSED) daemon->panel_dirty = 1;
    daemon->channel_shared = shared;
    int visible_before_requests = osdd_power_visible(&daemon->power);
    int flag = io->poweroff_flag(io->context);
    if (flag != 0) {
        if (osdd_power_flag_request(&daemon->power)) {
            daemon->power_dirty = 1;
            if (flag < 0)
                snprintf(daemon->power.diagnostic, sizeof(daemon->power.diagnostic),
                         "Cannot inspect /tmp/poweroff_flag");
        }
        if (flag < 0) fprintf(stderr, "poweroff flag: unable to inspect /tmp/poweroff_flag\n");
    }
    int power_request = io->consume_power_request(io->context, error, sizeof(error));
    if (power_request < 0) fprintf(stderr, "power hold marker: %s\n", error);
    else if (power_request > 0 && flag <= 0 && osdd_power_hold_request(&daemon->power))
        daemon->power_dirty = 1;
    int visible_after_requests = osdd_power_visible(&daemon->power);
    if (sync_power_presence(daemon, visible_before_requests, error, sizeof(error)) < 0) {
        fprintf(stderr, "power dialog input grab: %s\n", error);
        return -1;
    }
    int modal_this_poll = visible_before_requests || visible_after_requests;
    int modal_just_opened = !visible_before_requests && visible_after_requests;
    unsigned actions = 0;
    for (size_t index = 0; index < button_count; ++index) {
        if (modal_this_poll) {
            // Discard input collected in the same poll that opened the modal; no button
            // from the game or a just-released key can accept it accidentally.
            if (modal_just_opened) continue;
            int result = osdd_power_input(&daemon->power, buttons[index]);
            if (result != 0) daemon->power_dirty = 1;
            if (result == 1) {
                // A MainUI request can arrive after the hold marker but before OK. It
                // changes this accepted request to direct progress before any command.
                int current_flag = io->poweroff_flag(io->context);
                if (current_flag != 0) {
                    if (osdd_power_flag_request(&daemon->power)) {
                        daemon->power_dirty = 1;
                        if (current_flag < 0)
                            snprintf(daemon->power.diagnostic, sizeof(daemon->power.diagnostic),
                                     "Cannot inspect /tmp/poweroff_flag");
                    }
                } else {
                    osdd_power_worker_dispatch(&daemon->power);
                    if (io->start_power(io->context, daemon->power.action, error, sizeof(error)) < 0) {
                        osdd_power_worker_result(&daemon->power, -1, error);
                        fprintf(stderr, "power worker: %s\n", error);
                        daemon->power_dirty = 1;
                    }
                }
            }
        } else {
            actions |= panel_input(daemon, buttons[index], now);
        }
    }
    if (sync_power_presence(daemon, visible_after_requests, error, sizeof(error)) < 0) {
        fprintf(stderr, "power dialog input release: %s\n", error);
        return -1;
    }
    // keymon's panel requests are checked on the official loop cadence. A show request waits,
    // unconsumed, while GameSwitcher holds channel 2.
    if (toast_iterated && !shared && io->consume_request(io->context, 0)) {
        if (!osdd_power_visible(&daemon->power)) {
            if (daemon->panel.mode == OSDD_PANEL_CLOSED) io->reset_keys(io->context);
            actions |= panel_input(daemon, OSDD_PANEL_SHOW, now);
            daemon->status_pending = 1;
        }
    }
    if (toast_iterated && io->consume_request(io->context, 1)) {
        int was_visible = osdd_power_visible(&daemon->power);
        if (osdd_power_hide_request(&daemon->power)) daemon->power_dirty = 1;
        actions |= panel_input(daemon, OSDD_PANEL_HIDE, now);
        if (sync_power_presence(daemon, was_visible, error, sizeof(error)) < 0) {
            fprintf(stderr, "power dialog input release: %s\n", error);
            return -1;
        }
    }
    if (daemon->power.command_dispatched && !daemon->power.command_finished) {
        int result = io->poll_poweroff(io->context, error, sizeof(error));
        if (result != 0) {
            osdd_power_worker_result(&daemon->power, result, error);
            daemon->power_dirty = 1;
            const char *command = daemon->power.action == OSDD_POWER_ACTION_REBOOT ? "reboot" : "poweroff";
            if (result < 0) fprintf(stderr, "%s command failed: %s\n", command, error);
            else fprintf(stderr, "%s command returned successfully\n", command);
        }
    }
    // The power dialog's state each time it changes, for the end-to-end plans (docs/osdd.md).
    int power_state = (int)daemon->power.phase * 16 + (int)daemon->power.focus * 4 + (int)daemon->power.action;
    if (power_state != daemon->power_logged) {
        static const char *const phases[] = {"closed", "confirm", "progress"};
        static const char *const focuses[] = {"off", "reboot", "cancel"};
        fprintf(stderr, "power dialog phase=%s focus=%s action=%s\n", phases[daemon->power.phase],
                focuses[daemon->power.focus], daemon->power.action == OSDD_POWER_ACTION_REBOOT ? "reboot" : "off");
        daemon->power_logged = power_state;
    }
    actions |= osdd_panel_presence_tick(&daemon->presence, now);
    if (actions != 0 && apply_presence(daemon, actions, error, sizeof(error)) < 0) {
        fprintf(stderr, "panel presence: %s\n", error);
        return -1;
    }
    if (!daemon->presence.open) daemon->panel_frame_published = 0;
    int open = daemon->panel.mode != OSDD_PANEL_CLOSED;
    if (!daemon->widgets_ready && io->widgets_ready(io->context)) start_widgets(daemon, now);
    // Every widget's update script runs in layout order every 6.37 s while the panel is
    // closed, every 1.89 s while it is open, and every 1.24 s while a toast is shown on
    // the closed panel, as the official daemon's count of loop iterations gives.
    uint64_t elapsed = now > daemon->update_checked_ms ? now - daemon->update_checked_ms : 0;
    daemon->update_checked_ms = now;
    uint64_t period = open ? OSDD_PANEL_UPDATE_OPEN_MS :
                      osdd_toast_state_active(&daemon->toasts) ? OSDD_PANEL_UPDATE_TOAST_MS :
                      OSDD_PANEL_UPDATE_CLOSED_MS;
    daemon->update_remaining_ms -= (int64_t)(elapsed * OSDD_PANEL_UPDATE_CLOSED_MS / period);
    if (daemon->widgets_ready && daemon->update_remaining_ms <= 0 && io->widget_idle(io->context)) {
        for (int focus = 0; focus < OSDD_PANEL_FOCUS_COUNT; ++focus) {
            const struct osdd_panel_widget *widget = osdd_panel_widget((enum osdd_panel_focus)focus);
            if (widget->update != NULL)
                io->widget_queue(io->context, widget->directory, widget->update, NULL);
        }
        daemon->update_remaining_ms = OSDD_PANEL_UPDATE_CLOSED_MS;
    }
    if (io->widget_run(io->context, now)) daemon->status_pending = 1;
    // The official panel followed most canvas changes within 10 ms, so the canvas is
    // compared on every iteration while the panel is open. A failure only affects the
    // music tile; toast service below continues.
    if (open) {
        enum osdd_music_result result = io->poll_music(io->context, error, sizeof(error));
        if (result == OSDD_MUSIC_UPDATE || (result == OSDD_MUSIC_ABSENT && daemon->music_has_frame))
            daemon->music_dirty = 1;
        else if (result == OSDD_MUSIC_ERROR && !daemon->music_error_logged)
            fprintf(stderr, "music frame: %s\n", error);
        daemon->music_error_logged = result == OSDD_MUSIC_ERROR;
        if (result == OSDD_MUSIC_UPDATE) daemon->music_has_frame = 1;
        if (result == OSDD_MUSIC_ABSENT) daemon->music_has_frame = 0;
    }
    if (open && daemon->status_pending) {
        struct osdd_panel_status next;
        memset(&next, 0, sizeof(next));
        if (io->read_status(io->context, &next) == 0) {
            if (!daemon->status_valid || memcmp(&next, &daemon->status, sizeof(next)) != 0)
                daemon->panel_dirty = 1;
            daemon->status = next;
            daemon->status_valid = 1;
        } else {
            fprintf(stderr, "panel status incomplete\n");
        }
        daemon->status_pending = 0;
    }
    if (open && !shared && (daemon->panel_dirty || daemon->music_dirty) && daemon->status_valid &&
        (daemon->panel_last_publish_ms == 0 || now - daemon->panel_last_publish_ms >= 50)) {
        // A canvas change redraws only the music tile into the kept panel frame; the
        // result is still published to the other slot as a complete frame.
        int music_only = !daemon->panel_dirty && daemon->panel_frame_current;
        if (io->publish_panel(io->context, &daemon->panel, &daemon->status, music_only,
                              (unsigned char)daemon->presence.alpha, now,
                              error, sizeof(error)) < 0) {
            fprintf(stderr, "panel publish: %s\n", error);
            return -1;
        }
        daemon->panel_last_publish_ms = now;
        daemon->panel_dirty = 0;
        daemon->music_dirty = 0;
        daemon->panel_frame_current = 1;
        daemon->panel_frame_published = 1;
    }
    if (toast_iterated) {
        // One iteration of the official toast loop: consume a record, count every
        // slot down from the previous iteration, then decide the layer state.
        struct osdd_toast next;
        enum osdd_poll_result poll = io->poll_toast(io->context, &next, error, sizeof(error));
        if (poll == OSDD_POLL_UPDATE) {
            int slot = osdd_toast_state_add(&daemon->toasts, &next, now);
            daemon->toast_dirty = 1;
            fprintf(stderr, "toast slot=%d id=%s type=%s duration=%d\n",
                    slot, next.id, next.type, next.duration);
        }
        if (poll == OSDD_POLL_DISCARDED) fprintf(stderr, "toast discarded: %s\n", error);
        else if (error[0] != 0) fprintf(stderr, "toast input: %s\n", error);
        if (osdd_toast_state_check_timeout(&daemon->toasts, now) > 0) daemon->toast_dirty = 1;
        enum osdd_toast_show show = osdd_toast_state_check_show(&daemon->toasts);
        if (daemon->trace)
            fprintf(stderr, "trace t=%llu poll=%d count=%d show=%d pending=%d dirty=%d\n",
                    (unsigned long long)(now - daemon->start_ms), poll, daemon->toasts.count,
                    show, daemon->toasts.hide_pending, daemon->toast_dirty);
        // When the active count reaches zero the official daemon clears its frame, also
        // when a countdown stopped at exactly zero and freed nothing.
        if (show == OSDD_TOAST_KEEP && daemon->toasts.hide_pending == 1) daemon->toast_dirty = 1;
        if (show == OSDD_TOAST_SHOW) daemon->toast_visible = 1;
        if (show == OSDD_TOAST_HIDE) {
            daemon->toast_visible = 0;
            daemon->toast_dirty = 1;
        }
    }
    // A cleared frame stays published until the hide one iteration later. Toasts share
    // the renderer's frame, so the next panel frame is drawn in full.
    int overlay_wanted = daemon->toast_visible || osdd_power_visible(&daemon->power);
    if (overlay_wanted && (daemon->toast_dirty || daemon->power_dirty) &&
        (daemon->toast_last_publish_ms == 0 || now - daemon->toast_last_publish_ms >= 50)) {
        daemon->panel_frame_current = 0;
        if (io->publish_overlay(io->context, &daemon->toasts, &daemon->power,
                                daemon->toast_visible, now, error, sizeof(error)) < 0) {
            fprintf(stderr, "OSD publish: %s\n", error);
            return -1;
        }
        daemon->toast_last_publish_ms = now;
        daemon->toast_dirty = 0;
        daemon->power_dirty = 0;
        daemon->overlay_published = 1;
    } else if (!overlay_wanted && daemon->overlay_published) {
        if (io->hide_overlay(io->context, error, sizeof(error)) < 0) {
            fprintf(stderr, "OSD hide: %s\n", error);
            return -1;
        }
        daemon->overlay_published = 0;
    }
    uint64_t after = io->now_us(io->context);
    // Like the official loop, sleep 33,332 us while a toast or the panel is active and
    // 200,000 us otherwise, measured from the end of the iteration's work.
    if (toast_iterated)
        daemon->next_toast_us = after + (osdd_toast_state_active(&daemon->toasts) ||
                                         daemon->panel.mode != OSDD_PANEL_CLOSED ? 33332 : 200000);
    uint64_t wait = 20000;
    if (daemon->next_toast_us > after && daemon->next_toast_us - after < wait)
        wait = daemon->next_toast_us - after;
    else if (daemon->next_toast_us <= after) wait = 1000;
    // The panel fade steps every 16 ms; also wake for the grab and signal deadlines.
    uint64_t presence_us = osdd_panel_presence_due(&daemon->presence) * 1000;
    if (presence_us != 0 && presence_us <= after) wait = 1000;
    else if (presence_us != 0 && presence_us - after < wait) wait = presence_us - after;
    *wait_us = wait;
    return 0;
}
