#define _GNU_SOURCE
#include "input.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

void osdd_input_init(struct osdd_input *input) {
    if (input == NULL) return;
    memset(input, 0, sizeof(*input));
    input->fd = -1;
}

int osdd_input_open(struct osdd_input *input, char *error, size_t error_size) {
    if (input == NULL) return -1;
    const char *grab_path = input->grab_path; // Kept when a vanished node is reopened.
    osdd_input_init(input);
    input->grab_path = grab_path;
    for (int index = 0; index < 32; ++index) {
        char path[64], name[128] = {0};
        snprintf(path, sizeof(path), "/dev/input/event%d", index);
        int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0) continue;
        struct input_id id;
        if (ioctl(fd, EVIOCGNAME(sizeof(name)), name) > 0 &&
            ioctl(fd, EVIOCGID, &id) == 0 &&
            strcmp(name, "TRIMUI Player1") == 0 &&
            id.vendor == 0x045e && id.product == 0x028e) {
            input->fd = fd;
            if (error != NULL && error_size > 0) error[0] = 0;
            return 0;
        }
        close(fd);
    }
    if (error != NULL && error_size > 0)
        snprintf(error, error_size, "verified TRIMUI Player1 evdev not found");
    return -1;
}

void osdd_input_reset_keys(struct osdd_input *input) {
    if (input != NULL) input->pressed = 0;
}

int osdd_input_decode(struct osdd_input *input, const struct input_event *event,
                      enum osdd_panel_button *button) {
    if (input == NULL || event == NULL || button == NULL) return 0;
    // The panel hotkey belongs to keymon, which requests the panel through
    // /tmp/show_osdd; only the keys used inside the panel are decoded here.
    // Like the official daemon, A and B act on release and directions on press.
    if (event->type == EV_KEY) {
        unsigned bit = event->code == BTN_EAST ? 1U : event->code == BTN_SOUTH ? 2U : 0U;
        if (bit == 0) return 0;
        if (event->value == 1) input->pressed |= bit;
        if (event->value != 0 || (input->pressed & bit) == 0) return 0;
        input->pressed &= ~bit;
        *button = bit == 1U ? OSDD_PANEL_A : OSDD_PANEL_B;
        return 1;
    } else if (event->type == EV_ABS) {
        if (event->code == ABS_HAT0X && event->value != 0) {
            *button = event->value < 0 ? OSDD_PANEL_LEFT : OSDD_PANEL_RIGHT;
            return 1;
        }
        if (event->code == ABS_HAT0Y && event->value != 0) {
            *button = event->value < 0 ? OSDD_PANEL_UP : OSDD_PANEL_DOWN;
            return 1;
        }
    }
    return 0;
}

int osdd_input_poll(struct osdd_input *input, enum osdd_panel_button *buttons,
                    size_t capacity, size_t *count, char *error, size_t error_size) {
    if (input == NULL || input->fd < 0 || buttons == NULL || count == NULL) return -1;
    *count = 0;
    for (;;) {
        struct input_event event;
        ssize_t received = read(input->fd, &event, sizeof(event));
        if (received < 0 && errno == EINTR) continue;
        if (received < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return 0;
        // trimui_inputd owns TRIMUI Player1 and removes it when the firmware restarts
        // inputd before an emulator. The node is closed, with its grab and grab file,
        // so the caller can open the new one.
        if (received == 0 || (received < 0 && (errno == ENODEV || errno == EIO))) {
            if (input->grabbed && input->grab_path != NULL) unlink(input->grab_path);
            close(input->fd);
            input->fd = -1;
            input->grabbed = 0;
            input->pressed = 0;
            return 0;
        }
        if (received != sizeof(event)) {
            if (error != NULL && error_size > 0)
                snprintf(error, error_size, "evdev read: %s",
                         received < 0 ? strerror(errno) : "short event");
            return -1;
        }
        enum osdd_panel_button button;
        if (osdd_input_decode(input, &event, &button)) {
            if (*count == capacity) {
                if (error != NULL && error_size > 0)
                    snprintf(error, error_size, "too many input events in one poll");
                return -1;
            }
            buttons[(*count)++] = button;
        }
    }
}

int osdd_input_grab(struct osdd_input *input, int enabled,
                    char *error, size_t error_size) {
    if (input == NULL || input->fd < 0) return -1;
    enabled = enabled != 0;
    if (input->grabbed == enabled) return 0;
    if (ioctl(input->fd, EVIOCGRAB, enabled) < 0) {
        if (error != NULL && error_size > 0)
            snprintf(error, error_size, "evdev grab: %s", strerror(errno));
        return -1;
    }
    input->grabbed = enabled;
    if (input->grab_path != NULL) {
        if (enabled) {
            int fd = open(input->grab_path, O_WRONLY | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0644);
            if (fd >= 0) close(fd);
            else if (error != NULL && error_size > 0)
                snprintf(error, error_size, "create %s: %s", input->grab_path, strerror(errno));
        } else if (unlink(input->grab_path) < 0 && errno != ENOENT && error != NULL && error_size > 0) {
            snprintf(error, error_size, "remove %s: %s", input->grab_path, strerror(errno));
        }
    }
    return 0;
}

void osdd_input_close(struct osdd_input *input) {
    if (input == NULL) return;
    if (input->fd >= 0) {
        if (input->grabbed) {
            ioctl(input->fd, EVIOCGRAB, 0);
            if (input->grab_path != NULL) unlink(input->grab_path);
        }
        close(input->fd);
    }
    osdd_input_init(input);
}
