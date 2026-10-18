#define _POSIX_C_SOURCE 200809L
#include "json.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

struct cursor {
    const char *p, *end;
};

static void skip_space(struct cursor *c) {
    while (c->p < c->end && (*c->p == ' ' || *c->p == '\t' || *c->p == '\n' || *c->p == '\r')) ++c->p;
}

static int hex_value(char h) {
    if (h >= '0' && h <= '9') return h - '0';
    if (h >= 'a' && h <= 'f') return h - 'a' + 10;
    if (h >= 'A' && h <= 'F') return h - 'A' + 10;
    return -1;
}

// Appends a code point as UTF-8, or as one byte below 0x100 (see json.h).
static int put_code(char *out, size_t size, size_t *n, unsigned code) {
    char bytes[4];
    size_t count;
    if (code < 0x100) { bytes[0] = (char)code; count = 1; }
    else if (code < 0x800) { bytes[0] = (char)(0xC0 | code >> 6); bytes[1] = (char)(0x80 | (code & 0x3F)); count = 2; }
    else if (code < 0x10000) {
        bytes[0] = (char)(0xE0 | code >> 12); bytes[1] = (char)(0x80 | ((code >> 6) & 0x3F));
        bytes[2] = (char)(0x80 | (code & 0x3F)); count = 3;
    } else {
        bytes[0] = (char)(0xF0 | code >> 18); bytes[1] = (char)(0x80 | ((code >> 12) & 0x3F));
        bytes[2] = (char)(0x80 | ((code >> 6) & 0x3F)); bytes[3] = (char)(0x80 | (code & 0x3F)); count = 4;
    }
    if (*n + count >= size) return -1;
    memcpy(out + *n, bytes, count);
    *n += count;
    return 0;
}

static int parse_string(struct cursor *c, char *out, size_t size, size_t *length) {
    size_t n = 0;
    if (c->p >= c->end || *c->p != '"') return -1;
    ++c->p;
    while (c->p < c->end && *c->p != '"') {
        char ch = *c->p++;
        if (ch != '\\') {
            if (n + 1 >= size) return -1;
            out[n++] = ch;
            continue;
        }
        if (c->p >= c->end) return -1;
        char esc = *c->p++;
        unsigned code;
        switch (esc) {
        case '"': code = '"'; break;
        case '\\': code = '\\'; break;
        case '/': code = '/'; break;
        case 'b': code = '\b'; break;
        case 'f': code = '\f'; break;
        case 'n': code = '\n'; break;
        case 'r': code = '\r'; break;
        case 't': code = '\t'; break;
        case 'u': {
            if (c->end - c->p < 4) return -1;
            code = 0;
            for (int i = 0; i < 4; ++i) {
                int h = hex_value(c->p[i]);
                if (h < 0) return -1;
                code = code << 4 | (unsigned)h;
            }
            c->p += 4;
            // A surrogate pair becomes one code point.
            if (code >= 0xD800 && code < 0xDC00 && c->end - c->p >= 6 && c->p[0] == '\\' && c->p[1] == 'u') {
                unsigned low = 0;
                int ok = 1;
                for (int i = 0; i < 4; ++i) {
                    int h = hex_value(c->p[2 + i]);
                    if (h < 0) { ok = 0; break; }
                    low = low << 4 | (unsigned)h;
                }
                if (ok && low >= 0xDC00 && low < 0xE000) {
                    code = 0x10000 + ((code - 0xD800) << 10) + (low - 0xDC00);
                    c->p += 6;
                }
            }
            break;
        }
        default: return -1;
        }
        if (put_code(out, size, &n, code) < 0) return -1;
    }
    if (c->p >= c->end) return -1;
    ++c->p;
    out[n] = 0;
    *length = n;
    return 0;
}

