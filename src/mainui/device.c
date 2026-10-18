#define _POSIX_C_SOURCE 200809L
#include "device.h"

#include <fcntl.h>
#include <linux/fb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/shm.h>
#include <tinyalsa/asoundlib.h>
#include <unistd.h>

#define KEYMON_SHM_KEY 0x4e4f4d4b
#define DISP_LCD_SET_BRIGHTNESS 0x102
// Commands run with the firmware libraries ahead of any SD library path the frontend inherits.
#define CLEAN_LIBRARY_PATH "LD_LIBRARY_PATH=/usr/trimui/lib:/usr/lib "

#define DISP_LAYER_SET_CONFIG 0x47
#define DISP_LAYER_GET_CONFIG 0x48

// One disp_layer_config (184 bytes; channel at 0xac, alpha mode and value at 5 and 6) for
// screen 0, as the official LayerConfig passes it.
static int layer_config(int fd, unsigned long request, unsigned char config[256]) {
    unsigned long args[4] = {0, (unsigned long)config, 1, 0};
    return ioctl(fd, request, args);
}

int mainui_device_ui_layer(struct mainui_device *d) {
    unsigned char config[256] = {0};
    unsigned int channel = 1;
    if (d->disp_fd < 0) return -1;
    memcpy(config + 0xac, &channel, sizeof(channel));
    if (layer_config(d->disp_fd, DISP_LAYER_GET_CONFIG, config) != 0) return -1;
    config[5] = 1;    // global alpha
    config[6] = 255;
    if (layer_config(d->disp_fd, DISP_LAYER_SET_CONFIG, config) != 0) return -1;
    return layer_config(d->disp_fd, DISP_LAYER_GET_CONFIG, config) == 0 ? 0 : -1;
}

// The page on screen: its byte offset in /dev/fb0 and size, with the mode in the copy's fields.
static int screen_page(int fd, struct mainui_screen_copy *mode, off_t *offset) {
    struct fb_var_screeninfo var;
    struct fb_fix_screeninfo fix;
    if (ioctl(fd, FBIOGET_VSCREENINFO, &var) != 0 || ioctl(fd, FBIOGET_FSCREENINFO, &fix) != 0) return -1;
    if (!var.xres || !var.yres || var.yres > 4096 || var.bits_per_pixel % 8 || !fix.line_length ||
        (size_t)var.xres * (var.bits_per_pixel / 8) > fix.line_length) return -1;
    size_t bytes = (size_t)var.yres * fix.line_length;
    size_t start = (size_t)var.yoffset * fix.line_length + (size_t)var.xoffset * (var.bits_per_pixel / 8);
    if (start + bytes > fix.smem_len) return -1;
    mode->bytes = bytes;
    mode->xres = var.xres;
    mode->yres = var.yres;
    mode->bits = var.bits_per_pixel;
    mode->line = fix.line_length;
    *offset = (off_t)start;
    return 0;
}

