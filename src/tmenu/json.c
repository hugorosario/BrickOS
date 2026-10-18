#define _DEFAULT_SOURCE
#include "json.h"

#include <ctype.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

struct cursor {
    const char *p, *end;
};

static void skip_space(struct cursor *c) {
    while (c->p < c->end && (unsigned char)*c->p <= ' ') ++c->p;
}

static int hex4(struct cursor *c, unsigned *value) {
    if (c->end - c->p < 4) return -1;
    *value = 0;
    for (int i = 0; i < 4; ++i) {
        int d = c->p[i];
        *value <<= 4;
        if (d >= '0' && d <= '9') *value |= (unsigned)(d - '0');
        else if (d >= 'a' && d <= 'f') *value |= (unsigned)(d - 'a' + 10);
        else if (d >= 'A' && d <= 'F') *value |= (unsigned)(d - 'A' + 10);
        else return -1;
    }
    c->p += 4;
    return 0;
}

static void put_utf8(char *out, size_t size, size_t *used, unsigned code) {
    char bytes[4];
    size_t n;
    if (code < 0x80) { bytes[0] = (char)code; n = 1; }
    else if (code < 0x800) { bytes[0] = (char)(0xc0 | (code >> 6)); bytes[1] = (char)(0x80 | (code & 0x3f)); n = 2; }
    else if (code < 0x10000) {
        bytes[0] = (char)(0xe0 | (code >> 12));
        bytes[1] = (char)(0x80 | ((code >> 6) & 0x3f));
        bytes[2] = (char)(0x80 | (code & 0x3f));
        n = 3;
    } else {
        bytes[0] = (char)(0xf0 | (code >> 18));
        bytes[1] = (char)(0x80 | ((code >> 12) & 0x3f));
        bytes[2] = (char)(0x80 | ((code >> 6) & 0x3f));
        bytes[3] = (char)(0x80 | (code & 0x3f));
        n = 4;
    }
    if (*used + n < size) {
        memcpy(out + *used, bytes, n);
        *used += n;
    }
}

// Reads a string at c->p (on its opening quote) into out, truncated to size - 1 bytes.
static int read_string(struct cursor *c, char *out, size_t size) {
    size_t used = 0;
    if (c->p >= c->end || *c->p != '"') return -1;
    ++c->p;
    while (c->p < c->end && *c->p != '"') {
        unsigned code = (unsigned char)*c->p++;
        if (code == '\\') {
            if (c->p >= c->end) return -1;
            char e = *c->p++;
            switch (e) {
            case 'b': code = '\b'; break;
            case 'f': code = '\f'; break;
            case 'n': code = '\n'; break;
            case 'r': code = '\r'; break;
            case 't': code = '\t'; break;
            case 'u': {
                if (hex4(c, &code) < 0) return -1;
                if (code >= 0xd800 && code < 0xdc00 && c->end - c->p >= 6 && c->p[0] == '\\' && c->p[1] == 'u') {
                    unsigned low;
                    c->p += 2;
                    if (hex4(c, &low) < 0) return -1;
                    code = 0x10000 + ((code - 0xd800) << 10) + (low - 0xdc00);
                }
                put_utf8(out, size, &used, code);
                continue;
            }
            default: code = (unsigned char)e; break;
            }
        }
        if (used + 1 < size) out[used++] = (char)code;
    }
    if (c->p >= c->end) return -1;
    ++c->p;
    if (size) out[used] = 0;
    return 0;
}

static int skip_value(struct cursor *c);

static int skip_container(struct cursor *c, char open, char close) {
    ++c->p;
    skip_space(c);
    if (c->p < c->end && *c->p == close) { ++c->p; return 0; }
    for (;;) {
        skip_space(c);
        if (open == '{') {
            char key[2];
            if (read_string(c, key, sizeof(key)) < 0) return -1;
            skip_space(c);
            if (c->p >= c->end || *c->p != ':') return -1;
            ++c->p;
        }
        if (skip_value(c) < 0) return -1;
        skip_space(c);
        if (c->p < c->end && *c->p == ',') { ++c->p; continue; }
        if (c->p < c->end && *c->p == close) { ++c->p; return 0; }
        return -1;
    }
}

static int skip_value(struct cursor *c) {
    skip_space(c);
    if (c->p >= c->end) return -1;
    char first = *c->p;
    if (first == '"') {
        char scratch[2];
        return read_string(c, scratch, sizeof(scratch));
    }
    if (first == '{') return skip_container(c, '{', '}');
    if (first == '[') return skip_container(c, '[', ']');
    const char *start = c->p;
    while (c->p < c->end && (isalnum((unsigned char)*c->p) || strchr("+-.", *c->p))) ++c->p;
    return c->p == start ? -1 : 0;
}

static int read_number(struct cursor *c, int *number) {
    char text[64];
    size_t n = 0;
    while (c->p < c->end && n + 1 < sizeof(text) && strchr("+-.0123456789eE", *c->p)) text[n++] = *c->p++;
    if (n == 0) return -1;
    text[n] = 0;
    char *stop;
    double value = strtod(text, &stop);
    if (stop == text) return -1;
    if (value >= INT_MAX) *number = INT_MAX;
    else if (value <= (double)INT_MIN) *number = INT_MIN;
    else *number = (int)value;
    return 0;
}

int tmenu_json_parse(struct tmenu_json *json, const char *text, size_t length) {
    struct cursor c = {text, text + length};
    json->count = 0;
    skip_space(&c);
    if (c.p >= c.end || *c.p != '{') return -1;
    ++c.p;
    skip_space(&c);
    if (c.p < c.end && *c.p == '}') return 0;
    for (;;) {
        struct tmenu_json_member scratch, *member = json->count < TMENU_JSON_MEMBERS ? &json->members[json->count] : &scratch;
        memset(member, 0, sizeof(*member));
        skip_space(&c);
        if (read_string(&c, member->key, sizeof(member->key)) < 0) return -1;
        skip_space(&c);
        if (c.p >= c.end || *c.p != ':') return -1;
        ++c.p;
        skip_space(&c);
        if (c.p >= c.end) return -1;
        if (*c.p == '"') {
            member->type = TMENU_JSON_STRING;
            if (read_string(&c, member->text, sizeof(member->text)) < 0) return -1;
        } else if (*c.p == '-' || (*c.p >= '0' && *c.p <= '9')) {
            member->type = TMENU_JSON_NUMBER;
            if (read_number(&c, &member->number) < 0) return -1;
        } else {
            member->type = TMENU_JSON_OTHER;
            if (skip_value(&c) < 0) return -1;
        }
        if (member != &scratch) ++json->count;
        skip_space(&c);
        if (c.p < c.end && *c.p == ',') { ++c.p; continue; }
        if (c.p < c.end && *c.p == '}') return 0;
        return -1;
    }
}

const struct tmenu_json_member *tmenu_json_get(const struct tmenu_json *json, const char *key) {
    for (size_t i = 0; i < json->count; ++i)
        if (strcasecmp(json->members[i].key, key) == 0) return &json->members[i];
    return NULL;
}
