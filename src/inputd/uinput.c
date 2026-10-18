#define _GNU_SOURCE
#include "uinput.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/uinput.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <unistd.h>

const int inputd_uinput_keys[15] = {304, 305, 307, 308, 310, 311, 315, 314, 316, 317, 318, 115, 114, 59, 60};
const struct inputd_uinput_axis inputd_uinput_axes[8] = {
    {ABS_HAT0X, -1, 1}, {ABS_HAT0Y, -1, 1}, {ABS_X, -32767, 32767}, {ABS_Y, -32767, 32767},
    {ABS_Z, 0, 255}, {ABS_RX, -32767, 32767}, {ABS_RY, -32767, 32767}, {ABS_RZ, 0, 255},
};

static int failed(int fd, char *error, size_t size, const char *step) {
    snprintf(error, size, "%s: %s", step, strerror(errno));
    if (fd >= 0) close(fd);
    return -1;
}

int inputd_uinput_create(char *error, size_t size) {
    int fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) return failed(-1, error, size, "open /dev/uinput");
    static const int types[] = {EV_KEY, EV_ABS, EV_SW};
    for (size_t i = 0; i < sizeof(types) / sizeof(types[0]); ++i)
        if (ioctl(fd, UI_SET_EVBIT, types[i]) < 0) return failed(fd, error, size, "UI_SET_EVBIT");
    for (size_t i = 0; i < sizeof(inputd_uinput_keys) / sizeof(inputd_uinput_keys[0]); ++i)
        if (ioctl(fd, UI_SET_KEYBIT, inputd_uinput_keys[i]) < 0) return failed(fd, error, size, "UI_SET_KEYBIT");
    if (ioctl(fd, UI_SET_SWBIT, 1) < 0) return failed(fd, error, size, "UI_SET_SWBIT");
    for (size_t i = 0; i < sizeof(inputd_uinput_axes) / sizeof(inputd_uinput_axes[0]); ++i) {
        const struct inputd_uinput_axis *a = &inputd_uinput_axes[i];
        if (ioctl(fd, UI_SET_ABSBIT, a->code) < 0) return failed(fd, error, size, "UI_SET_ABSBIT");
        struct uinput_abs_setup setup;
        memset(&setup, 0, sizeof(setup));
        setup.code = (unsigned short)a->code;
        setup.absinfo.minimum = a->minimum;
        setup.absinfo.maximum = a->maximum;
        if (ioctl(fd, UI_ABS_SETUP, &setup) < 0) return failed(fd, error, size, "UI_ABS_SETUP");
    }
    struct uinput_setup setup;
    memset(&setup, 0, sizeof(setup));
    setup.id.bustype = BUS_USB;
    setup.id.vendor = 0x045e;
    setup.id.product = 0x028e;
    setup.id.version = 0x0114;
    snprintf(setup.name, sizeof(setup.name), "%s", INPUTD_DEVICE_NAME);
    if (ioctl(fd, UI_DEV_SETUP, &setup) < 0) return failed(fd, error, size, "UI_DEV_SETUP");
    if (ioctl(fd, UI_DEV_CREATE) < 0) return failed(fd, error, size, "UI_DEV_CREATE");
    return fd;
}

static int write_event(int fd, int type, int code, int value) {
    struct input_event ev;
    memset(&ev, 0, sizeof(ev));
    gettimeofday(&ev.time, NULL);
    ev.type = (unsigned short)type;
    ev.code = (unsigned short)code;
    ev.value = value;
    return write(fd, &ev, sizeof(ev)) == (ssize_t)sizeof(ev) ? 0 : -1;
}

int inputd_uinput_write(int fd, const struct inputd_events *events) {
    for (int i = 0; i < events->count; ++i) {
        const struct inputd_event *e = &events->ev[i];
        if (write_event(fd, e->type, e->code, e->value) < 0 || write_event(fd, EV_SYN, SYN_REPORT, 0) < 0) return -1;
    }
    return 0;
}

void inputd_uinput_destroy(int fd) {
    if (fd < 0) return;
    ioctl(fd, UI_DEV_DESTROY);
    close(fd);
}
