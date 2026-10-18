#define _POSIX_C_SOURCE 200809L
#include "driver.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

const char *const ledd_group_names[LEDD_GROUPS] = {"lr", "m", "f1", "f2"};

static int path_of(char *out, size_t size, const char *root, const char *file, int group) {
    int n = group < 0 ? snprintf(out, size, "%s" LEDD_LED_DIR "/%s", root, file)
                      : snprintf(out, size, "%s" LEDD_LED_DIR "/%s%s", root, file, ledd_group_names[group]);
    return n > 0 && (size_t)n < size ? 0 : -1;
}

static int read_text(const char *root, const char *file, int group, char *text, size_t size) {
    char path[512];
    if (path_of(path, sizeof(path), root, file, group) < 0) return -1;
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    ssize_t n;
    do n = read(fd, text, size - 1);
    while (n < 0 && errno == EINTR);
    close(fd);
    if (n <= 0) return -1;
    text[n] = 0;
    return 0;
}

static int read_long(const char *root, const char *file, int group, long *value) {
    char text[64], *end;
    if (read_text(root, file, group, text, sizeof(text)) < 0) return -1;
    errno = 0;
    long v = strtol(text, &end, 10);
    if (end == text || errno) return -1;
    *value = v;
    return 0;
}

// The colour reads back as "RRGGBB " and a newline.
static int read_rgb(const char *root, int group, unsigned *rgb) {
    char text[64], *end;
    if (read_text(root, "effect_rgb_hex_", group, text, sizeof(text)) < 0) return -1;
    unsigned long v = strtoul(text, &end, 16);
    if (end - text != 6) return -1;
    *rgb = (unsigned)v;
    return 0;
}

int ledd_driver_read(const char *root, struct ledd_led_state *state) {
    int ok = 1;
    long v;
    memset(state, 0, sizeof(*state));
    state->enable = read_long(root, "enable", -1, &v) == 0 ? (int)v : (ok = 0, -1);
    state->effect_enable = read_long(root, "effect_enable", -1, &v) == 0 ? (int)v : (ok = 0, -1);
    for (int g = 0; g < LEDD_GROUPS; ++g) {
        struct ledd_group_state *s = &state->groups[g];
        long effect;
        s->readable = read_long(root, "effect_", g, &effect) == 0 && read_rgb(root, g, &s->rgb) == 0 &&
                      read_long(root, "effect_cycles_", g, &s->cycles) == 0 &&
                      read_long(root, "effect_duration_", g, &s->duration) == 0;
        s->effect = s->readable ? (int)effect : -1;
        ok &= s->readable;
    }
    return ok ? 0 : -1;
}

static int write_text(const char *root, const char *file, int group, const char *text) {
    char path[512];
    if (path_of(path, sizeof(path), root, file, group) < 0) return -1;
    int fd = open(path, O_WRONLY | O_TRUNC | O_CLOEXEC);
    if (fd < 0) return -1;
    size_t length = strlen(text);
    ssize_t n;
    do n = write(fd, text, length);
    while (n < 0 && errno == EINTR);
    int ok = n == (ssize_t)length;
    return close(fd) == 0 && ok ? 0 : -1;
}

int ledd_driver_write(const char *root, int group, const struct ledd_written *w) {
    char text[32];
    int failed = 0;
    if (group < 0 || group >= LEDD_GROUPS) return -1;
    // As keymon writes it: six digits and a space.
    snprintf(text, sizeof(text), "%06X \n", w->rgb & 0xFFFFFFu);
    failed |= write_text(root, "effect_rgb_hex_", group, text);
    snprintf(text, sizeof(text), "%ld\n", w->cycles);
    failed |= write_text(root, "effect_cycles_", group, text);
    snprintf(text, sizeof(text), "%ld\n", w->duration);
    failed |= write_text(root, "effect_duration_", group, text);
    // The effect number last: writing it starts the effect with the values above.
    snprintf(text, sizeof(text), "%d\n", w->effect);
    failed |= write_text(root, "effect_", group, text);
    return failed ? -1 : 0;
}

int ledd_driver_engine_on(const char *root) { return write_text(root, "effect_enable", -1, "1\n"); }

int ledd_driver_dark(const char *root, int group) {
    const struct ledd_written black = {4, 0x000000u, 1, 200};
    return ledd_driver_write(root, group, &black);
}

int ledd_driver_shows(const struct ledd_group_state *s, const struct ledd_written *w) {
    return s->readable && s->effect == w->effect && s->rgb == (w->rgb & 0xFFFFFFu);
}