int mainui_device_screen_save(struct mainui_screen_copy *copy) {
    memset(copy, 0, sizeof(*copy));
    int fd = open("/dev/fb0", O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    off_t offset;
    int result = -1;
    if (screen_page(fd, copy, &offset) == 0 && (copy->pixels = malloc(copy->bytes)) &&
        pread(fd, copy->pixels, copy->bytes, offset) == (ssize_t)copy->bytes) result = 0;
    close(fd);
    if (result < 0) {
        free(copy->pixels);
        memset(copy, 0, sizeof(*copy));
    }
    return result;
}

int mainui_device_screen_restore(struct mainui_screen_copy *copy) {
    int result = -1;
    int fd = copy->pixels ? open("/dev/fb0", O_RDWR | O_CLOEXEC) : -1;
    if (fd >= 0) {
        struct mainui_screen_copy now;
        off_t offset;
        if (screen_page(fd, &now, &offset) == 0 && now.bytes == copy->bytes && now.xres == copy->xres &&
            now.yres == copy->yres && now.bits == copy->bits && now.line == copy->line &&
            pwrite(fd, copy->pixels, copy->bytes, offset) == (ssize_t)copy->bytes) result = 0;
        close(fd);
    }
    free(copy->pixels);
    memset(copy, 0, sizeof(*copy));
    return result;
}

int mainui_device_open(struct mainui_device *d) {
    memset(d, 0, sizeof(*d));
    d->disp_fd = open("/dev/disp", O_RDWR | O_CLOEXEC);
    int id = shmget(KEYMON_SHM_KEY, MAINUI_SHM_VALUES * sizeof(int), 0);
    if (id >= 0) {
        void *memory = shmat(id, NULL, 0);
        if (memory != (void *)-1) d->keymon = memory;
    }
    d->mixer = mixer_open(0);
    return 0;
}

void mainui_device_close(struct mainui_device *d) {
    if (d->keymon) shmdt(d->keymon);
    if (d->disp_fd >= 0) close(d->disp_fd);
    if (d->mixer) mixer_close(d->mixer);
    memset(d, 0, sizeof(*d));
    d->disp_fd = -1;
}

static int shm_set(void *context, int index, int value) {
    struct mainui_device *d = context;
    if (!d->keymon || index < 0 || index >= MAINUI_SHM_VALUES) return -1;
    d->keymon[index] = value;
    return 0;
}

static int shm_get(void *context, int index, int *value) {
    struct mainui_device *d = context;
    if (!d->keymon || index < 0 || index >= MAINUI_SHM_VALUES) return -1;
    *value = d->keymon[index];
    return 0;
}

static int write_bytes(void *context, const char *path, const void *value, size_t length) {
    (void)context;
    int flags = O_WRONLY | O_CLOEXEC;
    // osdd consumes and removes each request; publish a fresh complete message.
    if (!strcmp(path, "/tmp/trimui_osd/osd_toast_msg")) flags |= O_CREAT | O_TRUNC;
    int fd = open(path, flags, 0644);
    if (fd < 0) return -1;
    int ok = write(fd, value, length) == (ssize_t)length;
    return close(fd) == 0 && ok ? 0 : -1;
}

static int write_file(void *context, const char *path, const char *value) {
    return write_bytes(context, path, value, strlen(value));
}

static int lcd_brightness(void *context, int value) {
    struct mainui_device *d = context;
    unsigned long args[4] = {0, (unsigned long)value, 0, 0};
    return d->disp_fd >= 0 && ioctl(d->disp_fd, DISP_LCD_SET_BRIGHTNESS, args) >= 0 ? 0 : -1;
}

static int mixer_set(void *context, int control, int value) {
    struct mainui_device *d = context;
    if (!d->mixer) return -1;
    struct mixer_ctl *ctl = control == MAINUI_MIXER_DIGITAL_VOLUME ? mixer_get_ctl_by_name(d->mixer, "digital volume") : NULL;
    if (!ctl) ctl = mixer_get_ctl(d->mixer, (unsigned)control);
    if (!ctl) return -1;
    unsigned values = mixer_ctl_get_num_values(ctl);
    for (unsigned i = 0; i < values; ++i)
        if (mixer_ctl_set_value(ctl, i, value) != 0) return -1;
    return 0;
}

static int mixer_card_set(void *context, int card, int control, int value) {
    (void)context;
    struct mixer *mixer = mixer_open((unsigned)card);
    if (!mixer) return -1;
    struct mixer_ctl *ctl = mixer_get_ctl(mixer, (unsigned)control);
    int result = ctl ? 0 : -1;
    unsigned values = ctl ? mixer_ctl_get_num_values(ctl) : 0;
    for (unsigned i = 0; i < values && result == 0; ++i)
        if (mixer_ctl_set_value(ctl, i, value) != 0) result = -1;
    mixer_close(mixer);
    return result;
}

static int run(void *context, const char *command) {
    (void)context;
    size_t length = strlen(CLEAN_LIBRARY_PATH) + strlen(command) + 1;
    char *line = malloc(length);
    if (!line) return -1;
    snprintf(line, length, "%s%s", CLEAN_LIBRARY_PATH, command);
    int status = system(line);
    free(line);
    return status == 0 ? 0 : -1;
}

void mainui_device_ops(struct mainui_device *d, struct mainui_effect_ops *ops) {
    ops->context = d;
    ops->shm_set = shm_set;
    ops->shm_get = shm_get;
    ops->write_file = write_file;
    ops->lcd_brightness = lcd_brightness;
    ops->mixer_set = mixer_set;
    ops->run = run;
    ops->write_bytes = write_bytes;
    ops->mixer_card_set = mixer_card_set;
}
