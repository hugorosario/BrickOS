#define _POSIX_C_SOURCE 200809L
#include "catalog-xml.h"

#include <ctype.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

// Small non-expanding XML reader for the stock gameList format. Elements and data
// retain their sibling order. Comments/declarations are skipped; CDATA remains a
// child node and does not become its parent's value (the stock RapidXML flags=0).
struct node {
    char *name, *text;
    const char *value;
    int parent, child, last, next;
};
struct document { struct node *nodes; int count, capacity; };

static void destroy(struct document *doc) {
    for (int i = 0; i < doc->count; ++i) { free(doc->nodes[i].name); free(doc->nodes[i].text); }
    free(doc->nodes);
}

static int add_node(struct document *doc, int parent, const char *name, size_t length, char *text) {
    if (doc->count == doc->capacity) {
        if (doc->capacity > INT_MAX / 2) { free(text); return -1; }
        int capacity = doc->capacity ? doc->capacity * 2 : 32;
        struct node *grown = realloc(doc->nodes, (size_t)capacity * sizeof(*grown));
        if (!grown) { free(text); return -1; }
        doc->nodes = grown; doc->capacity = capacity;
    }
    char *copy = name ? strndup(name, length) : NULL;
    if (name && !copy) { free(text); return -1; }
    int index = doc->count++;
    doc->nodes[index] = (struct node){copy, text, text ? text : "", parent, -1, -1, -1};
    if (parent >= 0) {
        struct node *p = &doc->nodes[parent];
        if (p->last >= 0) doc->nodes[p->last].next = index; else p->child = index;
        p->last = index;
    }
    return index;
}

static int utf8(unsigned long cp, char *out) {
    if (!cp || cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff)) return -1;
    if (cp < 0x80) { out[0] = (char)cp; return 1; }
    if (cp < 0x800) { out[0] = (char)(0xc0 | (cp >> 6)); out[1] = (char)(0x80 | (cp & 63)); return 2; }
    if (cp < 0x10000) {
        out[0] = (char)(0xe0 | (cp >> 12)); out[1] = (char)(0x80 | ((cp >> 6) & 63)); out[2] = (char)(0x80 | (cp & 63)); return 3;
    }
    out[0] = (char)(0xf0 | (cp >> 18)); out[1] = (char)(0x80 | ((cp >> 12) & 63));
    out[2] = (char)(0x80 | ((cp >> 6) & 63)); out[3] = (char)(0x80 | (cp & 63)); return 4;
}

static char *decode(const char *text, size_t length, int entities) {
    char *out = malloc(length + 1);
    if (!out) return NULL;
    size_t used = 0;
    for (size_t i = 0; i < length;) {
        if (entities && text[i] == '&') {
            static const struct { const char *name; char value; } predefined[] = {
                {"&amp;", '&'}, {"&lt;", '<'}, {"&gt;", '>'}, {"&quot;", '"'}, {"&apos;", '\''}
            };
            int matched = 0;
            for (size_t n = 0; n < sizeof(predefined) / sizeof(predefined[0]); ++n) {
                size_t size = strlen(predefined[n].name);
                if (size <= length - i && !memcmp(text + i, predefined[n].name, size)) {
                    out[used++] = predefined[n].value; i += size; matched = 1; break;
                }
            }
            if (matched) continue;
            if (i + 2 < length && text[i + 1] == '#') {
                size_t at = i + 2;
                int base = 10;
                if (text[at] == 'x') { base = 16; ++at; }
                unsigned long cp = 0;
                size_t first = at;
                while (at < length && text[at] != ';') {
                    unsigned char c = (unsigned char)text[at++];
                    unsigned long digit = c >= '0' && c <= '9' ? c - '0' :
                        base == 16 && c >= 'a' && c <= 'f' ? c - 'a' + 10 :
                        base == 16 && c >= 'A' && c <= 'F' ? c - 'A' + 10 : 16;
                    if (digit >= (unsigned long)base || cp > (0x10ffffUL - digit) / (unsigned long)base) goto invalid;
                    cp = cp * (unsigned long)base + digit;
                }
                if (at == first || at == length) goto invalid;
                int bytes = utf8(cp, out + used);
                if (bytes < 0) goto invalid;
                used += (size_t)bytes; i = at + 1; continue;
            }
        }
        out[used++] = text[i++];
    }
    out[used] = 0;
    return out;
invalid:
    free(out); return NULL;
}

