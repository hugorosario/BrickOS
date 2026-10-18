#define _POSIX_C_SOURCE 200809L
#include "config.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

struct alias {
    const char *name;
    struct inputd_dest dest;
};

// The destination aliases of the version-one profile.
static const struct alias aliases[] = {
    {"KEY_VOLUMEDOWN", {INPUTD_EV_KEY, 114, 1}}, {"KEY_VOLUMEUP", {INPUTD_EV_KEY, 115, 1}},
    {"BTN_SOUTH", {INPUTD_EV_KEY, 304, 1}}, {"BTN_A", {INPUTD_EV_KEY, 304, 1}},
    {"BTN_EAST", {INPUTD_EV_KEY, 305, 1}}, {"BTN_B", {INPUTD_EV_KEY, 305, 1}},
    {"BTN_NORTH", {INPUTD_EV_KEY, 307, 1}}, {"BTN_X", {INPUTD_EV_KEY, 307, 1}},
    {"BTN_WEST", {INPUTD_EV_KEY, 308, 1}}, {"BTN_Y", {INPUTD_EV_KEY, 308, 1}},
    {"BTN_TL", {INPUTD_EV_KEY, 310, 1}}, {"BTN_TR", {INPUTD_EV_KEY, 311, 1}},
    {"BTN_SELECT", {INPUTD_EV_KEY, 314, 1}}, {"BTN_START", {INPUTD_EV_KEY, 315, 1}},
    {"BTN_MODE", {INPUTD_EV_KEY, 316, 1}}, {"BTN_THUMBL", {INPUTD_EV_KEY, 317, 1}},
    {"BTN_THUMBR", {INPUTD_EV_KEY, 318, 1}},
    {"HAT0X_LEFT", {INPUTD_EV_ABS, INPUTD_ABS_HAT0X, -1}}, {"HAT0X_RIGHT", {INPUTD_EV_ABS, INPUTD_ABS_HAT0X, 1}},
    {"HAT0Y_UP", {INPUTD_EV_ABS, INPUTD_ABS_HAT0Y, -1}}, {"HAT0Y_DOWN", {INPUTD_EV_ABS, INPUTD_ABS_HAT0Y, 1}},
    {"FN_SWITCH", {INPUTD_EV_SW, INPUTD_SW_FN, 1}},
};

int inputd_config_resolve(const char *alias, struct inputd_dest *dest) {
    if (strcmp(alias, "disabled") == 0) {
        *dest = (struct inputd_dest){0, 0, 0};
        return 0;
    }
    for (size_t i = 0; i < sizeof(aliases) / sizeof(aliases[0]); ++i) {
        if (strcmp(aliases[i].name, alias) == 0) {
            *dest = aliases[i].dest;
            return 0;
        }
    }
    return -1;
}

int inputd_config_source(const char *name) {
    for (int i = 0; i < INPUTD_SOURCES; ++i)
        if (strcmp(inputd_sources[i].name, name) == 0) return i;
    return -1;
}

void inputd_config_stock(struct inputd_config *config) {
    memset(config, 0, sizeof(*config));
    snprintf(config->active_profile, sizeof(config->active_profile), "stock");
}

struct parser {
    const char *p, *end;
    char *error;
    size_t size;
    int failed;
    long version;
    int has_version;
    char device[INPUTD_CONFIG_NAME];
    struct inputd_config_doc *doc;
};

static int fail(struct parser *ps, const char *format, ...) {
    if (!ps->failed) {
        va_list args;
        va_start(args, format);
        vsnprintf(ps->error, ps->size, format, args);
        va_end(args);
        ps->failed = 1;
    }
    return -1;
}

static void space(struct parser *ps) {
    while (ps->p < ps->end && (*ps->p == ' ' || *ps->p == '\t' || *ps->p == '\n' || *ps->p == '\r')) ++ps->p;
}

static int expect(struct parser *ps, char c) {
    space(ps);
    if (ps->p >= ps->end || *ps->p != c) return fail(ps, "invalid JSON: expected '%c' at offset %ld", c, (long)(ps->p - (ps->end - (ps->end - ps->p))));
    ++ps->p;
    return 0;
}

static int peek(struct parser *ps) {
    space(ps);
    return ps->p < ps->end ? *ps->p : -1;
}

