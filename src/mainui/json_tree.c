#define _POSIX_C_SOURCE 200809L
#include "json_tree.h"

#include <errno.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Memory comes in chunks freed together. A request larger than a chunk gets a chunk of its own.
#define CHUNK_BYTES (64 * 1024)

struct mainui_json_tree_chunk {
    struct mainui_json_tree_chunk *next;
    size_t used, size;
    max_align_t data[];
};

struct parser {
    const char *start, *p, *end;
    struct mainui_json_tree *tree;
    char *error;
    size_t size;
    int failed;
};

static int fail(struct parser *ps, const char *format, ...) {
    if (ps->failed) return -1;
    ps->failed = 1;
    if (ps->size) {
        int n = snprintf(ps->error, ps->size, "offset %ld: ", (long)(ps->p - ps->start));
        if (n >= 0 && (size_t)n < ps->size) {
            va_list args;
            va_start(args, format);
            vsnprintf(ps->error + n, ps->size - (size_t)n, format, args);
            va_end(args);
        }
    }
    return -1;
}

static void *allocate(struct parser *ps, size_t bytes) {
    bytes = (bytes + sizeof(max_align_t) - 1) / sizeof(max_align_t) * sizeof(max_align_t);
    struct mainui_json_tree_chunk *chunk = ps->tree->chunks;
    if (!chunk || chunk->size - chunk->used < bytes) {
        size_t size = bytes > CHUNK_BYTES ? bytes : CHUNK_BYTES;
        chunk = malloc(sizeof(*chunk) + size);
        if (!chunk) { fail(ps, "out of memory"); return NULL; }
        chunk->used = 0;
        chunk->size = size;
        chunk->next = ps->tree->chunks;
        ps->tree->chunks = chunk;
    }
    void *at = (char *)chunk->data + chunk->used;
    chunk->used += bytes;
    return at;
}

static void skip_space(struct parser *ps) {
    while (ps->p < ps->end && (*ps->p == ' ' || *ps->p == '\t' || *ps->p == '\n' || *ps->p == '\r')) ++ps->p;
}

static int hex4(struct parser *ps, unsigned *code) {
    if (ps->end - ps->p < 4) return fail(ps, "truncated \\u escape");
    unsigned value = 0;
    for (int i = 0; i < 4; ++i) {
        char h = *ps->p++;
        int digit = h >= '0' && h <= '9' ? h - '0' : h >= 'a' && h <= 'f' ? h - 'a' + 10 : h >= 'A' && h <= 'F' ? h - 'A' + 10 : -1;
        if (digit < 0) { --ps->p; return fail(ps, "invalid \\u escape"); }
        value = value << 4 | (unsigned)digit;
    }
    *code = value;
    return 0;
}

static size_t put_utf8(char *out, unsigned code) {
    if (code < 0x80) { out[0] = (char)code; return 1; }
    if (code < 0x800) { out[0] = (char)(0xC0 | code >> 6); out[1] = (char)(0x80 | (code & 0x3F)); return 2; }
    if (code < 0x10000) {
        out[0] = (char)(0xE0 | code >> 12); out[1] = (char)(0x80 | ((code >> 6) & 0x3F));
        out[2] = (char)(0x80 | (code & 0x3F)); return 3;
    }
    out[0] = (char)(0xF0 | code >> 18); out[1] = (char)(0x80 | ((code >> 12) & 0x3F));
    out[2] = (char)(0x80 | ((code >> 6) & 0x3F)); out[3] = (char)(0x80 | (code & 0x3F)); return 4;
}

