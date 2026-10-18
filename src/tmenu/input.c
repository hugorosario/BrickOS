#define _DEFAULT_SOURCE
#include "input.h"

#include <dirent.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <sys/ioctl.h>
#include <unistd.h>

enum tmenu_key tmenu_input_decode(struct tmenu_hats *hats, size_t position, const struct input_event *event,
                                  int *stop) {
    *stop = 0;
    if (position >= TMENU_INPUT_DEVICES) return TMENU_KEY_NONE;
    if (event->type == EV_KEY) {
        if (event->value == 0) return TMENU_KEY_NONE;
        switch (event->code) {
        case BTN_EAST:  // the Brick's A
        case BTN_WEST:  // X
            return TMENU_KEY_CONFIRM;
        case BTN_SOUTH:  // B
        case BTN_NORTH:  // Y
            return TMENU_KEY_BACK;
        default: return TMENU_KEY_NONE;
        }
    }
    if (event->type == EV_ABS && (event->code == ABS_HAT0X || event->code == ABS_HAT0Y)) {
        int *last = event->code == ABS_HAT0X ? &hats->x[position] : &hats->y[position];
        if (*last == event->value) {
            *stop = 1;
            return TMENU_KEY_NONE;
        }
        *last = event->value;
        if (event->value == 0) return TMENU_KEY_NONE;
        if (event->code == ABS_HAT0X) return event->value > 0 ? TMENU_KEY_RIGHT : TMENU_KEY_LEFT;
        return event->value > 0 ? TMENU_KEY_DOWN : TMENU_KEY_UP;
    }
    return TMENU_KEY_NONE;
}

// Closing an input node blocks for 15-20 ms in the kernel. The official library never
// closes the first node it opens or the nodes it rejects, so to keep its timing
// without leaking them, those close on a short-lived thread instead.
static void *close_thread(void *argument) {
    close((int)(intptr_t)argument);
    return NULL;
}

static void close_later(int fd) {
    pthread_t thread;
    pthread_attr_t attributes;
    pthread_attr_init(&attributes);
    pthread_attr_setdetachstate(&attributes, PTHREAD_CREATE_DETACHED);
    if (pthread_create(&thread, &attributes, close_thread, (void *)(intptr_t)fd) != 0) close(fd);
    pthread_attr_destroy(&attributes);
}

int tmenu_input_open(struct tmenu_input *input, const char *directory) {
    input->count = 0;
    DIR *dir = opendir(directory);
    if (!dir) return -1;
    struct dirent *entry;
    while ((entry = readdir(dir)) && input->count < TMENU_INPUT_DEVICES) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) continue;
        char path[300];
        snprintf(path, sizeof(path), "%s/%s", directory, entry->d_name);
        int fd = open(path, O_RDWR | O_CLOEXEC);
        if (fd < 0) continue;
        int version;
        if (ioctl(fd, EVIOCGVERSION, &version) < 0) {
            close_later(fd);
            continue;
        }
        input->fds[input->count++] = fd;
    }
    closedir(dir);
    return (int)input->count;
}

enum tmenu_key tmenu_input_read(struct tmenu_input *input, struct tmenu_hats *hats) {
    struct pollfd fds[TMENU_INPUT_DEVICES];
    for (size_t i = 0; i < input->count; ++i) fds[i] = (struct pollfd){.fd = input->fds[i], .events = POLLIN};
    if (poll(fds, (nfds_t)input->count, TMENU_INPUT_POLL_MS) <= 0) return TMENU_KEY_NONE;
    for (size_t i = 0; i < input->count; ++i) {
        if (!(fds[i].revents & POLLIN)) continue;
        struct input_event event;
        if (read(fds[i].fd, &event, sizeof(event)) < (ssize_t)sizeof(event)) return TMENU_KEY_NONE;
        int stop;
        enum tmenu_key key = tmenu_input_decode(hats, i, &event, &stop);
        if (key != TMENU_KEY_NONE || stop) return key;
    }
    return TMENU_KEY_NONE;
}

void tmenu_input_close(struct tmenu_input *input) {
    for (size_t i = 1; i < input->count; ++i) close(input->fds[i]);
    if (input->count > 0) close_later(input->fds[0]);
    input->count = 0;
}