static int whitespace(unsigned char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }
static int name_byte(unsigned char c) {
    return c >= 128 || isalnum(c) || c == '_' || c == ':' || c == '-' || c == '.';
}

static int parse(struct document *doc, const char *text) {
    if (add_node(doc, -1, "", 0, NULL) < 0) return -1;
    int parent = 0;
    const char *p = text;
    if (!strncmp(p, "\xef\xbb\xbf", 3)) p += 3;
    while (*p) {
        while (whitespace((unsigned char)*p)) ++p;
        if (!*p) break;
        if (*p != '<') {
            if (!parent) return -1;
            const char *end = strchr(p, '<');
            if (!end) return -1;
            char *value = decode(p, (size_t)(end - p), 1);
            if (!value) return -1;
            int index = add_node(doc, parent, NULL, 0, value);
            if (index < 0) return -1;
            if (!doc->nodes[parent].value[0]) doc->nodes[parent].value = doc->nodes[index].value;
            p = end; continue;
        }
        if (!strncmp(p, "<!--", 4) || !strncmp(p, "<?", 2)) {
            int comment = p[1] == '!';
            const char *end = strstr(p + (comment ? 4 : 2), comment ? "-->" : "?>");
            if (!end) return -1;
            p = end + (comment ? 3 : 2); continue;
        }
        if (!strncmp(p, "<![CDATA[", 9)) {
            if (!parent) return -1;
            const char *end = strstr(p + 9, "]]>");
            if (!end) return -1;
            char *value = decode(p + 9, (size_t)(end - p - 9), 0);
            if (!value || add_node(doc, parent, NULL, 0, value) < 0) return -1;
            p = end + 3; continue;
        }
        if (!strncmp(p, "<!DOCTYPE", 9)) {
            p += 9;
            int brackets = 0; char quote = 0;
            for (; *p; ++p) {
                if (quote) { if (*p == quote) quote = 0; }
                else if (*p == '"' || *p == '\'') quote = *p;
                else if (*p == '[') ++brackets;
                else if (*p == ']') { if (--brackets < 0) return -1; }
                else if (*p == '>' && !brackets) break;
            }
            if (!*p) return -1;
            ++p; continue;
        }
        ++p;
        int closing = *p == '/';
        if (closing) ++p;
        const char *name = p;
        while (name_byte((unsigned char)*p)) ++p;
        size_t length = (size_t)(p - name);
        if (!length) return -1;
        if (closing) {
            while (whitespace((unsigned char)*p)) ++p;
            if (!parent || *p != '>' || strlen(doc->nodes[parent].name) != length ||
                memcmp(doc->nodes[parent].name, name, length)) return -1;
            parent = doc->nodes[parent].parent; ++p; continue;
        }
        while (whitespace((unsigned char)*p)) ++p;
        while (*p && *p != '>' && *p != '/') {
            const char *attribute = p;
            while (name_byte((unsigned char)*p)) ++p;
            if (p == attribute) return -1;
            while (whitespace((unsigned char)*p)) ++p;
            if (*p++ != '=') return -1;
            while (whitespace((unsigned char)*p)) ++p;
            char quote = *p++;
            if (quote != '\'' && quote != '"') return -1;
            while (*p && *p != quote) ++p;
            if (!*p) return -1;
            ++p;
            while (whitespace((unsigned char)*p)) ++p;
        }
        int self_closed = *p == '/';
        if (self_closed) ++p;
        if (*p != '>') return -1;
        ++p;
        int index = add_node(doc, parent, name, length, NULL);
        if (index < 0) return -1;
        if (!self_closed) parent = index;
    }
    return parent ? -1 : 0;
}

static int child(const struct document *doc, int parent, const char *name) {
    for (int i = doc->nodes[parent].child; i >= 0; i = doc->nodes[i].next)
        if (doc->nodes[i].name && !strcmp(doc->nodes[i].name, name)) return i;
    return -1;
}

// The XML reader's fixed suffixes differ from a system's filesystem extlist.
// It compares a prefix at the final dot, including stock's combined .gbc.pce entry.
static int supported(const char *path) {
    static const char *const suffixes[] = {".zip", ".smc", ".fig", ".sfc", ".gd3", ".gd7", ".dx2", ".bsx", ".swc",
        ".gen", ".md", ".smd", ".32x", ".sms", ".rom", ".gg", ".sg", ".nes", ".fds", ".unf", ".unif",
        ".gba", ".agb", ".gbz", ".pbp", ".iso", ".cue", ".gb", ".gbc.pce"};
    const char *dot = strrchr(path, '.');
    if (!dot) return 0;
    for (size_t i = 0; i < sizeof(suffixes) / sizeof(suffixes[0]); ++i)
        if (!strncasecmp(dot, suffixes[i], strlen(suffixes[i]))) return 1;
    return 0;
}