// Skips a nested value (object, array, literal), for RAW members.
static int skip_value(struct cursor *c) {
    int depth = 0;
    int in_string = 0;
    const char *start = c->p;
    while (c->p < c->end) {
        char ch = *c->p;
        if (in_string) {
            if (ch == '\\') ++c->p;
            else if (ch == '"') in_string = 0;
        } else if (ch == '"') {
            in_string = 1;
        } else if (ch == '{' || ch == '[') {
            ++depth;
        } else if (ch == '}' || ch == ']') {
            if (depth == 0) break;
            if (--depth == 0) { ++c->p; return 0; }
        } else if (depth == 0 && (ch == ',' || isspace((unsigned char)ch))) {
            break;
        }
        ++c->p;
    }
    return c->p > start && depth == 0 ? 0 : -1;
}

// Skips one value of any kind: a string, or anything skip_value takes.
static int skip_any(struct cursor *c) {
    char scratch[MAINUI_JSON_TEXT];
    size_t n;
    if (c->p < c->end && *c->p == '"') {
        // A long string is only skipped, not decoded.
        const char *start = c->p;
        if (parse_string(c, scratch, sizeof(scratch), &n) == 0) return 0;
        c->p = start + 1;
        while (c->p < c->end && *c->p != '"') c->p += *c->p == '\\' ? 2 : 1;
        if (c->p >= c->end) return -1;
        ++c->p;
        return 0;
    }
    return skip_value(c);
}

// Parses one object, handing each member to emit, which returns -1 to stop. When fitting, a
// value too long to keep is left out instead of failing the parse.
static int parse_object(const char *text, size_t length, int fitting,
                        int (*emit)(void *, const struct mainui_json_member *), void *context) {
    static struct mainui_json_member member;  // large; parsing runs on one thread
    struct mainui_json_member *m = &member;
    struct cursor c = { text, text + length };
    skip_space(&c);
    if (c.p >= c.end || *c.p != '{') return -1;
    ++c.p;
    skip_space(&c);
    if (c.p < c.end && *c.p == '}') { ++c.p; goto done; }
    for (;;) {
        size_t key_length;
        skip_space(&c);
        if (parse_string(&c, m->key, sizeof(m->key), &key_length) < 0) return -1;
        skip_space(&c);
        if (c.p >= c.end || *c.p != ':') return -1;
        ++c.p;
        skip_space(&c);
        if (c.p >= c.end) return -1;
        if (*c.p == '"') {
            const char *start = c.p;
            m->type = MAINUI_JSON_STRING;
            if (parse_string(&c, m->text, sizeof(m->text), &m->text_length) < 0) {
                c.p = start;
                if (!fitting || skip_any(&c) < 0) return -1;
                goto next;
            }
        } else if (*c.p == '-' || isdigit((unsigned char)*c.p)) {
            char number[64];
            size_t n = 0;
            while (c.p < c.end && n + 1 < sizeof(number) && strchr("+-0123456789.eE", *c.p)) number[n++] = *c.p++;
            number[n] = 0;
            char *end = NULL;
            m->type = MAINUI_JSON_NUMBER;
            m->number = strtod(number, &end);
            if (!end || *end) return -1;
            memcpy(m->text, number, n + 1);
            m->text_length = n;
        } else {
            const char *start = c.p;
            if (skip_value(&c) < 0) return -1;
            size_t n = (size_t)(c.p - start);
            if (n >= sizeof(m->text)) {
                if (!fitting) return -1;
                goto next;
            }
            m->type = MAINUI_JSON_RAW;
            memcpy(m->text, start, n);
            m->text[n] = 0;
            m->text_length = n;
        }
        if (emit(context, m) < 0) return -1;
    next:
        skip_space(&c);
        if (c.p < c.end && *c.p == ',') { ++c.p; continue; }
        if (c.p < c.end && *c.p == '}') { ++c.p; break; }
        return -1;
    }
done:
    skip_space(&c);
    return c.p == c.end ? 0 : -1;
}

static int keep_member(void *context, const struct mainui_json_member *m) {
    struct mainui_json *json = context;
    if (json->count == MAINUI_JSON_MEMBERS) return -1;
    json->members[json->count++] = *m;
    return 0;
}

