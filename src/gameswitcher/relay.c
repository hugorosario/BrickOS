#define _GNU_SOURCE
#include "relay.h"
#include <fcntl.h>
#include <linux/input.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <unistd.h>

static int has_key(const unsigned long *bits, int code) {
    return !!(bits[code / (8 * sizeof(unsigned long))] & (1UL << (code % (8 * sizeof(unsigned long)))));
}
int gs_relay_open(void) {
    for (int i = 0; i < 32; ++i) {
        char path[64], name[128] = {0};
        unsigned long keys[(KEY_MAX + 8 * sizeof(unsigned long)) / (8 * sizeof(unsigned long))] = {0};
        snprintf(path, sizeof(path), "/dev/input/event%d", i);
        int fd = open(path, O_WRONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0) continue;
        // The audio jack's node also declares 114 and 115, for the headset's buttons: the name
        // tells the keyboard node apart.
        if (ioctl(fd, EVIOCGNAME(sizeof(name)), name) >= 0 && !strcmp(name, "sunxi-keyboard") &&
            ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(keys)), keys) >= 0 && has_key(keys, KEY_VOLUMEUP) &&
            has_key(keys, KEY_VOLUMEDOWN)) return fd;
        close(fd);
    }
    return -1;
}
int gs_relay_key(int fd, unsigned short code, int value) {
    struct input_event events[2];
    memset(events, 0, sizeof(events));
    gettimeofday(&events[0].time, NULL); events[1].time = events[0].time;
    events[0].type = EV_KEY; events[0].code = code; events[0].value = value;
    events[1].type = EV_SYN; events[1].code = SYN_REPORT;
    return write(fd, events, sizeof(events)) == (ssize_t)sizeof(events) ? 0 : -1;
}
int gs_relay_release(int fd) {
    int up = gs_relay_key(fd, KEY_VOLUMEUP, 0), down = gs_relay_key(fd, KEY_VOLUMEDOWN, 0);
    return up || down ? -1 : 0;
}
void gs_relay_release_node(void) {
    int fd = gs_relay_open();
    if (fd < 0) return;
    (void)gs_relay_release(fd); close(fd);
}
