#define _POSIX_C_SOURCE 200809L
#include "brickos_prefs.h"

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

static const char *const view_names[MAINUI_BRICKOS_VIEW_COUNT] = {
    "desktop", "games", "favorites", "recent", "gameswitcher", "apps", "last-used",
};

const char *mainui_brickos_initial_view_name(enum mainui_brickos_initial_view view) {
    return (unsigned)view < MAINUI_BRICKOS_VIEW_COUNT ? view_names[view] : NULL;
}

int mainui_brickos_initial_view_parse(const char *name, enum mainui_brickos_initial_view *view) {
    if (!name || !view) return -1;
    for (int i = 0; i < MAINUI_BRICKOS_VIEW_COUNT; ++i) {
        if (strcmp(name, view_names[i]) == 0) { *view = (enum mainui_brickos_initial_view)i; return 0; }
    }
    return -1;
}

void mainui_brickos_preferences_init(struct mainui_brickos_preferences *preferences) {
    if (!preferences) return;
    memset(preferences, 0, sizeof(*preferences));
    preferences->initial_view = MAINUI_BRICKOS_VIEW_DESKTOP;
}

static int read_bounded(const char *path, char *text, size_t capacity, size_t *length) {
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return errno == ENOENT ? 1 : -1;
    struct stat st;
    if (fstat(fd, &st) < 0 || !S_ISREG(st.st_mode) || st.st_size < 0 || (uint64_t)st.st_size >= capacity) {
        close(fd);
        return -1;
    }
    size_t used = 0;
    while (used < capacity) {
        ssize_t n = read(fd, text + used, capacity - used);
        if (n < 0 && errno == EINTR) continue;
        if (n < 0) { close(fd); return -1; }
        if (n == 0) break;
        used += (size_t)n;
        if (used == capacity) { close(fd); return -1; }
    }
    close(fd);
    text[used] = 0;
    *length = used;
    return 0;
}

int mainui_brickos_preferences_load(struct mainui_brickos_preferences *preferences, const char *path) {
    if (!preferences || !path || !path[0]) return -1;
    mainui_brickos_preferences_init(preferences);
    char text[MAINUI_BRICKOS_PREFERENCES_BYTES];
    size_t length = 0;
    int result = read_bounded(path, text, sizeof(text), &length);
    if (result == 1) return 0;
    if (result < 0) return -1;
    struct mainui_json document;
    if (mainui_json_parse(&document, text, length) < 0) return -1;
    const struct mainui_json_member *version = mainui_json_last(&document, "version");
    const struct mainui_json_member *initial = mainui_json_last(&document, "initial_view");
    const struct mainui_json_member *presentation = mainui_json_last(&document, "presentation");
    if (!version || version->type != MAINUI_JSON_NUMBER || version->number != 1.0 ||
        (initial && (initial->type != MAINUI_JSON_STRING || initial->text_length != strlen(initial->text)))) return -1;
    enum mainui_brickos_initial_view view = MAINUI_BRICKOS_VIEW_DESKTOP;
    if (initial && mainui_brickos_initial_view_parse(initial->text, &view) < 0) return -1;
    preferences->document = document;
    preferences->initial_view = view;
    preferences->present = 1;
    if (presentation && presentation->type == MAINUI_JSON_STRING &&
        presentation->text_length == strlen(presentation->text)) {
        if (!strcmp(presentation->text, "stock")) {
            preferences->presentation = MAINUI_PRESENTATION_STOCK;
            preferences->presentation_present = 1;
        } else if (!strcmp(presentation->text, "material-you")) {
            preferences->presentation = MAINUI_PRESENTATION_MATERIAL_YOU;
            preferences->presentation_present = 1;
        }
    }
    return 0;
}

struct output {
    char *text;
    size_t size, length;
    int failed;
};

static void append(struct output *out, const char *text, size_t length) {
    if (out->failed || length > out->size - out->length) { out->failed = 1; return; }
    memcpy(out->text + out->length, text, length);
    out->length += length;
}

static void append_json_string(struct output *out, const char *text, size_t length) {
    char encoded[MAINUI_JSON_TEXT * 6 + 3];
    int n = mainui_json_escape(encoded, sizeof(encoded), text, length);
    if (n < 0) { out->failed = 1; return; }
    append(out, encoded, (size_t)n);
}