// A string at ps->p (its opening quote). The decoded text is never longer than the source, so
// one allocation of the source length is enough.
static const char *parse_string(struct parser *ps, size_t *length) {
    const char *open = ps->p++;
    const char *scan = ps->p;
    while (scan < ps->end && *scan != '"') scan += *scan == '\\' && scan + 1 < ps->end ? 2 : 1;
    if (scan >= ps->end) { ps->p = open; fail(ps, "unterminated string"); return NULL; }
    char *out = allocate(ps, (size_t)(scan - ps->p) + 1);
    if (!out) return NULL;
    size_t n = 0;
    while (*ps->p != '"') {
        unsigned char ch = (unsigned char)*ps->p;
        if (ch < 0x20) { fail(ps, "control character in string"); return NULL; }
        ++ps->p;
        if (ch != '\\') { out[n++] = (char)ch; continue; }
        char escape = *ps->p++;
        unsigned code;
        switch (escape) {
        case '"': out[n++] = '"'; break;
        case '\\': out[n++] = '\\'; break;
        case '/': out[n++] = '/'; break;
        case 'b': out[n++] = '\b'; break;
        case 'f': out[n++] = '\f'; break;
        case 'n': out[n++] = '\n'; break;
        case 'r': out[n++] = '\r'; break;
        case 't': out[n++] = '\t'; break;
        case 'u':
            if (hex4(ps, &code) < 0) return NULL;
            if (code == 0) { fail(ps, "\\u0000 in string"); return NULL; }
            if (code >= 0xDC00 && code <= 0xDFFF) { fail(ps, "lone low surrogate"); return NULL; }
            if (code >= 0xD800 && code <= 0xDBFF) {
                unsigned low;
                if (ps->end - ps->p < 2 || ps->p[0] != '\\' || ps->p[1] != 'u') { fail(ps, "lone high surrogate"); return NULL; }
                ps->p += 2;
                if (hex4(ps, &low) < 0) return NULL;
                if (low < 0xDC00 || low > 0xDFFF) { fail(ps, "lone high surrogate"); return NULL; }
                code = 0x10000 + ((code - 0xD800) << 10) + (low - 0xDC00);
            }
            n += put_utf8(out + n, code);
            break;
        default:
            --ps->p;
            fail(ps, "invalid escape");
            return NULL;
        }
    }
    ++ps->p;
    out[n] = 0;
    *length = n;
    return out;
}

static int parse_number(struct parser *ps, double *number) {
    const char *s = ps->p;
    const char *q = s;
    if (q < ps->end && *q == '-') ++q;
    if (q >= ps->end || *q < '0' || *q > '9') return fail(ps, "invalid number");
    if (*q == '0') ++q;
    else while (q < ps->end && *q >= '0' && *q <= '9') ++q;
    if (q < ps->end && *q == '.') {
        ++q;
        if (q >= ps->end || *q < '0' || *q > '9') { ps->p = q; return fail(ps, "invalid number"); }
        while (q < ps->end && *q >= '0' && *q <= '9') ++q;
    }
    if (q < ps->end && (*q == 'e' || *q == 'E')) {
        ++q;
        if (q < ps->end && (*q == '+' || *q == '-')) ++q;
        if (q >= ps->end || *q < '0' || *q > '9') { ps->p = q; return fail(ps, "invalid number"); }
        while (q < ps->end && *q >= '0' && *q <= '9') ++q;
    }
    char copy[64];
    size_t n = (size_t)(q - s);
    if (n >= sizeof(copy)) return fail(ps, "number too long");
    memcpy(copy, s, n);
    copy[n] = 0;
    *number = strtod(copy, NULL);
    ps->p = q;
    return 0;
}

static int literal(struct parser *ps, const char *word) {
    size_t n = strlen(word);
    if ((size_t)(ps->end - ps->p) < n || memcmp(ps->p, word, n) != 0) return fail(ps, "unexpected character");
    ps->p += n;
    return 0;
}

static struct mainui_json_value *parse_value(struct parser *ps, int depth);

static int parse_members(struct parser *ps, struct mainui_json_value *v, int depth, int object) {
    char close = object ? '}' : ']';
    ++ps->p;
    skip_space(ps);
    struct mainui_json_value **tail = &v->first;
    if (ps->p < ps->end && *ps->p == close) { ++ps->p; return 0; }
    for (;;) {
        const char *key = NULL;
        skip_space(ps);
        if (object) {
            size_t key_length;
            if (ps->p >= ps->end || *ps->p != '"') return fail(ps, ps->p >= ps->end ? "truncated object" : "expected a member name");
            if (!(key = parse_string(ps, &key_length))) return -1;
            skip_space(ps);
            if (ps->p >= ps->end || *ps->p != ':') return fail(ps, "expected ':'");
            ++ps->p;
        }
        struct mainui_json_value *child = parse_value(ps, depth + 1);
        if (!child) return -1;
        child->key = key;
        *tail = child;
        tail = &child->next;
        ++v->length;
        skip_space(ps);
        if (ps->p >= ps->end) return fail(ps, object ? "truncated object" : "truncated array");
        if (*ps->p == ',') { ++ps->p; continue; }
        if (*ps->p == close) { ++ps->p; return 0; }
        return fail(ps, object ? "expected ',' or '}'" : "expected ',' or ']'");
    }
}