static void put_utf8(char *out, size_t size, size_t *n, unsigned code) {
    char bytes[4];
    size_t k = 0;
    if (code < 0x80) bytes[k++] = (char)code;
    else if (code < 0x800) { bytes[k++] = (char)(0xc0 | code >> 6); bytes[k++] = (char)(0x80 | (code & 0x3f)); }
    else if (code < 0x10000) { bytes[k++] = (char)(0xe0 | code >> 12); bytes[k++] = (char)(0x80 | (code >> 6 & 0x3f)); bytes[k++] = (char)(0x80 | (code & 0x3f)); }
    else { bytes[k++] = (char)(0xf0 | code >> 18); bytes[k++] = (char)(0x80 | (code >> 12 & 0x3f)); bytes[k++] = (char)(0x80 | (code >> 6 & 0x3f)); bytes[k++] = (char)(0x80 | (code & 0x3f)); }
    for (size_t i = 0; i < k; ++i)
        if (*n + 1 < size) out[(*n)++] = bytes[i];
}

static int hex4(struct parser *ps, unsigned *value) {
    if (ps->end - ps->p < 4) return fail(ps, "invalid JSON: short \\u escape");
    *value = 0;
    for (int i = 0; i < 4; ++i) {
        char c = *ps->p++;
        *value <<= 4;
        if (c >= '0' && c <= '9') *value |= (unsigned)(c - '0');
        else if (c >= 'a' && c <= 'f') *value |= (unsigned)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') *value |= (unsigned)(c - 'A' + 10);
        else return fail(ps, "invalid JSON: bad \\u escape");
    }
    return 0;
}

// Reads a string into out; a string longer than the buffer is an error.
static int string(struct parser *ps, char *out, size_t size) {
    if (expect(ps, '"') < 0) return -1;
    size_t n = 0;
    while (ps->p < ps->end && *ps->p != '"') {
        unsigned char c = (unsigned char)*ps->p++;
        if (c < 0x20) return fail(ps, "invalid JSON: control character in string");
        if (c != '\\') {
            if (n + 1 >= size) return fail(ps, "string too long");
            out[n++] = (char)c;
            continue;
        }
        if (ps->p >= ps->end) break;
        char e = *ps->p++;
        unsigned code = 0;
        switch (e) {
        case '"': case '\\': case '/': code = (unsigned char)e; break;
        case 'b': code = '\b'; break;
        case 'f': code = '\f'; break;
        case 'n': code = '\n'; break;
        case 'r': code = '\r'; break;
        case 't': code = '\t'; break;
        case 'u':
            if (hex4(ps, &code) < 0) return -1;
            if (code >= 0xd800 && code < 0xdc00 && ps->end - ps->p >= 6 && ps->p[0] == '\\' && ps->p[1] == 'u') {
                unsigned low;
                ps->p += 2;
                if (hex4(ps, &low) < 0) return -1;
                code = low >= 0xdc00 && low < 0xe000 ? 0x10000 + ((code - 0xd800) << 10) + (low - 0xdc00) : 0xfffd;
            } else if (code >= 0xd800 && code < 0xe000) {
                code = 0xfffd;
            }
            break;
        default: return fail(ps, "invalid JSON: bad escape \\%c", e);
        }
        size_t before = n;
        put_utf8(out, size, &n, code);
        if (n == before && size > 1) return fail(ps, "string too long");
    }
    if (ps->p >= ps->end) return fail(ps, "invalid JSON: unterminated string");
    ++ps->p;
    out[n] = 0;
    return 0;
}

static int literal(struct parser *ps, const char *word) {
    size_t n = strlen(word);
    if ((size_t)(ps->end - ps->p) < n || strncmp(ps->p, word, n) != 0) return fail(ps, "invalid JSON value");
    ps->p += n;
    return 0;
}

static int is_null(struct parser *ps) {
    if (peek(ps) != 'n') return 0;
    return literal(ps, "null") == 0 ? 1 : -1;
}

static const char *kind(int c) {
    switch (c) {
    case '{': return "an object";
    case '[': return "an array";
    case '"': return "a string";
    case 't': case 'f': return "a boolean";
    default: return "a number";
    }
}

// JSON null means the zero value, so null leaves the field unset.
static int nullable_string(struct parser *ps, const char *field, char *out, size_t size) {
    int null = is_null(ps);
    if (null < 0) return -1;
    if (null) { out[0] = 0; return 0; }
    if (peek(ps) != '"') return fail(ps, "%s must be a string, not %s", field, kind(peek(ps)));
    return string(ps, out, size);
}