static int format(const struct mainui_brickos_preferences *preferences, enum mainui_brickos_initial_view view,
                  char *text, size_t size, size_t *length) {
    const char *name = mainui_brickos_initial_view_name(view);
    if (!preferences || !name || !text || !size || !length) return -1;
    struct output out = {text, size, 0, 0};
    append(&out, "{\"version\":1,\"initial_view\":", sizeof("{\"version\":1,\"initial_view\":") - 1);
    append_json_string(&out, name, strlen(name));
    if (preferences->presentation_present) {
        const char *presentation = preferences->presentation == MAINUI_PRESENTATION_STOCK ? "stock" : "material-you";
        append(&out, ",\"presentation\":", sizeof(",\"presentation\":") - 1);
        append_json_string(&out, presentation, strlen(presentation));
    }
    for (size_t i = 0; i < preferences->document.count; ++i) {
        const struct mainui_json_member *member = &preferences->document.members[i];
        if (strcasecmp(member->key, "version") == 0 || strcasecmp(member->key, "initial_view") == 0 ||
            strcasecmp(member->key, "presentation") == 0) continue;
        append(&out, ",", 1);
        append_json_string(&out, member->key, strlen(member->key));
        append(&out, ":", 1);
        if (member->type == MAINUI_JSON_STRING) append_json_string(&out, member->text, member->text_length);
        else append(&out, member->text, member->text_length);
    }
    append(&out, "}\n", 2);
    if (out.failed) return -1;
    *length = out.length;
    return 0;
}

int mainui_brickos_preferences_save_presentation(struct mainui_brickos_preferences *preferences,
                                                const char *path, enum mainui_presentation presentation) {
    if (!preferences || (presentation != MAINUI_PRESENTATION_STOCK &&
                         presentation != MAINUI_PRESENTATION_MATERIAL_YOU)) return -1;
    // Commit in-memory selection only after the atomic disk publication succeeds.
    struct mainui_brickos_preferences next = *preferences;
    next.presentation = presentation;
    next.presentation_present = 1;
    if (mainui_brickos_preferences_save(&next, path, next.initial_view) < 0) return -1;
    *preferences = next;
    return 0;
}

enum mainui_presentation mainui_brickos_preferences_presentation(const struct mainui_brickos_preferences *preferences,
                                                                enum mainui_presentation fallback, int explicit_choice) {
    return preferences && preferences->presentation_present && !explicit_choice ? preferences->presentation : fallback;
}

static int make_parent_directories(const char *path) {
    char parent[2048];
    size_t n = strlen(path);
    if (n >= sizeof(parent)) return -1;
    memcpy(parent, path, n + 1);
    char *slash = strrchr(parent, '/');
    if (!slash) return 0;
    if (slash == parent) return 0;
    *slash = 0;
    for (char *p = parent + 1; *p; ++p) {
        if (*p != '/') continue;
        *p = 0;
        if (mkdir(parent, 0755) < 0 && errno != EEXIST) return -1;
        *p = '/';
    }
    return mkdir(parent, 0755) == 0 || errno == EEXIST ? 0 : -1;
}

int mainui_brickos_preferences_save(struct mainui_brickos_preferences *preferences, const char *path,
                                   enum mainui_brickos_initial_view view) {
    if (!preferences || !path || !path[0] || !mainui_brickos_initial_view_name(view) ||
        make_parent_directories(path) < 0) return -1;
    struct stat destination;
    if (lstat(path, &destination) == 0) {
        if (!S_ISREG(destination.st_mode)) return -1;
    } else if (errno != ENOENT) return -1;
    char text[MAINUI_BRICKOS_PREFERENCES_BYTES], temporary[2080];
    size_t length = 0;
    if (format(preferences, view, text, sizeof(text), &length) < 0 ||
        snprintf(temporary, sizeof(temporary), "%s.tmp.XXXXXX", path) >= (int)sizeof(temporary)) return -1;
    int fd = mkstemp(temporary);
    if (fd < 0) return -1;
    (void)fcntl(fd, F_SETFD, FD_CLOEXEC);
    if (fchmod(fd, 0600) < 0) { close(fd); unlink(temporary); return -1; }
    size_t written = 0;
    int ok = 1;
    while (written < length) {
        ssize_t n = write(fd, text + written, length - written);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) { ok = 0; break; }
        written += (size_t)n;
    }
    if (ok && fsync(fd) < 0) ok = 0;
    if (close(fd) < 0) ok = 0;
    if (!ok || rename(temporary, path) < 0) { unlink(temporary); return -1; }
    preferences->initial_view = view;
    preferences->present = 1;
    return 0;
}
