#define _POSIX_C_SOURCE 200809L
#include "lists.h"
#include "json.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void copy_text(char *out, size_t size, const struct mainui_json_member *m) {
    out[0] = 0;
    if (!m || m->type != MAINUI_JSON_STRING) return;
    size_t n = m->text_length < size - 1 ? m->text_length : size - 1;
    memcpy(out, m->text, n);
    out[n] = 0;
}

static void parse_entry(struct mainui_list_entry *e) {
    static struct mainui_json json;  // large; the lists are read on one thread
    char *line = e->line;
    memset(e, 0, sizeof(*e));
    e->line = line;
    if (mainui_json_parse(&json, line, strlen(line)) < 0) return;
    const struct mainui_json_member *label = mainui_json_get(&json, "label");
    if (!label || label->type != MAINUI_JSON_STRING) return;
    e->valid = 1;
    copy_text(e->label, sizeof(e->label), label);
    const struct mainui_json_member *rompath = mainui_json_get(&json, "rompath");
    if (rompath && rompath->type == MAINUI_JSON_STRING) {
        e->rompath_length = rompath->text_length < sizeof(e->rompath) ? rompath->text_length : sizeof(e->rompath) - 1;
        memcpy(e->rompath, rompath->text, e->rompath_length);
        e->rompath[e->rompath_length] = 0;
    }
    copy_text(e->imgpath, sizeof(e->imgpath), mainui_json_get(&json, "imgpath"));
    copy_text(e->launch, sizeof(e->launch), mainui_json_get(&json, "launch"));
    copy_text(e->sublabel, sizeof(e->sublabel), mainui_json_get(&json, "sublabel"));
    const struct mainui_json_member *type = mainui_json_get(&json, "type");
    if (type && type->type == MAINUI_JSON_NUMBER) e->type = (int)type->number;
    const struct mainui_json_member *order = mainui_json_last(&json, "order");
    if (order && order->type == MAINUI_JSON_NUMBER) {
        e->order = (int)order->number;
        e->has_order = 1;
    }
}

static int reserve(struct mainui_entries *list, size_t count) {
    if (count <= list->capacity) return 0;
    size_t capacity = list->capacity ? list->capacity * 2 : 32;
    while (capacity < count) capacity *= 2;
    struct mainui_list_entry *entries = realloc(list->entries, capacity * sizeof(*entries));
    if (!entries) return -1;
    list->entries = entries;
    list->capacity = capacity;
    return 0;
}

static char *copy_line(const char *text, size_t length) {
    char *line = malloc(length + 1);
    if (!line) return NULL;
    memcpy(line, text, length);
    line[length] = 0;
    return line;
}

int mainui_list_parse(struct mainui_entries *list, const char *text, size_t length) {
    memset(list, 0, sizeof(*list));
    size_t start = 0;
    while (start < length) {
        size_t end = start;
        while (end < length && text[end] != '\n') ++end;
        size_t line_length = end - start;
        if (line_length && text[start + line_length - 1] == '\r') --line_length;
        // Blank lines carry nothing and are not kept.
        if (line_length) {
            if (reserve(list, list->count + 1) < 0) return -1;
            struct mainui_list_entry *e = &list->entries[list->count];
            e->line = copy_line(text + start, line_length);
            if (!e->line) return -1;
            parse_entry(e);
            ++list->count;
        }
        start = end + 1;
    }
    return 0;
}

void mainui_list_free(struct mainui_entries *list) {
    for (size_t i = 0; i < list->count; ++i) free(list->entries[i].line);
    free(list->entries);
    memset(list, 0, sizeof(*list));
}

char *mainui_list_format(const struct mainui_entries *list, size_t *length) {
    size_t total = 1;
    for (size_t i = 0; i < list->count; ++i) total += strlen(list->entries[i].line) + 1;
    char *text = malloc(total);
    if (!text) return NULL;
    size_t n = 0;
    for (size_t i = 0; i < list->count; ++i) {
        size_t line = strlen(list->entries[i].line);
        memcpy(text + n, list->entries[i].line, line);
        n += line;
        text[n++] = '\n';
    }
    text[n] = 0;
    *length = n;
    return text;
}

int mainui_list_load(struct mainui_entries *list, const char *path) {
    FILE *file = fopen(path, "rb");
    memset(list, 0, sizeof(*list));
    if (!file) return 0;  // a missing list is an empty one
    char *text = NULL;
    size_t length = 0, capacity = 0;
    for (;;) {
        if (length + 4096 > capacity) {
            capacity = capacity ? capacity * 2 : 16384;
            char *grown = realloc(text, capacity);
            if (!grown) { free(text); fclose(file); return -1; }
            text = grown;
        }
        size_t n = fread(text + length, 1, capacity - length, file);
        length += n;
        if (n == 0) break;
    }
    fclose(file);
    int result = mainui_list_parse(list, text ? text : "", length);
    free(text);
    return result;
}

