#define _POSIX_C_SOURCE 200809L
#include "extensions.h"

#include <dirent.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>

#include "json.h"
#include "model.h"

#define PLACEHOLDERS_MAX 256
#define PLACEHOLDER_VALUE_MAX 1024
#define NAME_MAX_BYTES 64

// ---- Small helpers --------------------------------------------------------------------------

static int under(const struct mainui_ext_registry *r, char *out, size_t size, const char *path) {
    return snprintf(out, size, "%s%s", r->root, path) < (int)size ? 0 : -1;
}

static int grow(void **array, int *capacity, int needed, size_t item) {
    if (needed <= *capacity) return 0;
    int next = *capacity ? *capacity * 2 : 16;
    while (next < needed) next *= 2;
    void *grown = realloc(*array, (size_t)next * item);
    if (!grown) return -1;
    *array = grown;
    *capacity = next;
    return 0;
}

static int is_regular(const char *path) {
    struct stat st;
    return stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

static int is_directory(const char *path) {
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

// A chained menu's name: one path component, so the file stays inside the extension's folder.
static int safe_name(const char *name) {
    if (!name[0] || strcmp(name, ".") == 0 || strcmp(name, "..") == 0 || strlen(name) >= 256) return 0;
    return !strchr(name, '/') && !strchr(name, '\\');
}

// An icon path relative to its folder that cannot leave it.
static int safe_relative(const char *path) {
    if (!path[0] || path[0] == '/' || strchr(path, '\\') || strlen(path) >= 256) return 0;
    for (const char *p = path; *p;) {
        const char *slash = strchr(p, '/');
        size_t n = slash ? (size_t)(slash - p) : strlen(p);
        if (n == 2 && p[0] == '.' && p[1] == '.') return 0;
        p += n;
        if (*p == '/') ++p;
    }
    return 1;
}

void mainui_ext_set_dry(struct mainui_ext_registry *r, FILE *record) {
    r->dry = 1;
    r->record = record;
}

void mainui_ext_log(struct mainui_ext_registry *r, const char *format, ...) {
    if (r->dry) {
        if (!r->record) return;
        va_list args;
        va_start(args, format);
        fputs("extension log: ", r->record);
        vfprintf(r->record, format, args);
        fputc('\n', r->record);
        va_end(args);
        fflush(r->record);
        return;
    }
    char path[MAINUI_EXT_PATH];
    if (under(r, path, sizeof(path), MAINUI_EXT_LOG) < 0) return;
    struct stat st;
    int truncate = stat(path, &st) == 0 && st.st_size >= MAINUI_EXT_LOG_MAX;
    FILE *f = fopen(path, truncate ? "w" : "a");
    if (!f) return;
    if (truncate) fputs("--- log truncated at 64 KiB ---\n", f);
    fprintf(f, "%lld ", (long long)time(NULL));
    va_list args;
    va_start(args, format);
    vfprintf(f, format, args);
    va_end(args);
    fputc('\n', f);
    fclose(f);
}

// ---- Files and signatures -------------------------------------------------------------------

static void signature(struct mainui_ext_file *f, const char *path) {
    struct stat st;
    snprintf(f->path, sizeof(f->path), "%s", path);
    f->exists = stat(path, &st) == 0;
    f->dev = f->exists ? st.st_dev : 0;
    f->ino = f->exists ? st.st_ino : 0;
    f->size = f->exists ? st.st_size : 0;
    f->mtime_ns = f->exists ? (long long)st.st_mtim.tv_sec * 1000000000LL + st.st_mtim.tv_nsec : 0;
}

static int same_signature(const struct mainui_ext_file *a, const struct mainui_ext_file *b) {
    return a->exists == b->exists && a->dev == b->dev && a->ino == b->ino && a->size == b->size &&
           a->mtime_ns == b->mtime_ns;
}

static void record_file(struct mainui_ext_registry *r, const char *path) {
    if (grow((void **)&r->files, &r->file_capacity, r->file_count + 1, sizeof(*r->files)) < 0) return;
    signature(&r->files[r->file_count++], path);
}

// ---- Discovery ------------------------------------------------------------------------------

struct candidate {
    char name[256];
    char dir[MAINUI_EXT_PATH];
    char file[MAINUI_EXT_PATH];
    int app;
};

static int by_name(const void *a, const void *b) {
    const struct candidate *x = a, *y = b;
    int c = strcasecmp(x->name, y->name);
    return c ? c : strcmp(x->name, y->name);
}

static int discover_in(struct mainui_ext_registry *r, const char *base, int app, struct candidate **list,
                       int *count, int *capacity) {
    char dir[MAINUI_EXT_PATH];
    if (under(r, dir, sizeof(dir), base) < 0) return 0;
    DIR *d = opendir(dir);
    if (!d) return 0;
    int first = *count;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.' || strlen(e->d_name) >= 256) continue;
        struct candidate c;
        memset(&c, 0, sizeof(c));
        snprintf(c.name, sizeof(c.name), "%s", e->d_name);
        if (snprintf(c.dir, sizeof(c.dir), "%s/%s", dir, e->d_name) >= (int)sizeof(c.dir)) continue;
        if (!is_directory(c.dir)) continue;
        // EmulationStation's rule: only <name>/<name>.json, with the exact name, is an extension.
        int n = app ? snprintf(c.file, sizeof(c.file), "%s/settings.json", c.dir)
                    : snprintf(c.file, sizeof(c.file), "%s/%s.json", c.dir, e->d_name);
        if (n >= (int)sizeof(c.file)) continue;
        if (!is_regular(c.file)) continue;
        c.app = app;
        if (grow((void **)list, capacity, *count + 1, sizeof(**list)) < 0) break;
        (*list)[(*count)++] = c;
    }
    closedir(d);
    qsort(*list + first, (size_t)(*count - first), sizeof(**list), by_name);
    return 0;
}

static int discover(struct mainui_ext_registry *r, struct candidate **list) {
    int count = 0, capacity = 0;
    *list = NULL;
    discover_in(r, MAINUI_EXT_SETTINGS_DIR, 0, list, &count, &capacity);
    discover_in(r, MAINUI_EXT_APPS_DIR, 1, list, &count, &capacity);
    return count;
}

// ---- Building the table ---------------------------------------------------------------------

struct build {
    struct mainui_ext_registry *r;
    int extension;
    const char *file;           // the file being read, for messages
    int items;
    int failed;
    char reason[256];
};

static void build_fail(struct build *b, const char *format, ...) __attribute__((format(printf, 2, 3)));
static void build_fail(struct build *b, const char *format, ...) {
    if (b->failed) return;
    b->failed = 1;
    va_list args;
    va_start(args, format);
    vsnprintf(b->reason, sizeof(b->reason), format, args);
    va_end(args);
}

static int add_folder(struct mainui_ext_registry *r, const char *dir, int extension) {
    if (grow((void **)&r->folders, &r->folder_capacity, r->folder_count + 1, sizeof(*r->folders)) < 0) return -1;
    struct mainui_ext_folder *f = &r->folders[r->folder_count];
    memset(f, 0, sizeof(*f));
    snprintf(f->dir, sizeof(f->dir), "%s", dir);
    f->extension = extension;
    return r->folder_count++;
}

static const struct mainui_json_value *load_tree(struct build *b, const char *path, int record) {
    struct mainui_ext_registry *r = b->r;
    if (record) record_file(r, path);
    if (grow((void **)&r->trees, &r->tree_capacity, r->tree_count + 1, sizeof(*r->trees)) < 0) {
        build_fail(b, "out of memory");
        return NULL;
    }
    char error[160];
    struct mainui_json_tree *tree = &r->trees[r->tree_count];
    if (mainui_json_tree_load(tree, path, error, sizeof(error)) < 0) {
        build_fail(b, "%s: %s", path, error);
        return NULL;
    }
    ++r->tree_count;
    return tree->root;
}

static int new_node(struct build *b) {
    struct mainui_ext_registry *r = b->r;
    if (grow((void **)&r->nodes, &r->node_capacity, r->node_count + 1, sizeof(*r->nodes)) < 0) {
        build_fail(b, "out of memory");
        return -1;
    }
    struct mainui_ext_node *n = &r->nodes[r->node_count];
    memset(n, 0, sizeof(*n));
    n->extension = b->extension;
    n->parent = n->first_child = n->next_sibling = -1;
    n->section = -1;
    return r->node_count++;
}

static void add_items(struct build *b, int parent, const struct mainui_json_value *list, int folder, int depth,
                      const char *prefix, int section, enum mainui_ext_anchor anchor, const char *anchor_id);

// The rows of a chained menu: <folder>/<name>/<name>.json, an array or an object with "menu".
static void add_chained(struct build *b, int parent, const char *name, int folder, int depth, const char *prefix) {
    struct mainui_ext_registry *r = b->r;
    if (!safe_name(name)) {
        build_fail(b, "chained menu \"%s\" leaves its folder", name);
        return;
    }
    char dir[MAINUI_EXT_PATH], path[MAINUI_EXT_PATH];
    if (snprintf(dir, sizeof(dir), "%s/%s", r->folders[folder].dir, name) >= (int)sizeof(dir) ||
        snprintf(path, sizeof(path), "%s/%s.json", dir, name) >= (int)sizeof(path)) {
        build_fail(b, "chained menu \"%s\": path too long", name);
        return;
    }
    const struct mainui_json_value *root = load_tree(b, path, 1);
    if (!root) return;
    const struct mainui_json_value *list = root->type == MAINUI_JSON_TREE_ARRAY ? root : mainui_json_tree_get(root, "menu");
    if (!list || list->type != MAINUI_JSON_TREE_ARRAY) {
        build_fail(b, "%s is not a menu file", path);
        return;
    }
    int chained = add_folder(r, dir, b->extension);
    if (chained < 0) { build_fail(b, "out of memory"); return; }
    add_items(b, parent, list, chained, depth, prefix, -1, MAINUI_EXT_ANCHOR_END, NULL);
}

static int kind_of(const char *type, enum mainui_ext_kind *kind) {
    static const struct { const char *name; enum mainui_ext_kind kind; } kinds[] = {
        {"menu", MAINUI_EXT_MENU}, {"cmd", MAINUI_EXT_CMD}, {"toggle", MAINUI_EXT_TOGGLE},
        {"select", MAINUI_EXT_SELECT}, {"range", MAINUI_EXT_RANGE}, {"info", MAINUI_EXT_INFO},
    };
    for (size_t i = 0; i < sizeof(kinds) / sizeof(kinds[0]); ++i)
        if (strcmp(kinds[i].name, type) == 0) { *kind = kinds[i].kind; return 0; }
    return -1;
}

static int clamp(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

static int whole(double v) { return v >= 2147483647.0 ? 2147483647 : v <= -2147483647.0 ? -2147483647 : (int)(v < 0 ? v - 0.5 : v + 0.5); }

// One item; returns its node, or -1 when it is skipped or the build failed.
static int add_item(struct build *b, const struct mainui_json_value *item, int folder, int depth, const char *path) {
    struct mainui_ext_registry *r = b->r;
    if (item->type != MAINUI_JSON_TREE_OBJECT) {
        mainui_ext_log(r, "%s: item %s skipped: not an object", b->file, path[0] ? path : "(extend)");
        return -1;
    }
    const char *label = mainui_json_tree_string(item, "label");
    if (!label) {
        mainui_ext_log(r, "%s: item %s skipped: no label", b->file, path[0] ? path : "(extend)");
        return -1;
    }
    // EmulationStation reads an item without a type as a command.
    const char *type = mainui_json_tree_string(item, "type");
    enum mainui_ext_kind kind;
    if (kind_of(type ? type : "cmd", &kind) < 0) {
        mainui_ext_log(r, "%s: item \"%s\" skipped: unknown type \"%s\"", b->file, label, type);
        return -1;
    }
    double number;
    int min = 0, max = 100, step = 1;
    if (kind == MAINUI_EXT_RANGE) {
        if (mainui_json_tree_number(item, "min", &number) == 0) min = whole(number);
        if (mainui_json_tree_number(item, "max", &number) == 0) max = whole(number);
        if (mainui_json_tree_number(item, "step", &number) == 0) step = whole(number);
        if (step <= 0) step = 1;
        if (max < min) {
            mainui_ext_log(r, "%s: item \"%s\" skipped: max is below min", b->file, label);
            return -1;
        }
    }
    const struct mainui_json_value *items = mainui_json_tree_get(item, "items");
    int item_count = 0;
    if (kind == MAINUI_EXT_SELECT) {
        if (items && items->type == MAINUI_JSON_TREE_ARRAY)
            for (const struct mainui_json_value *v = items->first; v; v = v->next) item_count += v->type == MAINUI_JSON_TREE_STRING;
        if (!item_count) {
            mainui_ext_log(r, "%s: item \"%s\" skipped: a select needs items", b->file, label);
            return -1;
        }
    }
    const char *icon = mainui_json_tree_string(item, "icon");
    if (icon && !safe_relative(icon)) {
        build_fail(b, "icon \"%s\" leaves its folder", icon);
        return -1;
    }
    if (++b->items > MAINUI_EXT_MAX_ITEMS) {
        build_fail(b, "more than %d items", MAINUI_EXT_MAX_ITEMS);
        return -1;
    }
    int node = new_node(b);
    if (node < 0) return -1;
    struct mainui_ext_node *n = &r->nodes[node];
    n->kind = kind;
    n->folder = folder;
    n->depth = depth;
    snprintf(n->path, sizeof(n->path), "%s", path);
    n->id = mainui_json_tree_string(item, "id");
    n->label = label;
    n->description = mainui_json_tree_string(item, "description");
    n->icon = icon;
    n->execute = mainui_json_tree_string(item, "execute");
    n->load = mainui_json_tree_string(item, "load");
    n->value_text = kind == MAINUI_EXT_INFO ? mainui_json_tree_string(item, "value") : NULL;
    const char *output = mainui_json_tree_string(item, "output");
    n->output = output && strcmp(output, "line") == 0 ? MAINUI_EXT_OUTPUT_LINE
              : output && strcmp(output, "full") == 0 ? MAINUI_EXT_OUTPUT_FULL : MAINUI_EXT_OUTPUT_NONE;
    n->min = min;
    n->max = max;
    n->step = step;
    if (kind == MAINUI_EXT_SELECT) {
        n->items = malloc((size_t)item_count * sizeof(*n->items));
        if (!n->items) { build_fail(b, "out of memory"); return -1; }
        for (const struct mainui_json_value *v = items->first; v; v = v->next)
            if (v->type == MAINUI_JSON_TREE_STRING) n->items[n->item_count++] = v->string;
    }
    int value = 0;
    if (mainui_json_tree_number(item, kind == MAINUI_EXT_RANGE ? "value" : "selected", &number) == 0) value = whole(number);
    else if (kind == MAINUI_EXT_RANGE) value = min;
    n->value = n->initial = n->applied = kind == MAINUI_EXT_TOGGLE ? value != 0
                          : kind == MAINUI_EXT_SELECT ? clamp(value, 0, item_count - 1)
                          : kind == MAINUI_EXT_RANGE ? clamp(value, min, max) : 0;
    n->disabled = mainui_json_tree_number(item, "disable", &number) == 0 && number != 0;
    return node;
}

static void add_items(struct build *b, int parent, const struct mainui_json_value *list, int folder, int depth,
                      const char *prefix, int section, enum mainui_ext_anchor anchor, const char *anchor_id) {
    struct mainui_ext_registry *r = b->r;
    if (depth > MAINUI_EXT_MAX_DEPTH) {
        build_fail(b, "menus nested deeper than %d levels", MAINUI_EXT_MAX_DEPTH);
        return;
    }
    int last = -1, index = 0;
    for (const struct mainui_json_value *item = list->first; item && !b->failed; item = item->next, ++index) {
        char path[64] = "";
        if (section < 0) {
            if (prefix[0]) snprintf(path, sizeof(path), "%s.%d", prefix, index);
            else snprintf(path, sizeof(path), "%d", index);
        }
        int node = add_item(b, item, folder, depth, path);
        if (node < 0) continue;
        struct mainui_ext_node *n = &r->nodes[node];
        n->parent = parent;
        n->section = section;
        n->anchor = anchor;
        n->anchor_id = anchor_id;
        if (parent >= 0) {
            if (last < 0) r->nodes[parent].first_child = node;
            else r->nodes[last].next_sibling = node;
            ++r->nodes[parent].child_count;
        }
        last = node;
        if (n->kind != MAINUI_EXT_MENU) continue;
        const struct mainui_json_value *children = mainui_json_tree_get(item, "menu");
        if (children && children->type == MAINUI_JSON_TREE_ARRAY)
            add_items(b, node, children, folder, depth + 1, path, -1, MAINUI_EXT_ANCHOR_END, NULL);
        else if (children && children->type == MAINUI_JSON_TREE_STRING)
            add_chained(b, node, children->string, folder, depth + 1, path);
    }
}

static void add_extend(struct build *b, const struct mainui_json_value *extend, int folder) {
    struct mainui_ext_registry *r = b->r;
    int index = 0;
    for (const struct mainui_json_value *entry = extend->first; entry && !b->failed; entry = entry->next, ++index) {
        const char *section = mainui_json_tree_string(entry, "section");
        int screen = section ? mainui_model_extension_section(section) : -1;
        if (screen < 0) {
            mainui_ext_log(r, "%s: extend entry %d skipped: unknown section \"%s\"", b->file, index, section ? section : "");
            continue;
        }
        const char *after = mainui_json_tree_string(entry, "after"), *before = mainui_json_tree_string(entry, "before");
        enum mainui_ext_anchor anchor = after ? MAINUI_EXT_ANCHOR_AFTER : before ? MAINUI_EXT_ANCHOR_BEFORE : MAINUI_EXT_ANCHOR_END;
        const struct mainui_json_value *rows = mainui_json_tree_get(entry, "menu");
        if (!rows || rows->type != MAINUI_JSON_TREE_ARRAY) {
            mainui_ext_log(r, "%s: extend entry %d skipped: no menu list", b->file, index);
            continue;
        }
        add_items(b, -1, rows, folder, 1, "", screen, anchor, after ? after : before);
    }
}

static void app_label(const char *dir, char *out, size_t size, const char *fallback) {
    char path[MAINUI_EXT_PATH];
    snprintf(out, size, "%s", fallback);
    if (snprintf(path, sizeof(path), "%s/config.json", dir) >= (int)sizeof(path)) return;
    FILE *f = fopen(path, "rb");
    if (!f) return;
    char text[16 * 1024];
    size_t n = fread(text, 1, sizeof(text), f);
    fclose(f);
    static struct mainui_json json;
    if (mainui_json_parse_fitting(&json, text, n) < 0) return;
    const struct mainui_json_member *label = mainui_json_get(&json, "label");
    if (label && label->type == MAINUI_JSON_STRING && label->text[0] && snprintf(out, size, "%s", label->text) >= (int)size)
        out[size - 1] = 0;
}

static void free_node_memory(struct mainui_ext_registry *r, int from) {
    for (int i = from; i < r->node_count; ++i) free((void *)r->nodes[i].items);
}

static void free_folder_catalog(struct mainui_ext_folder *f) {
    free(f->entries);
    f->entries = NULL;
    f->entry_count = 0;
    mainui_json_tree_free(&f->tree);
    f->loaded = 0;
}

static void load_extension(struct mainui_ext_registry *r, const struct candidate *c) {
    if (grow((void **)&r->extensions, &r->extension_capacity, r->extension_count + 1, sizeof(*r->extensions)) < 0) return;
    // Rolled back when the extension is skipped. Its files stay recorded, so fixing one rebuilds.
    int nodes = r->node_count, folders = r->folder_count, trees = r->tree_count;
    int index = r->extension_count;
    struct mainui_ext_extension *e = &r->extensions[index];
    memset(e, 0, sizeof(*e));
    snprintf(e->name, sizeof(e->name), "%s", c->name);
    snprintf(e->dir, sizeof(e->dir), "%s", c->dir);
    e->app = c->app;
    e->root = -1;
    e->first_node = r->node_count;
    if (c->app) app_label(c->dir, e->fallback_title, sizeof(e->fallback_title), c->name);
    else snprintf(e->fallback_title, sizeof(e->fallback_title), "%s", c->name);

    struct build b = { r, index, c->file, 0, 0, "" };
    e->folder = add_folder(r, c->dir, index);
    const struct mainui_json_value *top = e->folder < 0 ? NULL : load_tree(&b, c->file, 0);
    const struct mainui_json_value *menu = NULL, *extend = NULL;
    const char *icon = NULL;
    if (top && top->type == MAINUI_JSON_TREE_ARRAY) {
        menu = top;
    } else if (top && top->type == MAINUI_JSON_TREE_OBJECT) {
        e->title = mainui_json_tree_string(top, "title");
        icon = mainui_json_tree_string(top, "icon");
        menu = mainui_json_tree_get(top, "menu");
        extend = mainui_json_tree_get(top, "extend");
        if (menu && menu->type != MAINUI_JSON_TREE_ARRAY && menu->type != MAINUI_JSON_TREE_STRING) menu = NULL;
        if (extend && extend->type != MAINUI_JSON_TREE_ARRAY) extend = NULL;
        if (!menu && !extend) build_fail(&b, "%s is not a menu file", c->file);
    } else if (top) {
        build_fail(&b, "%s is not a menu file", c->file);
    } else if (e->folder < 0) {
        build_fail(&b, "out of memory");
    }
    if (icon && !safe_relative(icon)) build_fail(&b, "icon \"%s\" leaves its folder", icon);
    if (!b.failed && menu) {
        int root = new_node(&b);
        if (root >= 0) {
            e = &r->extensions[index];
            e->root = root;
            r->nodes[root].kind = MAINUI_EXT_MENU;
            r->nodes[root].folder = e->folder;
            r->nodes[root].label = e->title;
            if (menu->type == MAINUI_JSON_TREE_ARRAY) add_items(&b, root, menu, e->folder, 1, "", -1, MAINUI_EXT_ANCHOR_END, NULL);
            else add_chained(&b, root, menu->string, e->folder, 1, "");
        }
    }
    if (!b.failed && extend) add_extend(&b, extend, e->folder);
    e = &r->extensions[index];
    if (b.failed) {
        mainui_ext_log(r, "%s skipped: %s", c->file, b.reason);
        free_node_memory(r, nodes);
        r->node_count = nodes;
        for (int i = folders; i < r->folder_count; ++i) free_folder_catalog(&r->folders[i]);
        r->folder_count = folders;
        for (int i = trees; i < r->tree_count; ++i) mainui_json_tree_free(&r->trees[i]);
        r->tree_count = trees;
        return;
    }
    e->node_count = r->node_count - e->first_node;
    // The tile's icon: its own, else icon.png in the folder, else the generic one.
    char path[MAINUI_EXT_PATH];
    if (icon && snprintf(path, sizeof(path), "%s/%s", c->dir, icon) < (int)sizeof(path) && is_regular(path))
        snprintf(e->icon, sizeof(e->icon), "%s", path);
    else if (snprintf(path, sizeof(path), "%s/icon.png", c->dir) < (int)sizeof(path) && is_regular(path))
        snprintf(e->icon, sizeof(e->icon), "%s", path);
    ++r->extension_count;
    if (e->root >= 0) {
        if (r->tile_count < MAINUI_EXT_MAX_TILES) r->tiles[r->tile_count++] = index;
        else mainui_ext_log(r, "%s: no tile, more than %d extension tiles", c->file, MAINUI_EXT_MAX_TILES);
    }
}

static void clear_table(struct mainui_ext_registry *r) {
    free_node_memory(r, 0);
    r->node_count = 0;
    for (int i = 0; i < r->extension_count; ++i) {
        for (int p = 0; p < r->extensions[i].placeholder_count; ++p) {
            free(r->extensions[i].placeholders[p].key);
            free(r->extensions[i].placeholders[p].value);
        }
        free(r->extensions[i].placeholders);
    }
    r->extension_count = 0;
    for (int i = 0; i < r->folder_count; ++i) free_folder_catalog(&r->folders[i]);
    r->folder_count = 0;
    for (int i = 0; i < r->tree_count; ++i) mainui_json_tree_free(&r->trees[i]);
    r->tree_count = 0;
    r->file_count = 0;
    r->tile_count = 0;
}

void mainui_ext_init(struct mainui_ext_registry *r, const char *root) {
    memset(r, 0, sizeof(*r));
    snprintf(r->root, sizeof(r->root), "%s", root ? root : "");
    snprintf(r->language, sizeof(r->language), "en");
}

void mainui_ext_free(struct mainui_ext_registry *r) {
    clear_table(r);
    free(r->extensions);
    free(r->nodes);
    free(r->folders);
    free(r->files);
    free(r->trees);
    char root[MAINUI_EXT_PATH], language[32];
    int dry = r->dry;
    FILE *record = r->record;
    memcpy(root, r->root, sizeof(root));
    memcpy(language, r->language, sizeof(language));
    memset(r, 0, sizeof(*r));
    memcpy(r->root, root, sizeof(root));
    memcpy(r->language, language, sizeof(language));
    r->dry = dry;
    r->record = record;
}

// The files recorded for the table are the found extension files first, in order, then those
// read while building. The table is unchanged when the same extension files are found and no
// recorded file changed.
static int unchanged(struct mainui_ext_registry *r, const struct candidate *list, int count) {
    if (!r->built || r->main_count != count) return 0;
    for (int i = 0; i < count; ++i)
        if (strcmp(r->files[i].path, list[i].file) != 0) return 0;
    for (int i = 0; i < r->file_count; ++i) {
        struct mainui_ext_file now;
        signature(&now, r->files[i].path);
        if (!same_signature(&now, &r->files[i])) return 0;
    }
    return 1;
}

int mainui_ext_scan(struct mainui_ext_registry *r) {
    struct candidate *list;
    int count = discover(r, &list);
    // Catalogs are cheap to read again and are not part of the node table.
    for (int i = 0; i < r->folder_count; ++i) free_folder_catalog(&r->folders[i]);
    if (unchanged(r, list, count)) {
        free(list);
        return 0;
    }
    clear_table(r);
    for (int i = 0; i < count; ++i) record_file(r, list[i].file);
    r->main_count = count;
    for (int i = 0; i < count; ++i) load_extension(r, &list[i]);
    free(list);
    r->built = 1;
    ++r->generation;
    return 1;
}

// ---- Access ---------------------------------------------------------------------------------

const struct mainui_ext_node *mainui_ext_node(const struct mainui_ext_registry *r, int node) {
    return node >= 0 && node < r->node_count ? &r->nodes[node] : NULL;
}

int mainui_ext_tile_count(const struct mainui_ext_registry *r) {
    return r->tile_count;
}

int mainui_ext_tile_extension(const struct mainui_ext_registry *r, int tile) {
    return tile >= 0 && tile < r->tile_count ? r->tiles[tile] : -1;
}

int mainui_ext_tile_node(const struct mainui_ext_registry *r, int tile) {
    int e = mainui_ext_tile_extension(r, tile);
    return e < 0 ? -1 : r->extensions[e].root;
}

int mainui_ext_find(const struct mainui_ext_registry *r, int extension, const char *path_or_id) {
    if (extension < 0 || extension >= r->extension_count || !path_or_id[0]) return -1;
    const struct mainui_ext_extension *e = &r->extensions[extension];
    for (int i = e->first_node; i < e->first_node + e->node_count; ++i)
        if (r->nodes[i].path[0] && strcmp(r->nodes[i].path, path_or_id) == 0) return i;
    for (int i = e->first_node; i < e->first_node + e->node_count; ++i)
        if (r->nodes[i].id && strcmp(r->nodes[i].id, path_or_id) == 0) return i;
    return -1;
}

const char *mainui_ext_node_dir(const struct mainui_ext_registry *r, int node) {
    const struct mainui_ext_node *n = mainui_ext_node(r, node);
    return n && n->folder >= 0 && n->folder < r->folder_count ? r->folders[n->folder].dir : "";
}

static int clamp_value(const struct mainui_ext_node *n, int value) {
    switch (n->kind) {
    case MAINUI_EXT_TOGGLE: return value != 0;
    case MAINUI_EXT_SELECT: return clamp(value, 0, n->item_count - 1);
    case MAINUI_EXT_RANGE: return clamp(value, n->min, n->max);
    default: return 0;
    }
}

void mainui_ext_set_value(struct mainui_ext_registry *r, int node, int value) {
    if (node >= 0 && node < r->node_count) r->nodes[node].value = r->nodes[node].applied = clamp_value(&r->nodes[node], value);
}

void mainui_ext_show_value(struct mainui_ext_registry *r, int node, int value) {
    if (node >= 0 && node < r->node_count) r->nodes[node].value = clamp_value(&r->nodes[node], value);
}

void mainui_ext_set_busy(struct mainui_ext_registry *r, int node, int busy) {
    if (node >= 0 && node < r->node_count) r->nodes[node].busy = busy;
}

void mainui_ext_set_disabled(struct mainui_ext_registry *r, int node, int disabled) {
    if (node >= 0 && node < r->node_count) r->nodes[node].disabled = disabled != 0;
}

// ---- Translation ----------------------------------------------------------------------------

void mainui_ext_set_language(struct mainui_ext_registry *r, const char *language) {
    char code[32];
    snprintf(code, sizeof(code), "%s", language && language[0] ? language : "en.lang");
    size_t n = strlen(code);
    if (n > 5 && strcmp(code + n - 5, ".lang") == 0) code[n - 5] = 0;
    if (strcmp(code, r->language) == 0) return;
    snprintf(r->language, sizeof(r->language), "%s", code);
    for (int i = 0; i < r->folder_count; ++i) free_folder_catalog(&r->folders[i]);
}

static int by_key(const void *a, const void *b) {
    const struct mainui_ext_catalog_entry *x = a, *y = b;
    int c = strcmp(x->key, y->key);
    return c ? c : x->order - y->order;
}

static void load_catalog(struct mainui_ext_registry *r, struct mainui_ext_folder *f) {
    f->loaded = 1;
    if (!safe_name(r->language)) return;
    char path[MAINUI_EXT_PATH], error[160];
    if (snprintf(path, sizeof(path), "%s/lang/%s.json", f->dir, r->language) >= (int)sizeof(path)) return;
    if (!is_regular(path)) return;
    if (mainui_json_tree_load(&f->tree, path, error, sizeof(error)) < 0) {
        mainui_ext_log(r, "catalog %s ignored: %s", path, error);
        return;
    }
    const struct mainui_json_value *root = f->tree.root;
    if (root->type != MAINUI_JSON_TREE_OBJECT) {
        mainui_ext_log(r, "catalog %s ignored: not an object", path);
        mainui_json_tree_free(&f->tree);
        return;
    }
    f->entries = malloc((root->length ? root->length : 1) * sizeof(*f->entries));
    if (!f->entries) return;
    int order = 0;
    for (const struct mainui_json_value *m = root->first; m; m = m->next, ++order) {
        // display_name is EmulationStation's name for the language itself.
        if (m->type != MAINUI_JSON_TREE_STRING || strcmp(m->key, "display_name") == 0) continue;
        f->entries[f->entry_count++] = (struct mainui_ext_catalog_entry){ m->key, m->string, order };
    }
    qsort(f->entries, (size_t)f->entry_count, sizeof(*f->entries), by_key);
}

static const char *lookup(struct mainui_ext_registry *r, int folder, const char *text) {
    if (folder < 0 || folder >= r->folder_count) return NULL;
    struct mainui_ext_folder *f = &r->folders[folder];
    if (!f->loaded) load_catalog(r, f);
    int lo = 0, hi = f->entry_count;
    while (lo < hi) {
        int mid = (lo + hi) / 2;
        if (strcmp(f->entries[mid].key, text) < 0) lo = mid + 1;
        else hi = mid;
    }
    // The last of equal keys: a later duplicate wins.
    const char *found = NULL;
    for (; lo < f->entry_count && strcmp(f->entries[lo].key, text) == 0; ++lo) found = f->entries[lo].value;
    return found;
}

const char *mainui_ext_translate(struct mainui_ext_registry *r, int folder, const char *text) {
    if (!text) return "";
    const char *found = lookup(r, folder, text);
    if (!found && folder >= 0 && folder < r->folder_count) {
        int extension = r->folders[folder].extension;
        int top = extension >= 0 && extension < r->extension_count ? r->extensions[extension].folder : -1;
        if (top != folder) found = lookup(r, top, text);
    }
    return found ? found : text;
}

// ---- Placeholders and texts -----------------------------------------------------------------

static int placeholder_name(const char *s, size_t n) {
    if (n == 0 || n >= NAME_MAX_BYTES) return 0;
    for (size_t i = 0; i < n; ++i) {
        char c = s[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.'))
            return 0;
    }
    return 1;
}

static const char *placeholder(const struct mainui_ext_registry *r, int extension, const char *name, size_t n) {
    if (extension < 0 || extension >= r->extension_count) return NULL;
    const struct mainui_ext_extension *e = &r->extensions[extension];
    for (int i = 0; i < e->placeholder_count; ++i)
        if (strlen(e->placeholders[i].key) == n && memcmp(e->placeholders[i].key, name, n) == 0) return e->placeholders[i].value;
    return NULL;
}

static void set_placeholder(struct mainui_ext_registry *r, int extension, const char *name, size_t n, const char *value, size_t length) {
    if (extension < 0 || extension >= r->extension_count) return;
    struct mainui_ext_extension *e = &r->extensions[extension];
    if (length >= PLACEHOLDER_VALUE_MAX) length = PLACEHOLDER_VALUE_MAX - 1;
    char *copy = malloc(length + 1);
    if (!copy) return;
    memcpy(copy, value, length);
    copy[length] = 0;
    for (int i = 0; i < e->placeholder_count; ++i)
        if (strlen(e->placeholders[i].key) == n && memcmp(e->placeholders[i].key, name, n) == 0) {
            free(e->placeholders[i].value);
            e->placeholders[i].value = copy;
            return;
        }
    if (e->placeholder_count >= PLACEHOLDERS_MAX) { free(copy); return; }
    struct mainui_ext_placeholder *grown = realloc(e->placeholders, (size_t)(e->placeholder_count + 1) * sizeof(*grown));
    if (grown) e->placeholders = grown;
    char *key = grown ? malloc(n + 1) : NULL;
    if (!key) {
        free(copy);
        return;
    }
    memcpy(key, name, n);
    key[n] = 0;
    e->placeholders[e->placeholder_count++] = (struct mainui_ext_placeholder){ key, copy };
}

static void append(char *out, size_t size, size_t *n, const char *s, size_t length) {
    if (*n + 1 >= size) return;
    if (length > size - 1 - *n) length = size - 1 - *n;
    memcpy(out + *n, s, length);
    *n += length;
    out[*n] = 0;
}

void mainui_ext_format(struct mainui_ext_registry *r, int extension, int folder, const char *text, char *out, size_t size) {
    if (!size) return;
    out[0] = 0;
    const char *s = mainui_ext_translate(r, folder, text);
    size_t n = 0;
    while (*s) {
        const char *open = strstr(s, "{{");
        if (!open) { append(out, size, &n, s, strlen(s)); break; }
        append(out, size, &n, s, (size_t)(open - s));
        const char *close = strstr(open + 2, "}}");
        size_t name = close ? (size_t)(close - open - 2) : 0;
        if (!close || !placeholder_name(open + 2, name)) {
            append(out, size, &n, "{{", 2);
            s = open + 2;
            continue;
        }
        const char *value = placeholder(r, extension, open + 2, name);
        if (value) append(out, size, &n, value, strlen(value));
        s = close + 2;
    }
}

int mainui_ext_text(struct mainui_ext_registry *r, int node, enum mainui_ext_text which, char *out, size_t size) {
    if (size) out[0] = 0;
    const struct mainui_ext_node *n = mainui_ext_node(r, node);
    if (!n) return -1;
    const char *text = NULL;
    switch (which) {
    case MAINUI_EXT_TEXT_LABEL:
        text = n->label;
        if (!text && r->extensions[n->extension].root == node) text = r->extensions[n->extension].fallback_title;
        break;
    case MAINUI_EXT_TEXT_DESCRIPTION: text = n->description; break;
    case MAINUI_EXT_TEXT_VALUE: text = n->value_text; break;
    case MAINUI_EXT_TEXT_ITEM: text = n->kind == MAINUI_EXT_SELECT ? n->items[n->value] : NULL; break;
    }
    if (!text) return -1;
    mainui_ext_format(r, n->extension, n->folder, text, out, size);
    return 0;
}

void mainui_ext_tile_title(struct mainui_ext_registry *r, int tile, char *out, size_t size) {
    int e = mainui_ext_tile_extension(r, tile);
    if (size) out[0] = 0;
    if (e < 0) return;
    const struct mainui_ext_extension *x = &r->extensions[e];
    mainui_ext_format(r, e, x->folder, x->title ? x->title : x->fallback_title, out, size);
}

// ---- Script output --------------------------------------------------------------------------

static int parse_int(const char *s, size_t n, int *value) {
    char copy[24];
    if (n == 0 || n >= sizeof(copy)) return -1;
    memcpy(copy, s, n);
    copy[n] = 0;
    char *rest;
    errno = 0;
    long v = strtol(copy, &rest, 10);
    if (*rest || errno || v < -2147483647L || v > 2147483647L) return -1;
    *value = (int)v;
    return 0;
}

// Applies one {{name=value}} tag; returns 1 when it is a tag, 0 when it is malformed.
static int apply_tag(struct mainui_ext_registry *r, int node, const char *body, size_t length) {
    int extension = r->nodes[node].extension;
    const char *equals = memchr(body, '=', length);
    if (!equals) return 0;
    size_t name = (size_t)(equals - body);
    const char *value = equals + 1;
    size_t value_length = length - name - 1;
    int number;
    for (int disable = 0; disable < 2; ++disable) {
        const char *word = disable ? "disable" : "state";
        size_t w = strlen(word);
        if (name < w || memcmp(body, word, w) != 0 || (name > w && body[w] != '(')) continue;
        int target = node;
        if (name > w) {
            // word(P): an index path or an id of the same extension.
            if (body[name - 1] != ')' || name - w - 2 == 0 || name - w - 2 >= 64) return 0;
            char path[64];
            memcpy(path, body + w + 1, name - w - 2);
            path[name - w - 2] = 0;
            target = mainui_ext_find(r, extension, path);
        }
        if (parse_int(value, value_length, &number) < 0) return 0;
        if (target >= 0) {
            if (disable) mainui_ext_set_disabled(r, target, number);
            else mainui_ext_set_value(r, target, number);
        }
        return 1;
    }
    if (!placeholder_name(body, name)) return 0;
    set_placeholder(r, extension, body, name, value, value_length);
    return 1;
}

enum mainui_ext_line mainui_ext_apply_line(struct mainui_ext_registry *r, int node, const char *line,
                                           char *visible, size_t size) {
    if (size) visible[0] = 0;
    if (node < 0 || node >= r->node_count) return MAINUI_EXT_LINE_HIDDEN;
    if (strncmp(line, "confirm:", 8) == 0) {
        const char *prompt = line + 8;
        while (*prompt == ' ') ++prompt;
        snprintf(visible, size, "%s", prompt);
        return MAINUI_EXT_LINE_CONFIRM;
    }
    int tags = 0;
    for (const char *at = strstr(line, "{{"); at; at = strstr(at + 2, "{{")) {
        const char *close = strstr(at + 2, "}}");
        if (!close) break;
        if (apply_tag(r, node, at + 2, (size_t)(close - at - 2))) {
            ++tags;
            at = close;
        }
    }
    if (tags) return MAINUI_EXT_LINE_HIDDEN;
    size_t n = 0;
    for (const char *p = line; *p; ++p) {
        if (p[0] == '\\' && p[1] == 'n') { append(visible, size, &n, "\n", 1); ++p; }
        else append(visible, size, &n, p, 1);
    }
    return MAINUI_EXT_LINE_VISIBLE;
}
