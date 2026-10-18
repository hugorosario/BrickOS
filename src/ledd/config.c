#define _POSIX_C_SOURCE 200809L
#include "config.h"

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static const char *const mode_names[LEDD_MODE_COUNT] = {
    "default", "battery", "cpu", "temperature", "charging", "custom",
};
static const char *const effect_names[LEDD_EFFECT_COUNT] = {
    "off", "linear", "breath", "sniff", "static", "blink1", "blink2", "blink3",
};
static const char *const colour_names[LEDD_COLOUR_COUNT] = {
    "random", "red", "green", "blue", "yellow", "purple", "cyan", "white",
};

static int find_name(const char *const *names, int count, const char *name) {
    if (!name) return -1;
    for (int i = 0; i < count; ++i)
        if (strcmp(names[i], name) == 0) return i;
    return -1;
}

const char *ledd_mode_name(enum ledd_mode mode) {
    return (unsigned)mode < LEDD_MODE_COUNT ? mode_names[mode] : NULL;
}

const char *ledd_effect_name(enum ledd_effect effect) {
    return (unsigned)effect < LEDD_EFFECT_COUNT ? effect_names[effect] : NULL;
}

const char *ledd_colour_name(enum ledd_colour colour) {
    return (unsigned)colour < LEDD_COLOUR_COUNT ? colour_names[colour] : NULL;
}

int ledd_mode_parse(const char *name, enum ledd_mode *mode) {
    int i = find_name(mode_names, LEDD_MODE_COUNT, name);
    if (i < 0 || !mode) return -1;
    *mode = (enum ledd_mode)i;
    return 0;
}

int ledd_effect_parse(const char *name, enum ledd_effect *effect) {
    int i = find_name(effect_names, LEDD_EFFECT_COUNT, name);
    if (i < 0 || !effect) return -1;
    *effect = (enum ledd_effect)i;
    return 0;
}

int ledd_colour_parse(const char *name, enum ledd_colour *colour) {
    int i = find_name(colour_names, LEDD_COLOUR_COUNT, name);
    if (i < 0 || !colour) return -1;
    *colour = (enum ledd_colour)i;
    return 0;
}

void ledd_config_defaults(struct ledd_config *config) {
    config->mode = LEDD_MODE_DEFAULT;
    config->effect = LEDD_DEFAULT_EFFECT;
    config->colour = LEDD_DEFAULT_COLOUR;
    config->time = LEDD_DEFAULT_TIME;
}

static enum ledd_config_status invalid(struct ledd_config *config, char *error, size_t error_size,
                                       const char *format, ...) {
    if (error && error_size) {
        va_list arguments;
        va_start(arguments, format);
        vsnprintf(error, error_size, format, arguments);
        va_end(arguments);
    }
    ledd_config_defaults(config);
    return LEDD_CONFIG_INVALID;
}

// The one member named key, compared exactly. Returns 0 when absent, 1 when found, -1 when it
// appears twice.
static int member(const struct mainui_json *json, const char *key, const struct mainui_json_member **out) {
    *out = NULL;
    for (size_t i = 0; i < json->count; ++i) {
        if (strcmp(json->members[i].key, key) != 0) continue;
        if (*out) return -1;
        *out = &json->members[i];
    }
    return *out ? 1 : 0;
}

static int plain_string(const struct mainui_json_member *m) {
    return m->type == MAINUI_JSON_STRING && m->text_length == strlen(m->text);
}

static const char *const known[] = {"version", "mode", "effect", "colour", "time"};

enum ledd_config_status ledd_config_parse(struct ledd_config *config, const char *text, size_t length,
                                          struct mainui_json *document, char *error, size_t error_size) {
    static struct mainui_json json;  // large; one thread parses at a time
    ledd_config_defaults(config);
    if (error && error_size) error[0] = 0;
    if (document) document->count = 0;
    if (!text || length > LEDD_CONFIG_BYTES) return invalid(config, error, error_size, "larger than %d bytes", LEDD_CONFIG_BYTES);
    if (mainui_json_parse(&json, text, length) < 0) return invalid(config, error, error_size, "not a JSON object");
    if (document) *document = json;
    const struct mainui_json_member *m[5];
    for (int i = 0; i < 5; ++i)
        if (member(&json, known[i], &m[i]) < 0) return invalid(config, error, error_size, "duplicate \"%s\"", known[i]);
    if (!m[0] || m[0]->type != MAINUI_JSON_NUMBER || m[0]->number != LEDD_CONFIG_VERSION)
        return invalid(config, error, error_size, "version is not %d", LEDD_CONFIG_VERSION);
    struct ledd_config next;
    ledd_config_defaults(&next);
    if (!m[1]) return invalid(config, error, error_size, "no mode");
    if (!plain_string(m[1]) || ledd_mode_parse(m[1]->text, &next.mode) < 0)
        return invalid(config, error, error_size, "unknown mode");
    if (m[2] && (!plain_string(m[2]) || ledd_effect_parse(m[2]->text, &next.effect) < 0))
        return invalid(config, error, error_size, "unknown effect");
    if (m[3] && (!plain_string(m[3]) || ledd_colour_parse(m[3]->text, &next.colour) < 0))
        return invalid(config, error, error_size, "unknown colour");
    if (m[4]) {
        double t = m[4]->type == MAINUI_JSON_NUMBER ? m[4]->number : 0;
        if (m[4]->type != MAINUI_JSON_NUMBER || t != (double)(int)t || t < LEDD_TIME_MIN || t > LEDD_TIME_MAX)
            return invalid(config, error, error_size, "time is not %d to %d", LEDD_TIME_MIN, LEDD_TIME_MAX);
        next.time = (int)t;
    }
    *config = next;
    return LEDD_CONFIG_OK;
}