int mainui_list_save(const struct mainui_entries *list, const char *path) {
    size_t length;
    char *text = mainui_list_format(list, &length);
    if (!text) return -1;
    char temporary[1100];
    if (snprintf(temporary, sizeof(temporary), "%s.tmp", path) >= (int)sizeof(temporary)) { free(text); return -1; }
    FILE *file = fopen(temporary, "wb");
    int ok = file && fwrite(text, 1, length, file) == length;
    if (file && fclose(file) != 0) ok = 0;
    free(text);
    if (!ok || rename(temporary, path) != 0) { remove(temporary); return -1; }
    return 0;
}

// ---- Entries -------------------------------------------------------------------------------

static void set_field(char *out, size_t size, const char *value) {
    snprintf(out, size, "%s", value ? value : "");
}

void mainui_list_entry_set(struct mainui_list_entry *e, const char *label, const char *rompath,
                           const char *imgpath, const char *launch, const char *sublabel, int type) {
    memset(e, 0, sizeof(*e));
    e->valid = 1;
    set_field(e->label, sizeof(e->label), label);
    set_field(e->rompath, sizeof(e->rompath), rompath);
    e->rompath_length = strlen(e->rompath);
    set_field(e->imgpath, sizeof(e->imgpath), imgpath);
    set_field(e->launch, sizeof(e->launch), launch);
    set_field(e->sublabel, sizeof(e->sublabel), sublabel);
    e->type = type;
}

// Appends "key":value to a line under construction.
static int add_string(char *line, size_t size, size_t *n, const char *key, const char *value, size_t length) {
    char escaped[4200];
    if (mainui_json_escape(escaped, sizeof(escaped), value, length) < 0) return -1;
    int written = snprintf(line + *n, size - *n, "%s\"%s\":%s", *n > 1 ? "," : "", key, escaped);
    if (written < 0 || (size_t)written >= size - *n) return -1;
    *n += (size_t)written;
    return 0;
}

static int add_number(char *line, size_t size, size_t *n, const char *key, int value) {
    int written = snprintf(line + *n, size - *n, "%s\"%s\":%d", *n > 1 ? "," : "", key, value);
    if (written < 0 || (size_t)written >= size - *n) return -1;
    *n += (size_t)written;
    return 0;
}

// The recent line, in the stock key order: label, rompath, imgpath, launch, type.
static char *recent_line(const struct mainui_list_entry *e) {
    char line[8192] = "{";
    size_t n = 1;
    // An app's line has only label, launch, and type (a2), and so has a port's (p3).
    if (MAINUI_ENTRY_SCRIPT(e->type)) {
        if (add_string(line, sizeof(line), &n, "label", e->label, strlen(e->label)) < 0 ||
            add_string(line, sizeof(line), &n, "launch", e->launch, strlen(e->launch)) < 0 ||
            add_number(line, sizeof(line), &n, "type", e->type) < 0 || n + 2 > sizeof(line))
            return NULL;
        line[n++] = '}';
        return copy_line(line, n);
    }
    if (add_string(line, sizeof(line), &n, "label", e->label, strlen(e->label)) < 0 ||
        add_string(line, sizeof(line), &n, "rompath", e->rompath, e->rompath_length) < 0 ||
        add_string(line, sizeof(line), &n, "imgpath", e->imgpath, strlen(e->imgpath)) < 0 ||
        add_string(line, sizeof(line), &n, "launch", e->launch, strlen(e->launch)) < 0 ||
        add_number(line, sizeof(line), &n, "type", e->type) < 0 || n + 2 > sizeof(line))
        return NULL;
    line[n++] = '}';
    return copy_line(line, n);
}

