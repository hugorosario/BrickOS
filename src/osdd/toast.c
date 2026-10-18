#define _POSIX_C_SOURCE 200809L
#include "toast.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

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
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == 0) {
            ++p->position;
        } else if (c == '\\' && p->position + 1 < p->length &&
                   (p->data[p->position + 1] == 'n' ||
                    p->data[p->position + 1] == 'r' ||
                    p->data[p->position + 1] == 't')) {
            p->position += 2;
        } else {
            break;
        }
    }
}

static int put_byte(struct parser *p, char *out, size_t capacity,
                    size_t *written, unsigned char value) {
    if (*written + 1 >= capacity) return fail(p, "string field exceeds capacity");
    out[(*written)++] = (char)value;
    return 0;
}

static int hex(unsigned char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static int unicode_unit(struct parser *p, uint32_t *value) {
    if (p->position + 4 > p->length) return fail(p, "incomplete unicode escape");
    uint32_t unit = 0;
    for (int i = 0; i < 4; ++i) {
        int digit = hex(p->data[p->position++]);
        if (digit < 0) return fail(p, "invalid unicode escape");
        unit = (unit << 4) | (uint32_t)digit;
    }
    *value = unit;
    return 0;
}

static int utf8(struct parser *p, char *out, size_t capacity,
                size_t *written, uint32_t value) {
    if (value < 0x80) return put_byte(p, out, capacity, written, (unsigned char)value);
    if (value < 0x800) {
        if (put_byte(p, out, capacity, written, 0xc0 | (value >> 6)) < 0) return -1;
        return put_byte(p, out, capacity, written, 0x80 | (value & 0x3f));
    }
    if (value < 0x10000) {
        if (put_byte(p, out, capacity, written, 0xe0 | (value >> 12)) < 0) return -1;
        if (put_byte(p, out, capacity, written, 0x80 | ((value >> 6) & 0x3f)) < 0) return -1;
        return put_byte(p, out, capacity, written, 0x80 | (value & 0x3f));
    }
    if (put_byte(p, out, capacity, written, 0xf0 | (value >> 18)) < 0) return -1;
    if (put_byte(p, out, capacity, written, 0x80 | ((value >> 12) & 0x3f)) < 0) return -1;
    if (put_byte(p, out, capacity, written, 0x80 | ((value >> 6) & 0x3f)) < 0) return -1;
    return put_byte(p, out, capacity, written, 0x80 | (value & 0x3f));
}

static int string(struct parser *p, char *out, size_t capacity) {
    if (p->position >= p->length || p->data[p->position++] != '"')
        return fail(p, "expected string at offset %zu", p->position);
    size_t written = 0;
    while (p->position < p->length) {
        unsigned char c = p->data[p->position++];
        if (c == '"') {
            out[written] = 0;
            return 0;
        }
        if (c < 0x20) return fail(p, "unescaped control character in string");
        if (c == '\\') {
            if (p->position >= p->length) return fail(p, "incomplete string escape");
            c = p->data[p->position++];
            switch (c) {
                case '"': case '\\': case '/': break;
                case 'b': c = '\b'; break;
                case 'f': c = '\f'; break;
                case 'n': c = '\n'; break;
                case 'r': c = '\r'; break;
                case 't': c = '\t'; break;
                case 'u': {
                    uint32_t value;
                    if (unicode_unit(p, &value) < 0) return -1;
                    if (value >= 0xd800 && value <= 0xdbff) {
                        if (p->position + 2 > p->length || p->data[p->position] != '\\' ||
                            p->data[p->position + 1] != 'u') return fail(p, "missing low surrogate");
                        p->position += 2;
                        uint32_t low;
                        if (unicode_unit(p, &low) < 0) return -1;
                        if (low < 0xdc00 || low > 0xdfff) return fail(p, "invalid low surrogate");
                        value = 0x10000 + ((value - 0xd800) << 10) + (low - 0xdc00);
                    } else if (value >= 0xdc00 && value <= 0xdfff) {
                        return fail(p, "unexpected low surrogate");
                    }
                    if (utf8(p, out, capacity, &written, value) < 0) return -1;
                    continue;
                }
                default: return fail(p, "invalid string escape");
            }
        }
        if (put_byte(p, out, capacity, &written, c) < 0) return -1;
    }
    return fail(p, "unterminated string");
}

static int integer(struct parser *p, int *out) {
    int sign = 1;
    if (p->position < p->length && p->data[p->position] == '-') {
        sign = -1;
        ++p->position;
    }
    if (p->position >= p->length || p->data[p->position] < '0' ||
        p->data[p->position] > '9') return fail(p, "expected integer");
    int64_t value = 0;
    while (p->position < p->length && p->data[p->position] >= '0' &&
           p->data[p->position] <= '9') {
        int digit = p->data[p->position++] - '0';
        if (value > ((int64_t)INT_MAX + 1 - digit) / 10)
            return fail(p, "integer out of range");
        value = value * 10 + digit;
    }
    value *= sign;
    if (value < INT_MIN || value > INT_MAX) return fail(p, "integer out of range");
    *out = (int)value;
    return 0;
}

static int parse_value(struct parser *p, const char *key, struct osdd_toast *toast) {
    char *target = NULL;
    size_t capacity = 0;
#define FIELD(name) if (strcmp(key, #name) == 0) { target = toast->name; capacity = sizeof(toast->name); }
    FIELD(type) else FIELD(id) else FIELD(message) else FIELD(font)
    else FIELD(bg) else FIELD(icon) else FIELD(fontcolor)
#undef FIELD
    if (target != NULL) return string(p, target, capacity);

    int *number = NULL;
#define NUMBER(name) if (strcmp(key, #name) == 0) number = &toast->name
    NUMBER(duration); else NUMBER(size); else NUMBER(x); else NUMBER(y);
    else NUMBER(w); else NUMBER(h); else NUMBER(fontsize);
#undef NUMBER
    if (number != NULL) return integer(p, number);

    if (p->position < p->length && p->data[p->position] == '"') {
        char ignored[OSDD_TOAST_MAX_BYTES];
        return string(p, ignored, sizeof(ignored));
    }
    if (p->position < p->length && (p->data[p->position] == '-' ||
        (p->data[p->position] >= '0' && p->data[p->position] <= '9'))) {
        int ignored;
        return integer(p, &ignored);
    }
    for (size_t i = 0; i < 3; ++i) {
        const char *literal = i == 0 ? "true" : i == 1 ? "false" : "null";
        size_t count = strlen(literal);
        if (p->length - p->position >= count &&
            memcmp(p->data + p->position, literal, count) == 0) {
            p->position += count;
            return 0;
        }
    }
    return fail(p, "unsupported value for %s", key);
}

int osdd_toast_parse(const unsigned char *data, size_t length,
                     struct osdd_toast *result, char *error, size_t error_size) {
    if (error != NULL && error_size > 0) error[0] = 0;
    if (data == NULL || result == NULL || length == 0 || length > OSDD_TOAST_MAX_BYTES) {
        if (error != NULL && error_size > 0) snprintf(error, error_size, "empty or oversized payload");
        return -1;
    }
    struct parser p = {data, length, 0, error, error_size};
    struct osdd_toast next = {0};
    space(&p);
    if (p.position >= p.length || p.data[p.position++] != '{') return fail(&p, "expected object");
    space(&p);
    if (p.position < p.length && p.data[p.position] == '}') return fail(&p, "empty toast object");
    for (;;) {
        char key[64];
        if (string(&p, key, sizeof(key)) < 0) return -1;
        space(&p);
        if (p.position >= p.length || p.data[p.position++] != ':') return fail(&p, "expected colon");
        space(&p);
        if (parse_value(&p, key, &next) < 0) return -1;
        space(&p);
        if (p.position >= p.length) return fail(&p, "incomplete object");
        unsigned char delimiter = p.data[p.position++];
        if (delimiter == '}') break;
        if (delimiter != ',') return fail(&p, "expected comma or object end");
        space(&p);
    }
    space(&p);
    if (p.position != p.length) return fail(&p, "trailing data");
    if (next.type[0] == 0 && next.id[0] == 0 && next.message[0] == 0)
        return fail(&p, "toast has no type, id, or message");
    *result = next;
    return 0;
}

static int same_metadata(const struct stat *a, const struct stat *b) {
    return a->st_dev == b->st_dev && a->st_ino == b->st_ino &&
           a->st_size == b->st_size && a->st_mtim.tv_sec == b->st_mtim.tv_sec &&
           a->st_mtim.tv_nsec == b->st_mtim.tv_nsec;
}

enum osdd_poll_result osdd_toast_reader_poll(struct osdd_toast_reader *reader,
                                              struct osdd_toast *result,
                                              char *error, size_t error_size) {
    if (error != NULL && error_size > 0) error[0] = 0;
    if (reader == NULL || reader->path == NULL || result == NULL) {
        if (error != NULL && error_size > 0) snprintf(error, error_size, "invalid reader");
        return OSDD_POLL_ERROR;
    }
    int fd = open(reader->path, O_RDONLY | O_CLOEXEC | O_NONBLOCK | O_NOFOLLOW);
    if (fd < 0) {
        if (errno == ENOENT) return OSDD_POLL_UNCHANGED;
        if (error != NULL && error_size > 0) snprintf(error, error_size, "open: %s", strerror(errno));
        return OSDD_POLL_ERROR;
    }
    struct stat before, after, current;
    if (fstat(fd, &before) < 0 || !S_ISREG(before.st_mode)) {
        if (error != NULL && error_size > 0) snprintf(error, error_size, "toast path is not a regular file");
        close(fd);
        return OSDD_POLL_ERROR;
    }
    if (reader->has_kept && same_metadata(&reader->kept, &before)) {
        close(fd);
        return OSDD_POLL_UNCHANGED;
    }
    // One byte past the limit identifies an oversized record without reading all of it.
    unsigned char bytes[OSDD_TOAST_MAX_BYTES + 1];
    size_t length = 0;
    ssize_t count = 0;
    while (length < sizeof(bytes)) {
        count = read(fd, bytes + length, sizeof(bytes) - length);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) break;
        length += (size_t)count;
    }
    int saved_errno = errno;
    int stable = fstat(fd, &after) == 0 && same_metadata(&before, &after);
    close(fd);
    if (count < 0) {
        if (error != NULL && error_size > 0) snprintf(error, error_size, "read: %s", strerror(saved_errno));
        return OSDD_POLL_ERROR;
    }
    if (!stable) return OSDD_POLL_UNCHANGED; // Written during the read; take it on the next check.
    if (reader->after_read != NULL) reader->after_read(reader->after_read_context);
    // The official daemon deletes every record it reads. Delete only the file that was
    // read, so a rewrite racing this check stays for the next one.
    char delete_error[96] = "";
    if (lstat(reader->path, &current) == 0 && same_metadata(&after, &current) &&
        unlink(reader->path) < 0 && errno != ENOENT) {
        snprintf(delete_error, sizeof(delete_error), "cannot delete consumed record: %s",
                 strerror(errno));
        reader->kept = after;
        reader->has_kept = 1;
    }
    struct osdd_toast candidate;
    if (length > OSDD_TOAST_MAX_BYTES) {
        if (error != NULL && error_size > 0)
            snprintf(error, error_size, "record exceeds %d bytes", OSDD_TOAST_MAX_BYTES);
        return OSDD_POLL_DISCARDED;
    }
    if (osdd_toast_parse(bytes, length, &candidate, error, error_size) < 0)
        return OSDD_POLL_DISCARDED;
    if (delete_error[0] != 0 && error != NULL && error_size > 0)
        snprintf(error, error_size, "%s", delete_error);
    *result = candidate;
    return OSDD_POLL_UPDATE;
}