static int version(struct parser *ps) {
    int null = is_null(ps);
    if (null < 0) return -1;
    if (null) { ps->version = 0; ps->has_version = 1; return 0; }
    space(ps);
    const char *start = ps->p;
    if (ps->p < ps->end && *ps->p == '-') ++ps->p;
    while (ps->p < ps->end && *ps->p >= '0' && *ps->p <= '9') ++ps->p;
    if (ps->p == start || (ps->p < ps->end && (*ps->p == '.' || *ps->p == 'e' || *ps->p == 'E')))
        return fail(ps, "version must be an integer");
    char digits[24];
    size_t n = (size_t)(ps->p - start);
    if (n >= sizeof(digits)) return fail(ps, "version out of range");
    memcpy(digits, start, n);
    digits[n] = 0;
    errno = 0;
    ps->version = strtol(digits, NULL, 10);
    if (errno) return fail(ps, "version out of range");
    ps->has_version = 1;
    return 0;
}

// Calls member(ps, key, context) for each member of an object; null counts as an empty object.
typedef int (*member_fn)(struct parser *ps, const char *key, void *context);

static int object(struct parser *ps, const char *field, member_fn member, void *context) {
    int null = is_null(ps);
    if (null) return null < 0 ? -1 : 0;
    if (peek(ps) != '{') return fail(ps, "%s must be an object, not %s", field, kind(peek(ps)));
    ++ps->p;
    if (peek(ps) == '}') { ++ps->p; return 0; }
    for (;;) {
        char key[INPUTD_CONFIG_NAME];
        if (peek(ps) != '"') return fail(ps, "invalid JSON: expected a member name in %s", field);
        if (string(ps, key, sizeof(key)) < 0) return -1;
        if (expect(ps, ':') < 0 || member(ps, key, context) < 0) return -1;
        int c = peek(ps);
        if (c == ',') { ++ps->p; continue; }
        if (c == '}') { ++ps->p; return 0; }
        return fail(ps, "invalid JSON: expected ',' or '}' in %s", field);
    }
}

struct binding_context {
    struct inputd_config_profile *profile;
    const char *source;
    char alias[INPUTD_CONFIG_NAME];
    int has_key;
};

static int binding_member(struct parser *ps, const char *key, void *context) {
    struct binding_context *b = context;
    if (strcasecmp(key, "key") != 0) return fail(ps, "profile \"%s\": binding of \"%s\" has unknown field \"%s\"", b->profile->name, b->source, key);
    b->has_key = 1;
    return nullable_string(ps, "key", b->alias, sizeof(b->alias));
}

static int bindings_member(struct parser *ps, const char *key, void *context) {
    struct inputd_config_profile *profile = context;
    struct binding_context b = {profile, key, "", 0};
    if (object(ps, "binding", binding_member, &b) < 0) return -1;
    int source = inputd_config_source(key);
    if (source < 0) return fail(ps, "profile \"%s\" binds unknown physical source \"%s\"", profile->name, key);
    profile->bound[source] = 1;
    snprintf(profile->alias[source], sizeof(profile->alias[source]), "%s", b.alias);
    return 0;
}

static int profile_member(struct parser *ps, const char *key, void *context) {
    struct inputd_config_profile *profile = context;
    if (strcasecmp(key, "bindings") != 0) return fail(ps, "profile \"%s\" has unknown field \"%s\"", profile->name, key);
    // A repeated "bindings" member does not start over: earlier entries are kept and later
    // keys overwrite them.
    return object(ps, "bindings", bindings_member, profile);
}

static int profiles_member(struct parser *ps, const char *key, void *context) {
    (void)context;
    struct inputd_config_doc *doc = ps->doc;
    struct inputd_config_profile *profile = NULL;
    for (int i = 0; i < doc->count; ++i)
        if (strcmp(doc->profiles[i].name, key) == 0) profile = &doc->profiles[i];
    if (!profile) {
        if (doc->count == INPUTD_CONFIG_MAX_PROFILES) return fail(ps, "more than %d profiles", INPUTD_CONFIG_MAX_PROFILES);
        profile = &doc->profiles[doc->count++];
        memset(profile, 0, sizeof(*profile));
        snprintf(profile->name, sizeof(profile->name), "%s", key);
    } else {
        // A later duplicate profile replaces the earlier value.
        memset(profile->bound, 0, sizeof(profile->bound));
    }
    return object(ps, "profile", profile_member, profile);
}

static int top_member(struct parser *ps, const char *key, void *context) {
    (void)context;
    if (strcasecmp(key, "version") == 0) return version(ps);
    if (strcasecmp(key, "device") == 0) return nullable_string(ps, "device", ps->device, sizeof(ps->device));
    if (strcasecmp(key, "active_profile") == 0) return nullable_string(ps, "active_profile", ps->doc->active_profile, sizeof(ps->doc->active_profile));
    if (strcasecmp(key, "profiles") == 0) {
        ps->doc->count = 0;
        return object(ps, "profiles", profiles_member, NULL);
    }
    return fail(ps, "unknown field \"%s\"", key);
}