static void label_initials(const char *label, char *out, size_t size) {
    char initials[256];
    out[0] = 0;
    if (mainui_catalog_pinyin(label, initials, sizeof(initials)) < 0) return;
    size_t used = 0;
    // readFromXML escapes the SQL disp string before deriving its initials.
    // The second SQL escape is undone on insertion, leaving doubled apostrophes.
    for (const char *p = initials; *p; ++p) {
        size_t bytes = *p == '\'' ? 2 : 1;
        if (used + bytes >= size) { out[0] = 0; return; }
        out[used++] = *p;
        if (bytes == 2) out[used++] = *p;
    }
    out[used] = 0;
}

int mainui_catalog_xml(const struct mainui_fs *fs, const struct mainui_system *system,
                       const struct mainui_names *names, const char *language, struct mainui_game **games) {
    *games = NULL;
    if (!system->gamelist[0]) return 0;
    char full[2048];
    if (snprintf(full, sizeof(full), "%s%s", fs->root ? fs->root : "", system->gamelist) >= (int)sizeof(full)) return -1;
    FILE *file = fopen(full, "rb");
    if (!file) return 0;
    char *text = NULL;
    long size = -1;
    if (!fseek(file, 0, SEEK_END)) size = ftell(file);
    if (size >= 0 && (unsigned long)size < SIZE_MAX && !fseek(file, 0, SEEK_SET)) {
        text = malloc((size_t)size + 1);
        if (text && (fread(text, 1, (size_t)size, file) != (size_t)size || memchr(text, 0, (size_t)size))) {
            free(text); text = NULL;
        }
        if (text) text[size] = 0;
    }
    fclose(file);
    if (!text) return -1;
    struct document doc = {0};
    if (parse(&doc, text) < 0) { destroy(&doc); free(text); return -1; }
    free(text);
    int root = child(&doc, 0, "gameList"), first = root < 0 ? -1 : child(&doc, root, "game");
    char directory[sizeof(system->gamelist)];
    memcpy(directory, system->gamelist, sizeof(directory));
    char *slash = strrchr(directory, '/');
    if (slash) *slash = 0;
    int count = 0, capacity = 0;
    struct mainui_game *out = NULL;
    for (int i = first; i >= 0; i = doc.nodes[i].next) {
        int path = child(&doc, i, "path");
        if (path < 0) break;
        struct mainui_game game = {0};
        if (snprintf(game.path, sizeof(game.path), "%s/%s", directory, doc.nodes[path].value) >= (int)sizeof(game.path) ||
            !supported(game.path)) continue;
        int label = child(&doc, i, "name"), image = child(&doc, i, "image");
        if (label < 0 || image < 0) break;
        if (!doc.nodes[path].value[0] || !doc.nodes[label].value[0]) continue;
        if (snprintf(game.label, sizeof(game.label), "%s", doc.nodes[label].value) >= (int)sizeof(game.label) ||
            snprintf(game.image, sizeof(game.image), "%s/%s", directory, doc.nodes[image].value) >= (int)sizeof(game.image) ||
            snprintf(full, sizeof(full), "%s%s", fs->root ? fs->root : "", game.path) >= (int)sizeof(full) || access(full, F_OK)) continue;
        const char *basename = strrchr(game.path, '/');
        if (snprintf(game.name, sizeof(game.name), "%s", basename ? basename + 1 : game.path) >= (int)sizeof(game.name)) continue;
        label_initials(game.label, game.pinyin, sizeof(game.pinyin));
        memcpy(game.opinyin, game.pinyin, sizeof(game.opinyin));
        if (system->shortname) {
            const char *display = mainui_names_label(names, language, game.label);
            if (display) snprintf(game.display, sizeof(game.display), "%s", display);
        }
        if (count == capacity) {
            if (capacity > INT_MAX / 2) { count = -1; break; }
            int next = capacity ? capacity * 2 : 32;
            struct mainui_game *grown = realloc(out, (size_t)next * sizeof(*grown));
            if (!grown) { count = -1; break; }
            out = grown; capacity = next;
        }
        out[count++] = game;
    }
    destroy(&doc);
    if (count <= 0) { free(out); out = NULL; }
    *games = out;
    return count;
}
