#define _POSIX_C_SOURCE 200809L
#include "profile.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

// ---- Parsing -------------------------------------------------------------------------------
// Standard JSON, strictly: the profile comes from a frontend, not from a stock producer, so none
// of the stock record quirks apply.

#define PROFILE_DEPTH 16

struct parser {
    const unsigned char *data;
    size_t length;
    size_t position;
    char *error;
    size_t error_size;
};

static int fail(struct parser *p, const char *format, ...) {
    if (p->error != NULL && p->error_size > 0) {
        va_list arguments;
        va_start(arguments, format);
        vsnprintf(p->error, p->error_size, format, arguments);
        va_end(arguments);
    }
    return -1;
}

static void space(struct parser *p) {
    while (p->position < p->length) {
        unsigned char c = p->data[p->position];
        if (c != ' ' && c != '\t' && c != '\r' && c != '\n') break;
        ++p->position;
    }
}

static int expect(struct parser *p, char c) {
    space(p);
    if (p->position >= p->length || p->data[p->position] != (unsigned char)c)
        return fail(p, "expected '%c' at offset %zu", c, p->position);
    ++p->position;
    return 0;
}

static int hex_digit(unsigned char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static int put(struct parser *p, char *out, size_t capacity, size_t *written, unsigned value) {
    if (out == NULL) return 0;  // A skipped string.
    if (*written + 1 >= capacity) return fail(p, "string too long at offset %zu", p->position);
    out[(*written)++] = (char)value;
    return 0;
}

static int unit(struct parser *p, unsigned *value) {
    if (p->position + 4 > p->length) return fail(p, "incomplete unicode escape");
    *value = 0;
    for (int i = 0; i < 4; ++i) {
        int digit = hex_digit(p->data[p->position++]);
        if (digit < 0) return fail(p, "invalid unicode escape");
        *value = (*value << 4) | (unsigned)digit;
    }
    return 0;
}

static int utf8(struct parser *p, char *out, size_t capacity, size_t *written, unsigned value) {
    if (value < 0x80) return put(p, out, capacity, written, value);
    if (value < 0x800)
        return put(p, out, capacity, written, 0xc0 | (value >> 6)) ||
               put(p, out, capacity, written, 0x80 | (value & 0x3f)) ? -1 : 0;
    if (value < 0x10000)
        return put(p, out, capacity, written, 0xe0 | (value >> 12)) ||
               put(p, out, capacity, written, 0x80 | ((value >> 6) & 0x3f)) ||
               put(p, out, capacity, written, 0x80 | (value & 0x3f)) ? -1 : 0;
    return put(p, out, capacity, written, 0xf0 | (value >> 18)) ||
           put(p, out, capacity, written, 0x80 | ((value >> 12) & 0x3f)) ||
           put(p, out, capacity, written, 0x80 | ((value >> 6) & 0x3f)) ||
           put(p, out, capacity, written, 0x80 | (value & 0x3f)) ? -1 : 0;
}

// Reads a string into out, or skips it with out NULL.
static int string(struct parser *p, char *out, size_t capacity) {
    space(p);
    if (p->position >= p->length || p->data[p->position] != '"')
        return fail(p, "expected a string at offset %zu", p->position);
    ++p->position;
    size_t written = 0;
    while (p->position < p->length) {
        unsigned c = p->data[p->position++];
        if (c == '"') {
            if (out != NULL) out[written] = 0;
            return 0;
        }
        if (c < 0x20) return fail(p, "control character in a string");
        if (c != '\\') {
            if (put(p, out, capacity, &written, c) < 0) return -1;
            continue;
        }
        if (p->position >= p->length) break;
        c = p->data[p->position++];
        switch (c) {
            case '"': case '\\': case '/': break;
            case 'b': c = '\b'; break;
            case 'f': c = '\f'; break;
            case 'n': c = '\n'; break;
            case 'r': c = '\r'; break;
            case 't': c = '\t'; break;
            case 'u': {
                unsigned value, low;
                if (unit(p, &value) < 0) return -1;
                if (value >= 0xd800 && value <= 0xdbff) {
                    if (p->position + 2 > p->length || p->data[p->position] != '\\' ||
                        p->data[p->position + 1] != 'u') return fail(p, "missing low surrogate");
                    p->position += 2;
                    if (unit(p, &low) < 0) return -1;
                    if (low < 0xdc00 || low > 0xdfff) return fail(p, "invalid low surrogate");
                    value = 0x10000 + ((value - 0xd800) << 10) + (low - 0xdc00);
                } else if (value >= 0xdc00 && value <= 0xdfff) {
                    return fail(p, "unexpected low surrogate");
                }
                if (value == 0) return fail(p, "NUL in a string");
                if (utf8(p, out, capacity, &written, value) < 0) return -1;
                continue;
            }
            default: return fail(p, "invalid string escape");
        }
        if (put(p, out, capacity, &written, c) < 0) return -1;
    }
    return fail(p, "unterminated string");
}

// A JSON number, which must be an integer within [minimum, maximum] when out is not NULL.
static int number(struct parser *p, int *out, int minimum, int maximum) {
    space(p);
    size_t start = p->position;
    if (p->position < p->length && p->data[p->position] == '-') ++p->position;
    size_t digits = p->position;
    while (p->position < p->length && p->data[p->position] >= '0' && p->data[p->position] <= '9')
        ++p->position;
    if (p->position == digits) return fail(p, "expected a number at offset %zu", start);
    int integral = 1;
    if (p->position < p->length && p->data[p->position] == '.') {
        integral = 0;
        size_t fraction = ++p->position;
        while (p->position < p->length && p->data[p->position] >= '0' && p->data[p->position] <= '9')
            ++p->position;
        if (p->position == fraction) return fail(p, "invalid number at offset %zu", start);
    }
    if (p->position < p->length && (p->data[p->position] == 'e' || p->data[p->position] == 'E')) {
        integral = 0;
        ++p->position;
        if (p->position < p->length && (p->data[p->position] == '+' || p->data[p->position] == '-'))
            ++p->position;
        size_t exponent = p->position;
        while (p->position < p->length && p->data[p->position] >= '0' && p->data[p->position] <= '9')
            ++p->position;
        if (p->position == exponent) return fail(p, "invalid number at offset %zu", start);
    }
    if (out == NULL) return 0;
    if (!integral || p->position - start > 10) return fail(p, "expected an integer at offset %zu", start);
    char text[16];
    memcpy(text, p->data + start, p->position - start);
    text[p->position - start] = 0;
    long value = strtol(text, NULL, 10);
    if (value < minimum || value > maximum)
        return fail(p, "%ld is outside %d..%d at offset %zu", value, minimum, maximum, start);
    *out = (int)value;
    return 0;
}

static int literal(struct parser *p, const char *word) {
    size_t length = strlen(word);
    if (p->length - p->position < length || memcmp(p->data + p->position, word, length) != 0)
        return fail(p, "unexpected value at offset %zu", p->position);
    p->position += length;
    return 0;
}

// Skips any value: members the contract does not define.
static int skip(struct parser *p, int depth) {
    if (depth > PROFILE_DEPTH) return fail(p, "values nested too deeply");
    space(p);
    if (p->position >= p->length) return fail(p, "missing value");
    unsigned char c = p->data[p->position];
    if (c == '"') return string(p, NULL, 0);
    if (c == '{' || c == '[') {
        char close = c == '{' ? '}' : ']';
        ++p->position;
        space(p);
        if (p->position < p->length && p->data[p->position] == (unsigned char)close) {
            ++p->position;
            return 0;
        }
        for (;;) {
            if (c == '{' && (string(p, NULL, 0) < 0 || expect(p, ':') < 0)) return -1;
            if (skip(p, depth + 1) < 0) return -1;
            space(p);
            if (p->position < p->length && p->data[p->position] == ',') { ++p->position; continue; }
            return expect(p, close);
        }
    }
    if (c == 't') return literal(p, "true");
    if (c == 'f') return literal(p, "false");
    if (c == 'n') return literal(p, "null");
    return number(p, NULL, 0, 0);
}

// "RRGGBB" or "RRGGBBAA", with or without a leading "#".
static int color(struct parser *p, unsigned char out[4]) {
    char text[16];
    if (string(p, text, sizeof(text)) < 0) return -1;
    const char *digits = text[0] == '#' ? text + 1 : text;
    size_t length = strlen(digits);
    if (length != 6 && length != 8) return fail(p, "a color is RRGGBB or RRGGBBAA");
    out[3] = 255;
    for (size_t i = 0; i < length; i += 2) {
        int high = hex_digit((unsigned char)digits[i]), low = hex_digit((unsigned char)digits[i + 1]);
        if (high < 0 || low < 0) return fail(p, "a color is RRGGBB or RRGGBBAA");
        out[i / 2] = (unsigned char)(high * 16 + low);
    }
    return 0;
}

static int path(struct parser *p, char *out, size_t capacity) {
    if (string(p, out, capacity) < 0) return -1;
    if (out[0] != '/') return fail(p, "a path must be absolute");
    return 0;
}

int osdd_profile_parse(const unsigned char *data, size_t length, struct osdd_profile *profile,
                       char *error, size_t error_size) {
    struct parser parser = {data, length, 0, error, error_size}, *p = &parser;
    if (data == NULL || profile == NULL) return fail(p, "missing profile input");
    memset(profile, 0, sizeof(*profile));
    profile->font_size = OSDD_PROFILE_DEFAULT_FONT_SIZE;
    memset(profile->font_color, 255, sizeof(profile->font_color));
    profile->padding = OSDD_PROFILE_DEFAULT_PADDING;
    if (expect(p, '{') < 0) return -1;
    space(p);
    if (p->position < p->length && p->data[p->position] == '}') {
        ++p->position;
    } else {
        for (;;) {
            char key[64];
            // A long key is not one of ours; it is skipped with its value.
            size_t key_start = p->position;
            if (string(p, key, sizeof(key)) < 0) {
                p->position = key_start;
                if (string(p, NULL, 0) < 0) return -1;
                key[0] = 0;
            }
            if (expect(p, ':') < 0) return -1;
            int result;
            if (strcmp(key, "fontPath") == 0) {
                result = path(p, profile->font_path, sizeof(profile->font_path));
            } else if (strcmp(key, "fontSize") == 0) {
                result = number(p, &profile->font_size, 8, 256);
            } else if (strcmp(key, "fontColor") == 0) {
                result = color(p, profile->font_color);
                profile->has_font_color = result == 0;
            } else if (strcmp(key, "backgroundPath") == 0) {
                result = path(p, profile->background_path, sizeof(profile->background_path));
            } else if (strcmp(key, "backgroundColor") == 0) {
                result = color(p, profile->background_color);
                profile->has_background_color = result == 0;
            } else if (strcmp(key, "backgroundMargins") == 0) {
                result = expect(p, '[');
                for (int i = 0; result == 0 && i < 4; ++i)
                    result = (i > 0 ? expect(p, ',') : 0) || number(p, &profile->margins[i], 0, 512) ? -1 : 0;
                if (result == 0) result = expect(p, ']');
                if (result < 0) fail(p, "backgroundMargins must be four integers 0..512");
            } else if (strcmp(key, "padding") == 0) {
                result = number(p, &profile->padding, 0, 512);
            } else if (strcmp(key, "iconColor") == 0) {
                result = color(p, profile->icon_color);
                profile->has_icon_color = result == 0;
            } else if (strcmp(key, "selectorColor") == 0) {
                result = color(p, profile->selector_color);
                profile->has_selector_color = result == 0;
            } else if (strcmp(key, "secondaryColor") == 0) {
                result = color(p, profile->secondary_color);
                profile->has_secondary_color = result == 0;
            } else if (strcmp(key, "selectedColor") == 0) {
                result = color(p, profile->selected_color);
                profile->has_selected_color = result == 0;
            } else {
                result = skip(p, 1);
            }
            if (result < 0) return -1;
            space(p);
            if (p->position < p->length && p->data[p->position] == ',') { ++p->position; continue; }
            if (expect(p, '}') < 0) return -1;
            break;
        }
    }
    space(p);
    if (p->position != p->length) return fail(p, "content after the profile at offset %zu", p->position);
    return 0;
}

// ---- Loading -------------------------------------------------------------------------------

void osdd_profile_free(struct osdd_profile *profile, const struct osdd_profile_assets *assets) {
    if (profile == NULL) return;
    if (profile->font != NULL && assets != NULL && assets->close_font != NULL)
        assets->close_font(assets->context, profile->font);
    if (profile->background != NULL && assets != NULL && assets->close_image != NULL)
        assets->close_image(assets->context, profile->background);
    free(profile);
}

static int load_fail(char *error, size_t error_size, const char *format, const char *detail) {
    if (error != NULL && error_size > 0) snprintf(error, error_size, format, detail);
    return -1;
}

int osdd_profile_read(const char *path, struct osdd_profile *profile, char *error, size_t error_size) {
    if (path == NULL || profile == NULL) return load_fail(error, error_size, "%s", "missing profile input");
    int descriptor = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (descriptor < 0) return load_fail(error, error_size, "profile: %s", strerror(errno));
    struct stat st;
    if (fstat(descriptor, &st) < 0 || !S_ISREG(st.st_mode)) {
        close(descriptor);
        return load_fail(error, error_size, "%s", "profile is not a regular file");
    }
    static unsigned char data[OSDD_PROFILE_MAX_BYTES + 1];
    size_t length = 0;
    for (;;) {
        ssize_t count = read(descriptor, data + length, sizeof(data) - length);
        if (count < 0 && errno == EINTR) continue;
        if (count < 0) {
            close(descriptor);
            return load_fail(error, error_size, "profile: %s", strerror(errno));
        }
        if (count == 0) break;
        length += (size_t)count;
        if (length == sizeof(data)) break;
    }
    close(descriptor);
    if (length > OSDD_PROFILE_MAX_BYTES)
        return load_fail(error, error_size, "%s", "profile exceeds its size limit");
    return osdd_profile_parse(data, length, profile, error, error_size);
}

int osdd_profile_load(const char *path, const struct osdd_profile_assets *assets,
                      struct osdd_profile **profile, char *error, size_t error_size) {
    if (profile != NULL) *profile = NULL;
    if (path == NULL || assets == NULL || assets->open_font == NULL || profile == NULL)
        return load_fail(error, error_size, "%s", "missing profile loader input");
    struct osdd_profile *loaded = malloc(sizeof(*loaded));
    if (loaded == NULL) return load_fail(error, error_size, "%s", "out of memory");
    if (osdd_profile_read(path, loaded, error, error_size) < 0) {
        free(loaded);
        return -1;
    }
    // A profile is used whole or not at all: its typeface and image must open now.
    const char *font = loaded->font_path[0] ? loaded->font_path : OSDD_PROFILE_STOCK_FONT;
    loaded->font = assets->open_font(assets->context, font, loaded->font_size);
    if (loaded->font == NULL) {
        load_fail(error, error_size, "typeface cannot be opened: %s", font);
        osdd_profile_free(loaded, assets);
        return -1;
    }
    if (loaded->background_path[0]) {
        loaded->background = assets->open_image != NULL ?
                             assets->open_image(assets->context, loaded->background_path) : NULL;
        if (loaded->background == NULL) {
            load_fail(error, error_size, "background cannot be opened: %s", loaded->background_path);
            osdd_profile_free(loaded, assets);
            return -1;
        }
    }
    *profile = loaded;
    return 0;
}

// ---- The loader ----------------------------------------------------------------------------

static int stat_file(const char *path, struct stat *st) { return stat(path, st); }

void osdd_profile_loader_init(struct osdd_profile_loader *loader, const char *path,
                              const struct osdd_profile_assets *assets,
                              osdd_profile_in_use in_use, void *in_use_context) {
    memset(loader, 0, sizeof(*loader));
    loader->path = path;
    if (assets != NULL) loader->assets = *assets;
    loader->in_use = in_use;
    loader->in_use_context = in_use_context;
    loader->stat_file = stat_file;
}

void osdd_profile_loader_collect(struct osdd_profile_loader *loader) {
    size_t kept = 0;
    for (size_t i = 0; i < loader->retired_count; ++i) {
        struct osdd_profile *profile = loader->retired[i];
        if (loader->in_use != NULL && loader->in_use(loader->in_use_context, profile))
            loader->retired[kept++] = profile;
        else
            osdd_profile_free(profile, &loader->assets);
    }
    loader->retired_count = kept;
}

// Replaces the active profile. Returns 1 when it changed.
static int replace(struct osdd_profile_loader *loader, struct osdd_profile *next) {
    int changed = loader->current != NULL || next != NULL;
    if (loader->current != NULL) loader->retired[loader->retired_count++] = loader->current;
    loader->current = next;
    return changed;
}

int osdd_profile_loader_tick(struct osdd_profile_loader *loader, uint64_t now_ms) {
    if (loader->checked && now_ms < loader->next_check_ms) return 0;
    loader->next_check_ms = now_ms + OSDD_PROFILE_CHECK_MS;
    osdd_profile_loader_collect(loader);
    // A replaced profile must stay alive while a toast uses it; with no room, try again later.
    if (loader->retired_count == OSDD_PROFILE_RETIRED) return 0;
    loader->checked = 1;
    struct stat st;
    if (loader->stat_file(loader->path, &st) != 0 || !S_ISREG(st.st_mode)) {
        int was_present = loader->present;
        loader->present = 0;
        if (!was_present) return 0;
        snprintf(loader->error, sizeof(loader->error), "profile removed");
        return replace(loader, NULL);
    }
    if (loader->present && st.st_mtim.tv_sec == loader->mtime.tv_sec &&
        st.st_mtim.tv_nsec == loader->mtime.tv_nsec && st.st_size == loader->size &&
        st.st_ino == loader->inode) return 0;
    loader->present = 1;
    loader->mtime = st.st_mtim;
    loader->size = st.st_size;
    loader->inode = st.st_ino;
    struct osdd_profile *next = NULL;
    loader->error[0] = 0;
    if (osdd_profile_load(loader->path, &loader->assets, &next, loader->error, sizeof(loader->error)) < 0)
        next = NULL;
    return replace(loader, next);
}

void osdd_profile_loader_close(struct osdd_profile_loader *loader) {
    if (loader == NULL) return;
    osdd_profile_free(loader->current, &loader->assets);
    loader->current = NULL;
    for (size_t i = 0; i < loader->retired_count; ++i) osdd_profile_free(loader->retired[i], &loader->assets);
    loader->retired_count = 0;
}