int inputd_config_doc_parse(struct inputd_config_doc *doc, const char *text, size_t length, char *error, size_t size) {
    struct parser ps;
    memset(&ps, 0, sizeof(ps));
    memset(doc, 0, sizeof(*doc));
    ps.p = text;
    ps.end = text + length;
    ps.error = error;
    ps.size = size;
    ps.doc = doc;
    if (size) error[0] = 0;
    if (peek(&ps) != '{') { fail(&ps, "the profile must be a JSON object"); return -1; }
    if (object(&ps, "the profile", top_member, NULL) < 0) return -1;
    if (peek(&ps) != -1) { fail(&ps, "extra data after the JSON object"); return -1; }
    if (ps.version != 1) { fail(&ps, "version %ld is not supported (expected 1)", ps.version); return -1; }
    if (strcmp(ps.device, "trimui-brick") != 0) { fail(&ps, "device \"%s\" is not supported (expected \"trimui-brick\")", ps.device); return -1; }
    if (!doc->active_profile[0]) { fail(&ps, "active_profile is missing"); return -1; }
    const struct inputd_config_profile *active = NULL;
    for (int i = 0; i < doc->count; ++i) {
        const struct inputd_config_profile *profile = &doc->profiles[i];
        if (strcmp(profile->name, doc->active_profile) == 0) active = profile;
        for (int s = 0; s < INPUTD_SOURCES; ++s) {
            if (!profile->bound[s]) continue;
            struct inputd_dest dest;
            if (!profile->alias[s][0]) { fail(&ps, "profile \"%s\": binding of \"%s\" has no destination", profile->name, inputd_sources[s].name); return -1; }
            if (inputd_config_resolve(profile->alias[s], &dest) < 0) { fail(&ps, "profile \"%s\": source \"%s\": unknown destination \"%s\"", profile->name, inputd_sources[s].name, profile->alias[s]); return -1; }
        }
    }
    if (!active) { fail(&ps, "active_profile \"%s\" is not in profiles", doc->active_profile); return -1; }
    return 0;
}

static int active_index(const struct inputd_config_doc *doc) {
    for (int i = 0; i < doc->count; ++i)
        if (strcmp(doc->profiles[i].name, doc->active_profile) == 0) return i;
    return -1;
}

int inputd_config_parse(struct inputd_config *config, const char *text, size_t length, char *error, size_t size) {
    static struct inputd_config_doc doc;
    if (inputd_config_doc_parse(&doc, text, length, error, size) < 0) return -1;
    const struct inputd_config_profile *active = &doc.profiles[active_index(&doc)];
    memset(config, 0, sizeof(*config));
    snprintf(config->active_profile, sizeof(config->active_profile), "%s", doc.active_profile);
    config->profiles = doc.count;
    for (int s = 0; s < INPUTD_SOURCES; ++s) {
        if (!active->bound[s]) continue;
        config->bound[s] = 1;
        inputd_config_resolve(active->alias[s], &config->dest[s]);
    }
    return 0;
}

// Reads a whole profile file. Returns the length, -2 when it does not exist, or -1.
static long read_file(const char *path, char *text, size_t capacity, char *error, size_t size) {
    FILE *file = fopen(path, "rb");
    if (!file) {
        if (errno == ENOENT) return -2;
        snprintf(error, size, "%s: %s", path, strerror(errno));
        return -1;
    }
    size_t length = fread(text, 1, capacity, file);
    int too_long = !feof(file);
    fclose(file);
    if (too_long) { snprintf(error, size, "%s: larger than %zu bytes", path, capacity); return -1; }
    return (long)length;
}

int inputd_config_load(struct inputd_config *config, const char *path, char *error, size_t size) {
    static char text[65536];
    long length = read_file(path, text, sizeof(text), error, size);
    if (length == -2) { inputd_config_stock(config); return 0; }
    if (length < 0) return -1;
    return inputd_config_parse(config, text, (size_t)length, error, size) < 0 ? -1 : 1;
}

int inputd_config_doc_load(struct inputd_config_doc *doc, const char *path, char *error, size_t size) {
    static char text[65536];
    long length = read_file(path, text, sizeof(text), error, size);
    if (length == -2) { memset(doc, 0, sizeof(*doc)); return 0; }
    if (length < 0) return -1;
    return inputd_config_doc_parse(doc, text, (size_t)length, error, size) < 0 ? -1 : 1;
}

