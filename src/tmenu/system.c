#define _DEFAULT_SOURCE
#include "system.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <unistd.h>

#define KEYSHM_BYTES 92

const struct tmenu_system_paths tmenu_system_default_paths = {
    .system_json = "/mnt/UDISK/system.json",
    .res = "/usr/trimui/res/",
    .rumble = "/sys/class/gpio/gpio227/value",
    .color_dir = NULL,
    .keyshm_key = 0x4e4f4d4b,
};

static char language[64];

const char *tmenu_system_language(void) {
    return language[0] ? language : "en.lang";
}

char *tmenu_read_file(const char *path, size_t *length) {
    FILE *file = fopen(path, "r");
    if (!file) return NULL;
    size_t size = 0, capacity = 4096;
    char *data = malloc(capacity + 1);
    while (data) {
        size_t n = fread(data + size, 1, capacity - size, file);
        size += n;
        if (n == 0) break;
        if (size == capacity) {
            char *grown = realloc(data, capacity * 2 + 1);
            if (!grown) { free(data); data = NULL; break; }
            data = grown;
            capacity *= 2;
        }
    }
    fclose(file);
    if (!data) return NULL;
    data[size] = 0;
    if (length) *length = size;
    return data;
}

// Writes text to a sysfs attribute the way the official library does: fopen "w",
// one fwrite, fclose.
static void write_attribute(const char *path, const char *text, size_t length) {
    FILE *file = fopen(path, "w");
    if (!file) return;
    fwrite(text, 1, length, file);
    fclose(file);
}

static void write_color(struct tmenu_system *s, enum tmenu_color color, int setting) {
    char text[32], path[300];
    int n = snprintf(text, sizeof(text), "%d", tmenu_color_value(color, setting));
    const char *target = tmenu_color_path(color);
    if (s->paths.color_dir) {
        snprintf(path, sizeof(path), "%s/%s", s->paths.color_dir, strrchr(target, '/') + 1);
        target = path;
    }
    write_attribute(target, text, (size_t)n);
}

static void attach_keyshm(struct tmenu_system *s) {
    int id = shmget(s->paths.keyshm_key, KEYSHM_BYTES, IPC_CREAT | IPC_EXCL | 0666);
    if (id < 0) id = shmget(s->paths.keyshm_key, KEYSHM_BYTES, IPC_CREAT | 0666);
    if (id < 0) return;
    void *memory = shmat(id, NULL, 0);
    s->keyshm = memory == (void *)-1 ? NULL : memory;
}

int tmenu_system_keyshm_get(struct tmenu_system *s, int index) {
    return s->keyshm && index >= 0 && index < TMENU_KEYSHM_VALUES ? s->keyshm[index] : 0;
}

void tmenu_system_keyshm_set(struct tmenu_system *s, int index, int value) {
    if (s->keyshm && index >= 0 && index < TMENU_KEYSHM_VALUES) s->keyshm[index] = value;
}

void tmenu_system_open(struct tmenu_system *s, const struct tmenu_system_paths *paths) {
    memset(s, 0, sizeof(*s));
    s->paths = *paths;
    attach_keyshm(s);
    size_t length = 0;
    char *text = tmenu_read_file(s->paths.system_json, &length);
    tmenu_settings_parse(&s->settings, text, length);
    free(text);
    for (size_t i = 0; i < s->settings.step_count; ++i) {
        const struct tmenu_step *step = &s->settings.steps[i];
        switch (step->kind) {
        case TMENU_STEP_KEYSHM: tmenu_system_keyshm_set(s, step->index, step->value); break;
        case TMENU_STEP_LANGUAGE: {
            snprintf(language, sizeof(language), "%s", s->settings.language);
            const char *env = tmenu_language_env(language);
            if (env) putenv((char *)env);
            break;
        }
        case TMENU_STEP_THEME:
            if (access(s->settings.theme, F_OK) == 0) s->theme = s->settings.theme;
            break;
        case TMENU_STEP_COLOR: write_color(s, (enum tmenu_color)step->index, step->value); break;
        }
    }
}

void tmenu_system_rumble_off(struct tmenu_system *s) {
    write_attribute(s->paths.rumble, "0", 1);
}

void tmenu_system_close(struct tmenu_system *s) {
    if (s->keyshm) shmdt(s->keyshm);
    s->keyshm = NULL;
}