int mainui_json_parse(struct mainui_json *json, const char *text, size_t length) {
    json->count = 0;
    return parse_object(text, length, 0, keep_member, json);
}

int mainui_json_parse_fitting(struct mainui_json *json, const char *text, size_t length) {
    json->count = 0;
    return parse_object(text, length, 1, keep_member, json);
}

int mainui_json_each(const char *text, size_t length, int (*callback)(void *, const struct mainui_json_member *),
                     void *context) {
    return parse_object(text, length, 0, callback, context);
}

int mainui_json_find(const char *text, size_t length, const char *key, const char **value, size_t *value_length) {
    char name[MAINUI_JSON_KEY];
    size_t name_length;
    struct cursor c = { text, text + length };
    skip_space(&c);
    if (c.p >= c.end || *c.p != '{') return -1;
    ++c.p;
    for (;;) {
        skip_space(&c);
        if (c.p < c.end && *c.p == '}') return -1;
        if (parse_string(&c, name, sizeof(name), &name_length) < 0) return -1;
        skip_space(&c);
        if (c.p >= c.end || *c.p != ':') return -1;
        ++c.p;
        skip_space(&c);
        const char *start = c.p;
        if (skip_any(&c) < 0) return -1;
        if (strcasecmp(name, key) == 0) {
            *value = start;
            *value_length = (size_t)(c.p - start);
            return 0;
        }
        skip_space(&c);
        if (c.p < c.end && *c.p == ',') { ++c.p; continue; }
        return -1;
    }
}

int mainui_json_array_each(const char *text, size_t length, int (*callback)(void *, const char *, size_t), void *context) {
    struct cursor c = { text, text + length };
    skip_space(&c);
    if (c.p >= c.end || *c.p != '[') return -1;
    ++c.p;
    skip_space(&c);
    if (c.p < c.end && *c.p == ']') return 0;
    for (;;) {
        skip_space(&c);
        const char *start = c.p;
        if (skip_any(&c) < 0) return -1;
        if (callback(context, start, (size_t)(c.p - start)) < 0) return -1;
        skip_space(&c);
        if (c.p < c.end && *c.p == ',') { ++c.p; continue; }
        if (c.p < c.end && *c.p == ']') return 0;
        return -1;
    }
}

const struct mainui_json_member *mainui_json_get(const struct mainui_json *json, const char *key) {
    for (size_t i = 0; i < json->count; ++i)
        if (strcasecmp(json->members[i].key, key) == 0) return &json->members[i];
    return NULL;
}

const struct mainui_json_member *mainui_json_last(const struct mainui_json *json, const char *key) {
    for (size_t i = json->count; i > 0; --i)
        if (strcasecmp(json->members[i - 1].key, key) == 0) return &json->members[i - 1];
    return NULL;
}

int mainui_json_escape(char *out, size_t size, const char *bytes, size_t length) {
    static const char hex[] = "0123456789abcdef";
    size_t n = 0;
#define PUT(ch) do { if (n + 1 >= size) return -1; out[n++] = (ch); } while (0)
    PUT('"');
    for (size_t i = 0; i < length; ++i) {
        unsigned char ch = (unsigned char)bytes[i];
        switch (ch) {
        case '"': PUT('\\'); PUT('"'); break;
        case '\\': PUT('\\'); PUT('\\'); break;
        case '\b': PUT('\\'); PUT('b'); break;
        case '\f': PUT('\\'); PUT('f'); break;
        case '\n': PUT('\\'); PUT('n'); break;
        case '\r': PUT('\\'); PUT('r'); break;
        case '\t': PUT('\\'); PUT('t'); break;
        default:
            if (ch < 0x20) {
                PUT('\\'); PUT('u'); PUT('0'); PUT('0'); PUT(hex[ch >> 4]); PUT(hex[ch & 15]);
            } else {
                PUT((char)ch);
            }
        }
    }
    PUT('"');
#undef PUT
    out[n] = 0;
    return (int)n;
}