void inputd_config_doc_new(struct inputd_config_doc *doc) {
    memset(doc, 0, sizeof(*doc));
    snprintf(doc->active_profile, sizeof(doc->active_profile), "brickos");
    snprintf(doc->profiles[0].name, sizeof(doc->profiles[0].name), "brickos");
    doc->count = 1;
}

struct output {
    char *out;
    size_t size, length;
    int overflow;
};

static void emit(struct output *o, const char *text) {
    size_t n = strlen(text);
    if (o->length + n >= o->size) { o->overflow = 1; return; }
    memcpy(o->out + o->length, text, n + 1);
    o->length += n;
}

// A JSON string: quotes, backslashes and control characters escaped, UTF-8 kept as is.
static void emit_string(struct output *o, const char *text) {
    emit(o, "\"");
    for (const unsigned char *c = (const unsigned char *)text; *c; ++c) {
        char buf[8];
        if (*c == '"' || *c == '\\') snprintf(buf, sizeof(buf), "\\%c", *c);
        else if (*c < 0x20) snprintf(buf, sizeof(buf), "\\u%04x", *c);
        else snprintf(buf, sizeof(buf), "%c", *c);
        emit(o, buf);
    }
    emit(o, "\"");
}

int inputd_config_doc_format(const struct inputd_config_doc *doc, char *out, size_t size) {
    struct output o = {out, size, 0, 0};
    if (size) out[0] = 0;
    emit(&o, "{\n  \"version\": 1,\n  \"device\": \"trimui-brick\",\n  \"active_profile\": ");
    emit_string(&o, doc->active_profile);
    emit(&o, ",\n  \"profiles\": {");
    for (int i = 0; i < doc->count; ++i) {
        const struct inputd_config_profile *profile = &doc->profiles[i];
        emit(&o, i ? ",\n    " : "\n    ");
        emit_string(&o, profile->name);
        emit(&o, ": {\n      \"bindings\": {");
        int bindings = 0;
        for (int s = 0; s < INPUTD_SOURCES; ++s) {
            if (!profile->bound[s]) continue;
            emit(&o, bindings++ ? ",\n        " : "\n        ");
            emit_string(&o, inputd_sources[s].name);
            emit(&o, ": { \"key\": ");
            emit_string(&o, profile->alias[s]);
            emit(&o, " }");
        }
        emit(&o, bindings ? "\n      }\n    }" : "}\n    }");
    }
    emit(&o, doc->count ? "\n  }\n}\n" : "}\n}\n");
    return o.overflow ? -1 : (int)o.length;
}

const char *inputd_config_doc_get(const struct inputd_config_doc *doc, int source) {
    int active = active_index(doc);
    if (active < 0 || source < 0 || source >= INPUTD_SOURCES || !doc->profiles[active].bound[source]) return NULL;
    return doc->profiles[active].alias[source];
}

int inputd_config_doc_set(struct inputd_config_doc *doc, int source, const char *alias) {
    int active = active_index(doc);
    struct inputd_dest dest;
    if (active < 0 || source < 0 || source >= INPUTD_SOURCES || inputd_config_resolve(alias, &dest) < 0) return -1;
    struct inputd_config_profile *profile = &doc->profiles[active];
    profile->bound[source] = 1;
    snprintf(profile->alias[source], sizeof(profile->alias[source]), "%s", alias);
    return 0;
}

void inputd_config_doc_unset(struct inputd_config_doc *doc, int source) {
    int active = active_index(doc);
    if (active < 0 || source < 0 || source >= INPUTD_SOURCES) return;
    doc->profiles[active].bound[source] = 0;
    doc->profiles[active].alias[source][0] = 0;
}

void inputd_config_stamp(const char *path, struct inputd_config_stamp *stamp) {
    struct stat st;
    memset(stamp, 0, sizeof(*stamp));
    if (stat(path, &st) != 0) return;
    stamp->present = 1;
    stamp->device = (unsigned long long)st.st_dev;
    stamp->inode = (unsigned long long)st.st_ino;
    stamp->size = (long long)st.st_size;
    stamp->seconds = (long long)st.st_mtim.tv_sec;
    stamp->nanoseconds = st.st_mtim.tv_nsec;
}

int inputd_config_stamp_same(const struct inputd_config_stamp *a, const struct inputd_config_stamp *b) {
    if (a->present != b->present) return 0;
    return !a->present || (a->device == b->device && a->inode == b->inode && a->size == b->size &&
                           a->seconds == b->seconds && a->nanoseconds == b->nanoseconds);
}