// A new favorite line, in the stock key order: label, launch, type, rompath, imgpath,
// sublabel, order. An app's or a port's has label, launch, type, and sublabel only (p7).
static char *favorite_line(const struct mainui_list_entry *e, int order) {
    char line[8192] = "{";
    size_t n = 1;
    if (MAINUI_ENTRY_SCRIPT(e->type)) {
        if (add_string(line, sizeof(line), &n, "label", e->label, strlen(e->label)) < 0 ||
            add_string(line, sizeof(line), &n, "launch", e->launch, strlen(e->launch)) < 0 ||
            add_number(line, sizeof(line), &n, "type", e->type) < 0 ||
            add_string(line, sizeof(line), &n, "sublabel", e->sublabel, strlen(e->sublabel)) < 0 || n + 2 > sizeof(line))
            return NULL;
        line[n++] = '}';
        return copy_line(line, n);
    }
    if (add_string(line, sizeof(line), &n, "label", e->label, strlen(e->label)) < 0 ||
        add_string(line, sizeof(line), &n, "launch", e->launch, strlen(e->launch)) < 0 ||
        add_number(line, sizeof(line), &n, "type", e->type) < 0 ||
        add_string(line, sizeof(line), &n, "rompath", e->rompath, e->rompath_length) < 0 ||
        add_string(line, sizeof(line), &n, "imgpath", e->imgpath, strlen(e->imgpath)) < 0 ||
        add_string(line, sizeof(line), &n, "sublabel", e->sublabel, strlen(e->sublabel)) < 0 ||
        add_number(line, sizeof(line), &n, "order", order) < 0 || n + 2 > sizeof(line))
        return NULL;
    line[n++] = '}';
    return copy_line(line, n);
}

static int same_entry(const struct mainui_list_entry *a, const struct mainui_list_entry *b) {
    if (!a->valid || !b->valid) return 0;
    if (MAINUI_ENTRY_SCRIPT(b->type)) return a->type == b->type && strcmp(a->launch, b->launch) == 0;
    return !MAINUI_ENTRY_SCRIPT(a->type) && a->rompath_length == b->rompath_length &&
           memcmp(a->rompath, b->rompath, b->rompath_length) == 0;
}

static int insert(struct mainui_entries *list, size_t at, char *line) {
    if (!line || reserve(list, list->count + 1) < 0) { free(line); return -1; }
    memmove(&list->entries[at + 1], &list->entries[at], (list->count - at) * sizeof(list->entries[0]));
    list->entries[at].line = line;
    parse_entry(&list->entries[at]);
    ++list->count;
    return 0;
}

int mainui_list_remove(struct mainui_entries *list, size_t index) {
    if (index >= list->count) return -1;
    free(list->entries[index].line);
    memmove(&list->entries[index], &list->entries[index + 1], (list->count - index - 1) * sizeof(list->entries[0]));
    --list->count;
    return 0;
}

void mainui_list_clear(struct mainui_entries *list) {
    while (list->count) mainui_list_remove(list, list->count - 1);
}

// ---- Recent --------------------------------------------------------------------------------

size_t mainui_recent_visible(const struct mainui_entries *list, size_t *indexes, size_t max) {
    size_t n = 0;
    for (size_t i = 0; i < list->count && n < max; ++i) {
        const struct mainui_list_entry *e = &list->entries[i];
        if (!e->valid) continue;
        int seen = 0;
        for (size_t j = 0; j < n && !seen; ++j) seen = strcmp(list->entries[indexes[j]].label, e->label) == 0;
        if (!seen) indexes[n++] = i;
    }
    return n;
}

int mainui_recent_add(struct mainui_entries *list, const struct mainui_list_entry *entry) {
    if (!MAINUI_ENTRY_SCRIPT(entry->type)) {
        for (size_t i = list->count; i > 0; --i)
            if (same_entry(&list->entries[i - 1], entry)) mainui_list_remove(list, i - 1);
    }
    if (insert(list, 0, recent_line(entry)) < 0) return -1;
    while (list->count > MAINUI_RECENT_LINES) mainui_list_remove(list, list->count - 1);
    return 0;
}

// ---- Favorites -----------------------------------------------------------------------------

size_t mainui_favorites_visible(const struct mainui_entries *list, size_t *indexes, size_t max) {
    size_t n = 0;
    for (size_t i = 0; i < list->count && n < max; ++i) {
        if (!list->entries[i].valid) continue;
        // Insertion sort by order, highest first. A line without an order (an app's or a port's)
        // comes after every ordered line, and equal orders show the later line first (p7, p8:
        // PortMaster, added after 2048, above it).
        size_t at = n;
        const struct mainui_list_entry *e = &list->entries[i];
        int order = e->has_order ? e->order : -1;
        while (at > 0 && (list->entries[indexes[at - 1]].has_order ? list->entries[indexes[at - 1]].order : -1) <= order) {
            indexes[at] = indexes[at - 1];
            --at;
        }
        indexes[at] = i;
        ++n;
    }
    return n;
}