static struct mainui_json_value *parse_value(struct parser *ps, int depth) {
    skip_space(ps);
    if (ps->p >= ps->end) { fail(ps, "unexpected end of input"); return NULL; }
    struct mainui_json_value *v = allocate(ps, sizeof(*v));
    if (!v) return NULL;
    memset(v, 0, sizeof(*v));
    switch (*ps->p) {
    case '{':
    case '[':
        if (depth >= MAINUI_JSON_TREE_MAX_DEPTH) { fail(ps, "nested deeper than %d levels", MAINUI_JSON_TREE_MAX_DEPTH); return NULL; }
        v->type = *ps->p == '{' ? MAINUI_JSON_TREE_OBJECT : MAINUI_JSON_TREE_ARRAY;
        if (parse_members(ps, v, depth, v->type == MAINUI_JSON_TREE_OBJECT) < 0) return NULL;
        return v;
    case '"':
        v->type = MAINUI_JSON_TREE_STRING;
        if (!(v->string = parse_string(ps, &v->length))) return NULL;
        return v;
    case 't': v->type = MAINUI_JSON_TREE_TRUE; return literal(ps, "true") < 0 ? NULL : v;
    case 'f': v->type = MAINUI_JSON_TREE_FALSE; return literal(ps, "false") < 0 ? NULL : v;
    case 'n': v->type = MAINUI_JSON_TREE_NULL; return literal(ps, "null") < 0 ? NULL : v;
    default:
        v->type = MAINUI_JSON_TREE_NUMBER;
        return parse_number(ps, &v->number) < 0 ? NULL : v;
    }
}

int mainui_json_tree_parse(struct mainui_json_tree *tree, const char *text, size_t length, char *error, size_t size) {
    struct parser ps = { text, text, text + length, tree, error, size, 0 };
    tree->root = NULL;
    tree->chunks = NULL;
    if (size) error[0] = 0;
    if (length > MAINUI_JSON_TREE_MAX_BYTES) {
        fail(&ps, "larger than %d KiB", MAINUI_JSON_TREE_MAX_BYTES / 1024);
        return -1;
    }
    // A UTF-8 byte order mark, as Windows editors write, is not part of the document.
    if (length >= 3 && memcmp(text, "\xEF\xBB\xBF", 3) == 0) ps.p += 3;
    struct mainui_json_value *root = parse_value(&ps, 0);
    if (root) {
        skip_space(&ps);
        if (ps.p < ps.end) { fail(&ps, "extra data after the document"); root = NULL; }
    }
    if (!root) {
        mainui_json_tree_free(tree);
        return -1;
    }
    tree->root = root;
    return 0;
}

int mainui_json_tree_load(struct mainui_json_tree *tree, const char *path, char *error, size_t size) {
    tree->root = NULL;
    tree->chunks = NULL;
    FILE *f = fopen(path, "rb");
    if (!f) {
        if (size) snprintf(error, size, "cannot open: %s", strerror(errno));
        return -1;
    }
    // One byte past the limit tells an oversized file from one exactly at it.
    char *text = malloc(MAINUI_JSON_TREE_MAX_BYTES + 1);
    if (!text) {
        fclose(f);
        if (size) snprintf(error, size, "out of memory");
        return -1;
    }
    size_t n = fread(text, 1, MAINUI_JSON_TREE_MAX_BYTES + 1, f);
    int failed = ferror(f);
    fclose(f);
    int result;
    if (failed) {
        if (size) snprintf(error, size, "read error");
        result = -1;
    } else {
        result = mainui_json_tree_parse(tree, text, n, error, size);
    }
    free(text);
    return result;
}

void mainui_json_tree_free(struct mainui_json_tree *tree) {
    while (tree->chunks) {
        struct mainui_json_tree_chunk *next = tree->chunks->next;
        free(tree->chunks);
        tree->chunks = next;
    }
    tree->root = NULL;
}

const struct mainui_json_value *mainui_json_tree_get(const struct mainui_json_value *object, const char *key) {
    if (!object || object->type != MAINUI_JSON_TREE_OBJECT) return NULL;
    const struct mainui_json_value *found = NULL;
    for (const struct mainui_json_value *m = object->first; m; m = m->next)
        if (strcmp(m->key, key) == 0) found = m;
    return found;
}

const char *mainui_json_tree_string(const struct mainui_json_value *object, const char *key) {
    const struct mainui_json_value *v = mainui_json_tree_get(object, key);
    return v && v->type == MAINUI_JSON_TREE_STRING ? v->string : NULL;
}

int mainui_json_tree_number(const struct mainui_json_value *object, const char *key, double *value) {
    const struct mainui_json_value *v = mainui_json_tree_get(object, key);
    if (!v) return -1;
    switch (v->type) {
    case MAINUI_JSON_TREE_NUMBER: *value = v->number; return 0;
    case MAINUI_JSON_TREE_TRUE: *value = 1; return 0;
    case MAINUI_JSON_TREE_FALSE: *value = 0; return 0;
    case MAINUI_JSON_TREE_STRING: {
        char *rest;
        errno = 0;
        long n = strtol(v->string, &rest, 10);
        if (rest == v->string || *rest || errno) return -1;
        *value = (double)n;
        return 0;
    }
    default: return -1;
    }
}