enum ledd_config_status ledd_config_load(struct ledd_config *config, const char *path,
                                         struct mainui_json *document, char *error, size_t error_size) {
    ledd_config_defaults(config);
    if (error && error_size) error[0] = 0;
    if (document) document->count = 0;
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) {
        if (errno == ENOENT) return LEDD_CONFIG_MISSING;
        return invalid(config, error, error_size, "cannot open: %s", strerror(errno));
    }
    char text[LEDD_CONFIG_BYTES + 1];
    struct stat st;
    if (fstat(fd, &st) < 0 || !S_ISREG(st.st_mode)) {
        close(fd);
        return invalid(config, error, error_size, "not a regular file");
    }
    size_t used = 0;
    for (;;) {
        ssize_t n = read(fd, text + used, sizeof(text) - used);
        if (n < 0 && errno == EINTR) continue;
        if (n < 0) {
            int e = errno;
            close(fd);
            return invalid(config, error, error_size, "cannot read: %s", strerror(e));
        }
        if (n == 0) break;
        used += (size_t)n;
        if (used == sizeof(text)) break;
    }
    close(fd);
    if (used > LEDD_CONFIG_BYTES) return invalid(config, error, error_size, "larger than %d bytes", LEDD_CONFIG_BYTES);
    return ledd_config_parse(config, text, used, document, error, error_size);
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

static void append_text(struct output *out, const char *text) { append(out, text, strlen(text)); }

static void append_string(struct output *out, const char *bytes, size_t length) {
    char encoded[MAINUI_JSON_TEXT * 6 + 3];
    int n = mainui_json_escape(encoded, sizeof(encoded), bytes, length);
    if (n < 0) { out->failed = 1; return; }
    append(out, encoded, (size_t)n);
}

int ledd_config_format(const struct ledd_config *config, const struct mainui_json *document, char *text,
                       size_t size, size_t *length) {
    const char *mode = config ? ledd_mode_name(config->mode) : NULL;
    const char *effect = config ? ledd_effect_name(config->effect) : NULL;
    const char *colour = config ? ledd_colour_name(config->colour) : NULL;
    if (!mode || !effect || !colour || config->time < LEDD_TIME_MIN || config->time > LEDD_TIME_MAX ||
        !text || !length) return -1;
    struct output out = {text, size, 0, 0};
    char time[16];
    snprintf(time, sizeof(time), "%d", config->time);
    append_text(&out, "{\"version\":1,\"mode\":\"");
    append_text(&out, mode);
    append_text(&out, "\",\"effect\":\"");
    append_text(&out, effect);
    append_text(&out, "\",\"colour\":\"");
    append_text(&out, colour);
    append_text(&out, "\",\"time\":");
    append_text(&out, time);
    for (size_t i = 0; document && i < document->count; ++i) {
        const struct mainui_json_member *m = &document->members[i];
        int is_known = 0;
        for (int k = 0; k < 5; ++k) is_known |= strcmp(m->key, known[k]) == 0;
        if (is_known) continue;
        append_text(&out, ",");
        append_string(&out, m->key, strlen(m->key));
        append_text(&out, ":");
        if (m->type == MAINUI_JSON_STRING) append_string(&out, m->text, m->text_length);
        else append(&out, m->text, m->text_length);
    }
    append_text(&out, "}\n");
    if (out.failed || out.length > LEDD_CONFIG_BYTES) return -1;
    *length = out.length;
    return 0;
}

int ledd_config_changed(struct ledd_config_stamp *stamp, const char *path) {
    struct stat st;
    int present = stat(path, &st) == 0;
    if (!present) {
        int changed = !stamp->checked || stamp->present;
        stamp->checked = 1;
        stamp->present = 0;
        return changed;
    }
    if (stamp->checked && stamp->present && stamp->device == st.st_dev && stamp->inode == st.st_ino &&
        stamp->size == st.st_size && stamp->mtime.tv_sec == st.st_mtim.tv_sec &&
        stamp->mtime.tv_nsec == st.st_mtim.tv_nsec) return 0;
    stamp->checked = 1;
    stamp->present = 1;
    stamp->device = st.st_dev;
    stamp->inode = st.st_ino;
    stamp->size = st.st_size;
    stamp->mtime = st.st_mtim;
    return 1;
}