int mainui_favorites_find(const struct mainui_entries *list, const struct mainui_list_entry *entry) {
    for (size_t i = 0; i < list->count; ++i)
        if (same_entry(&list->entries[i], entry)) return (int)i;
    return -1;
}

int mainui_favorites_add(struct mainui_entries *list, const struct mainui_list_entry *entry) {
    if (mainui_favorites_find(list, entry) >= 0) return 1;
    // Open: whether the stock order is the entry count + 1 or the highest order + 2; both gave
    // 15 for 14 entries whose last orders ran from 13 down to 0.
    int valid = 0;
    for (size_t i = 0; i < list->count; ++i) valid += list->entries[i].valid;
    return insert(list, list->count, favorite_line(entry, valid + 1));
}

// ---- List positions ------------------------------------------------------------------------

static int position_entry(void *context, const char *text, size_t length) {
    static struct mainui_json json;
    struct mainui_positions *p = context;
    if (p->count >= MAINUI_POSITIONS_MAX || mainui_json_parse(&json, text, length) < 0) return 0;
    const struct mainui_json_member *rompath = mainui_json_get(&json, "rompath"), *pos = mainui_json_get(&json, "pos"),
                                    *start = mainui_json_get(&json, "start"), *end = mainui_json_get(&json, "end");
    if (!rompath || rompath->type != MAINUI_JSON_STRING || rompath->text_length >= sizeof(p->entries[0].rompath) ||
        strlen(rompath->text) != rompath->text_length || !pos || pos->type != MAINUI_JSON_NUMBER || !start ||
        start->type != MAINUI_JSON_NUMBER || !end || end->type != MAINUI_JSON_NUMBER)
        return 0;
    struct mainui_position *e = &p->entries[p->count++];
    copy_text(e->rompath, sizeof(e->rompath), rompath);
    e->pos = (int)pos->number;
    e->start = (int)start->number;
    e->end = (int)end->number;
    return 0;
}

int mainui_positions_parse(struct mainui_positions *positions, const char *text, size_t length) {
    const char *list;
    size_t list_length;
    positions->count = 0;
    if (mainui_json_find(text, length, "list", &list, &list_length) < 0) return -1;
    return mainui_json_array_each(list, list_length, position_entry, positions);
}

const struct mainui_position *mainui_positions_find(const struct mainui_positions *positions, const char *rompath) {
    for (int i = 0; i < positions->count; ++i)
        if (strcmp(positions->entries[i].rompath, rompath) == 0) return &positions->entries[i];
    return NULL;
}

int mainui_positions_set(struct mainui_positions *positions, const char *rompath, int pos, int start, int end) {
    if (strlen(rompath) >= sizeof(positions->entries[0].rompath)) return -1;
    int found = 0, changed = 0;
    for (int i = 0; i < positions->count; ++i) {
        struct mainui_position *e = &positions->entries[i];
        if (strcmp(e->rompath, rompath) != 0) continue;
        found = 1;
        if (e->pos != pos || e->start != start || e->end != end) changed = 1;
        e->pos = pos;
        e->start = start;
        e->end = end;
    }
    if (found) return changed;
    if (positions->count >= MAINUI_POSITIONS_MAX) return -1;
    struct mainui_position *e = &positions->entries[positions->count++];
    snprintf(e->rompath, sizeof(e->rompath), "%s", rompath);
    e->pos = pos;
    e->start = start;
    e->end = end;
    return 1;
}

int mainui_positions_format(const struct mainui_positions *positions, char *out, size_t size) {
    char rompath[1024 * 6 + 3];
    size_t n = 0;
    int w = snprintf(out, size, "{\n\t\"list\":\t[");
    if (w < 0 || (size_t)w >= size) return -1;
    n = (size_t)w;
    for (int i = 0; i < positions->count; ++i) {
        const struct mainui_position *e = &positions->entries[i];
        if (mainui_json_escape(rompath, sizeof(rompath), e->rompath, strlen(e->rompath)) < 0) return -1;
        w = snprintf(out + n, size - n, "%s{\n\t\t\t\"rompath\":\t%s,\n\t\t\t\"pos\":\t%d,\n\t\t\t\"start\":\t%d,\n\t\t\t\"end\":\t%d\n\t\t}",
                     i ? ", " : "", rompath, e->pos, e->start, e->end);
        if (w < 0 || (size_t)w >= size - n) return -1;
        n += (size_t)w;
    }
    w = snprintf(out + n, size - n, "]\n}");
    if (w < 0 || (size_t)w >= size - n) return -1;
    return (int)(n + (size_t)w);
}
