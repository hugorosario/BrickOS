#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE 700
#include "catalog.h"
#include "catalog-xml.h"
#include "json.h"
#include "pinyin-data.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#define NAMES_INITIAL 64
#define PATH_BYTES 1024

int mainui_readdir(void *context, const char *path, char names[][256], int max) {
    (void)context;
    DIR *dir = opendir(path);
    if (!dir) return -1;
    int n = 0;
    struct dirent *entry;
    while ((entry = readdir(dir)) && n < max) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) continue;
        size_t length = strlen(entry->d_name);
        if (length > 255) continue;
        memcpy(names[n++], entry->d_name, length + 1);
    }
    closedir(dir);
    return n;
}

// Joins up to four strings into out; returns -1, leaving out empty, when they do not fit.
static int cat(char *out, size_t size, const char *a, const char *b, const char *c, const char *d) {
    const char *parts[4] = {a, b, c, d};
    size_t n = 0;
    for (int i = 0; i < 4; ++i) {
        size_t length = strlen(parts[i]);
        if (n + length >= size) { out[0] = 0; return -1; }
        memcpy(out + n, parts[i], length);
        n += length;
    }
    out[n] = 0;
    return 0;
}

static void device_path(const struct mainui_fs *fs, char *out, const char *path) {
    cat(out, PATH_BYTES, fs->root ? fs->root : "", path, "", "");
}

// getFileType (0x4752e4) uses suffix prefixes rather than whole-extension matches.
// These are ROM types; it does not recognize arbitrary pictures or audio files.
int mainui_catalog_file_type(const char *name) {
    static const struct { const char *suffix; int type; } types[] = {
        {".smc", 8}, {".fig", 8}, {".sfc", 8}, {".gd3", 8}, {".gd7", 8},
        {".dx2", 8}, {".bsx", 8}, {".swc", 8},
        {".gen", 10}, {".md", 10}, {".smd", 10}, {".32x", 10},
        {".sms", 12}, {".rom", 12}, {".gg", 12}, {".sg", 12},
        {".nes", 7}, {".fds", 7}, {".unf", 7}, {".unif", 7},
        {".gba", 11}, {".agb", 11}, {".gbz", 11},
        {".pbp", 14}, {".iso", 14}, {".cue", 14},
        {".gb", 9}, {".gbc", 9}, {".pce", 13},
    };
    const char *dot = name ? strrchr(name, '.') : NULL;
    if (!dot) return -1;
    for (size_t i = 0; i < sizeof(types) / sizeof(types[0]); ++i)
        if (!strncasecmp(dot, types[i].suffix, strlen(types[i].suffix))) return types[i].type;
    return -1;
}

int mainui_catalog_file_system(const struct mainui_system *systems, int count, const char *name) {
    const char *dot = name ? strrchr(name, '.') : NULL;
    if (!dot || !systems) return -1;
    for (int i = 0; i < count; ++i) {
        for (const char *p = systems[i].extlist; *p;) {
            const char *end = strchr(p, '|');
            size_t n = end ? (size_t)(end - p) : strlen(p);
            if (n && !strncasecmp(dot + 1, p, n)) return i;
            p += n;
            if (*p == '|') ++p;
        }
    }
    return -1;
}

int mainui_catalog_file_allowed(const struct mainui_system *systems, int count,
                               const char *name, const char *path) {
    if (!name || !path) return 0;
    if (mainui_catalog_file_type(name) > 0 || mainui_catalog_file_system(systems, count, name) >= 0) return 1;
    const char *dot = strrchr(name, '.');
    return (dot && !strncasecmp(name, "trimui_", 7) && !strncasecmp(dot, ".img", 4)) ||
        !strncmp(name, "COPYING", 7) || !strncasecmp(name, "LICENSE", 7) || strstr(path, "/usr/share/zoneinfo") != NULL;
}

static int compare_files(const void *a, const void *b) {
    const struct mainui_file *left = a, *right = b;
    return left->folder != right->folder ? right->folder - left->folder : strcmp(left->name, right->name);
}

int mainui_catalog_files(const struct mainui_fs *fs, const struct mainui_system *systems,
                         int system_count, const char *path, int include_parent, struct mainui_file **files) {
    if (!files) return -1;
    *files = NULL;
    if (!fs || !path || path[0] != '/') return -1;
    char full[PATH_BYTES], child[PATH_BYTES], device_child[PATH_BYTES];
    device_path(fs, full, path);
    DIR *dir = full[0] ? opendir(full) : NULL;
    if (!dir) return -1;
    size_t capacity = 32;
    struct mainui_file *rows = malloc(capacity * sizeof(*rows));
    if (!rows) { closedir(dir); return -1; }
    int n = 0, failed = 0;
    struct dirent *entry;
    while (n < 20000 && (entry = readdir(dir))) {
        const char *name = entry->d_name;
        struct stat st;
        if (strlen(name) > 255 || cat(device_child, sizeof(device_child), path,
            path[strlen(path) - 1] == '/' ? "" : "/", name, "") < 0) continue;
        device_path(fs, child, device_child);
        if (!child[0] || stat(child, &st) != 0) continue;
        int folder = S_ISDIR(st.st_mode);
        if (folder ? (name[0] == '.' && (!include_parent || strncmp(name, "..", 2))) :
            (!S_ISREG(st.st_mode) || !mainui_catalog_file_allowed(systems, system_count, name, device_child))) continue;
        if ((size_t)n == capacity) {
            capacity = capacity * 2 < 20000 ? capacity * 2 : 20000;
            struct mainui_file *grown = realloc(rows, capacity * sizeof(*rows));
            if (!grown) { failed = 1; break; }
            rows = grown;
        }
        snprintf(rows[n].name, sizeof(rows[n].name), "%s", name);
        rows[n++].folder = folder;
    }
    closedir(dir);
    if (failed) { free(rows); return -1; }
    qsort(rows, (size_t)n, sizeof(*rows), compare_files);
    *files = rows;
    return n;
}

static char *read_text(const char *path, size_t *length) {
    FILE *file = fopen(path, "rb");
    if (!file) return NULL;
    size_t capacity = 8192, n = 0;
    char *text = malloc(capacity);
    while (text) {
        size_t got = fread(text + n, 1, capacity - n - 1, file);
        n += got;
        if (got == 0) break;
        if (n + 1 >= capacity) {
            char *grown = realloc(text, capacity * 2);
            if (!grown) { free(text); text = NULL; break; }
            text = grown;
            capacity *= 2;
        }
    }
    fclose(file);
    if (text) { text[n] = 0; *length = n; }
    return text;
}

// Copies a string member, cut to fit.
static void copy_string(char *out, size_t size, const struct mainui_json *json, const char *key) {
    const struct mainui_json_member *m = mainui_json_get(json, key);
    out[0] = 0;
    if (!m || m->type != MAINUI_JSON_STRING) return;
    size_t n = strlen(m->text);
    if (n >= size) n = size - 1;
    memcpy(out, m->text, n);
    out[n] = 0;
}

static int get_int(const struct mainui_json *json, const char *key, int fallback) {
    const struct mainui_json_member *m = mainui_json_get(json, key);
    return m && m->type == MAINUI_JSON_NUMBER ? (int)m->number : fallback;
}

// The tab background, LED colour and effect script of a system, app or port (static:
// handleEmuItemList and handleAppItemList): a path that is not absolute is kept under the
// item's folder, cut to the official 128-byte buffer, and the colour counts only as exactly six
// hex digits.
static void backdrop(struct mainui_backdrop *out, const struct mainui_json *json, const char *directory) {
    static const char *const keys[2] = {"background", "effectsh"};
    char *paths[2] = {out->background, out->effectsh};
    char value[512];
    for (int i = 0; i < 2; ++i) {
        copy_string(value, sizeof(value), json, keys[i]);
        paths[i][0] = 0;
        if (value[0] && snprintf(paths[i], sizeof(out->background), "%s%s%s", value[0] == '/' ? "" : directory,
                                 value[0] == '/' ? "" : "/", value) < 0)
            paths[i][0] = 0;
    }
    copy_string(value, sizeof(value), json, "themecolor");
    int valid = strlen(value) == 6;
    for (int i = 0; valid && i < 6; ++i) valid = isxdigit((unsigned char)value[i]) != 0;
    if (valid) memcpy(out->themecolor, value, sizeof(out->themecolor));
    else out->themecolor[0] = 0;
}

// Calls back with each object of a JSON array's text, as its own text.
static int each_object(const char *text, size_t length, int (*callback)(void *, const char *, size_t), void *context) {
    size_t i = 0;
    while (i < length && text[i] != '[') ++i;
    for (++i; i < length; ++i) {
        if (text[i] != '{') continue;
        size_t start = i;
        int depth = 0, in_string = 0;
        for (; i < length; ++i) {
            char c = text[i];
            if (in_string) {
                if (c == '\\') ++i;
                else if (c == '"') in_string = 0;
            } else if (c == '"') {
                in_string = 1;
            } else if (c == '{') {
                ++depth;
            } else if (c == '}' && --depth == 0) {
                break;
            }
        }
        if (i >= length) return -1;
        if (callback(context, text + start, i - start + 1) < 0) return -1;
    }
    return 0;
}

// ---- show.json -----------------------------------------------------------------------------

struct show_list {
    char labels[256][128];
    int shows[256];
    int count;
};

static int add_show(void *context, const char *text, size_t length) {
    static struct mainui_json json;
    struct show_list *list = context;
    if (list->count == 256 || mainui_json_parse(&json, text, length) < 0) return 0;
    copy_string(list->labels[list->count], sizeof(list->labels[0]), &json, "label");
    list->shows[list->count++] = get_int(&json, "show", 1);
    return 0;
}

static void load_show(const struct mainui_fs *fs, const char *path, struct show_list *list) {
    char full[PATH_BYTES];
    size_t length;
    list->count = 0;
    device_path(fs, full, path);
    char *text = read_text(full, &length);
    if (!text) return;
    each_object(text, length, add_show, list);
    free(text);
}

// An entry missing from show.json is shown.
static int shown(const struct show_list *list, const char *label) {
    for (int i = 0; i < list->count; ++i)
        if (strcmp(list->labels[i], label) == 0) return list->shows[i] != 0;
    return 1;
}

// ---- Systems -------------------------------------------------------------------------------

static void join(char *out, size_t size, const char *directory, const char *path) {
    if (!path[0]) out[0] = 0;
    else if (path[0] == '/') cat(out, size, path, "", "", "");
    else cat(out, size, directory, "/", path, "");
}

struct launcher_parse {
    struct mainui_system *system;
};

static int add_launcher(void *context, const char *text, size_t length) {
    static struct mainui_json json;
    struct mainui_system *s = ((struct launcher_parse *)context)->system;
    if (s->launcher_count == MAINUI_LAUNCHERS_MAX || mainui_json_parse(&json, text, length) < 0) return 0;
    char launch[256];
    copy_string(s->launcher_names[s->launcher_count], sizeof(s->launcher_names[0]), &json, "name");
    copy_string(launch, sizeof(launch), &json, "launch");
    if (!launch[0]) return 0;
    join(s->launchers[s->launcher_count], sizeof(s->launchers[0]), s->directory, launch);
    ++s->launcher_count;
    return 0;
}

static int load_system(const struct mainui_fs *fs, const char *root, const char *folder, struct mainui_system *s) {
    static struct mainui_json json;
    char path[PATH_BYTES], full[PATH_BYTES], value[512];
    size_t length;
    memset(s, 0, sizeof(*s));
    if (cat(s->folder, sizeof(s->folder), folder, "", "", "") < 0 ||
        cat(s->directory, sizeof(s->directory), root, "/", folder, "") < 0 ||
        cat(path, sizeof(path), s->directory, "/config.json", "", "") < 0)
        return -1;
    device_path(fs, full, path);
    char *text = read_text(full, &length);
    if (!text) return -1;
    // The official reads the file with cJSON, so a long value, such as a launchlist past 1 KiB,
    // never hides the system; the launchlist is read from the text at any length.
    int parsed = mainui_json_parse_fitting(&json, text, length);
    const char *list;
    size_t list_length;
    if (parsed == 0 && mainui_json_find(text, length, "launchlist", &list, &list_length) == 0 && *list == '[') {
        struct launcher_parse parse = { s };
        each_object(list, list_length, add_launcher, &parse);
    }
    free(text);
    if (parsed < 0) return -1;
    copy_string(s->label, sizeof(s->label), &json, "label");
    if (!s->label[0]) cat(s->label, sizeof(s->label), folder, "", "", "");
    copy_string(value, sizeof(value), &json, "launch");
    join(s->launch, sizeof(s->launch), s->directory, value);
    copy_string(value, sizeof(value), &json, "rompath");
    join(s->rompath, sizeof(s->rompath), s->directory, value);
    copy_string(value, sizeof(value), &json, "imgpath");
    join(s->imgpath, sizeof(s->imgpath), s->directory, value);
    copy_string(value, sizeof(value), &json, "gamelist");
    join(s->gamelist, sizeof(s->gamelist), s->directory, value);
    copy_string(s->extlist, sizeof(s->extlist), &json, "extlist");
    copy_string(s->icontop, sizeof(s->icontop), &json, "icontop");
    copy_string(s->icon, sizeof(s->icon), &json, "icon");
    // A system with only "icon" (PORTS) shows that picture on the grid (n06).
    if (!s->icontop[0]) {
        copy_string(s->icontop, sizeof(s->icontop), &json, "icon");
        s->icon_only = s->icontop[0] != 0;
    }
    backdrop(&s->backdrop, &json, s->directory);
    s->useswap = get_int(&json, "useswap", 0);
    s->shortname = get_int(&json, "shortname", 0);
    s->hidebios = get_int(&json, "hidebios", 0);
    s->show = 1;
    return 0;
}

static int compare_systems(const void *a, const void *b) {
    const struct mainui_system *x = a, *y = b;
    int by_label = strcmp(x->label, y->label);
    return by_label ? by_label : strcmp(x->folder, y->folder);
}

static int list_names(const struct mainui_fs *fs, const char *path, char (**names)[256]) {
    char full[PATH_BYTES];
    device_path(fs, full, path);
    int capacity = NAMES_INITIAL;
    *names = malloc(sizeof(**names) * (size_t)capacity);
    if (!*names) return -1;
    for (;;) {
        errno = 0;
        int n = (fs->list ? fs->list : mainui_readdir)(fs->context, full, *names, capacity);
        if (n < 0) break;
        if (n < capacity) {
            // Keep only actual rows while a recursive walk retains ancestors.
            if (!n) { free(*names); *names = NULL; }
            else if (n < capacity) {
                void *smaller = realloc(*names, (size_t)n * sizeof(**names));
                if (smaller) *names = smaller;
            }
            return n;
        }
        // A full bounded read may have more rows. A provider may instead report
        // its total count while copying only capacity rows; neither is complete.
        if (capacity == INT_MAX || n == INT_MAX) { errno = EOVERFLOW; break; }
        int next = capacity > INT_MAX / 2 ? INT_MAX : capacity * 2;
        if (n > capacity && next <= n) next = n + 1;
        if ((size_t)next > SIZE_MAX / sizeof(**names)) { errno = EOVERFLOW; break; }
        void *grown = realloc(*names, (size_t)next * sizeof(**names));
        if (!grown) break;
        *names = grown; capacity = next;
    }
    int failure = errno;
    free(*names); *names = NULL; errno = failure;
    return -1;
}

int mainui_catalog_systems(const struct mainui_fs *fs, struct mainui_system **systems) {
    char (*names)[256];
    *systems = NULL;
    int n = list_names(fs, "/mnt/SDCARD/Emus", &names);
    if (n < 0) return -1;
    struct mainui_system *out = calloc((size_t)(n ? n : 1), sizeof(*out));
    if (!out) { free(names); return -1; }
    static struct show_list show;
    load_show(fs, "/mnt/SDCARD/Emus/show.json", &show);
    int count = 0;
    for (int i = 0; i < n; ++i) {
        if (names[i][0] == '.') continue;
        if (load_system(fs, "/mnt/SDCARD/Emus", names[i], &out[count]) < 0) continue;
        out[count].show = shown(&show, out[count].label);
        ++count;
    }
    free(names);
    qsort(out, (size_t)count, sizeof(*out), compare_systems);
    *systems = out;
    return count;
}

// ---- Games ---------------------------------------------------------------------------------

int mainui_catalog_bests(const struct mainui_fs *fs, struct mainui_system **systems) {
    static const char *const roots[] = {"/mnt/SDCARD/Best", "/mnt/exUDISK/Best"};
    static struct show_list show;
    *systems = NULL;
    int count = 0, capacity = 0;
    for (unsigned r = 0; r < sizeof(roots) / sizeof(roots[0]); ++r) {
        char (*names)[256], path[PATH_BYTES];
        cat(path, sizeof(path), roots[r], "/show.json", "", "");
        load_show(fs, path, &show);
        int n = list_names(fs, roots[r], &names);
        if (n < 0) continue;
        for (int i = 0; i < n; ++i) {
            if (names[i][0] == '.') continue;
            if (count == capacity) {
                capacity = capacity ? capacity * 2 : 16;
                struct mainui_system *grown = realloc(*systems, sizeof(**systems) * (size_t)capacity);
                if (!grown) { free(names); free(*systems); *systems = NULL; return -1; }
                *systems = grown;
            }
            struct mainui_system item;
            if (load_system(fs, roots[r], names[i], &item) < 0) continue;
            item.show = shown(&show, item.label);
            // handleEmuItemList replaces the previous entry with this exact launcher,
            // then inserts after existing equal labels (SD first, exUDISK second).
            for (int j = 0; j < count; ++j) {
                if (strcmp((*systems)[j].launch, item.launch)) continue;
                memmove(&(*systems)[j], &(*systems)[j + 1],
                        (size_t)(count - j - 1) * sizeof(item));
                --count;
                break;
            }
            int at = 0;
            while (at < count && strcmp((*systems)[at].label, item.label) <= 0) ++at;
            memmove(&(*systems)[at + 1], &(*systems)[at], (size_t)(count - at) * sizeof(item));
            (*systems)[at] = item;
            ++count;
        }
        free(names);
    }
    return count;
}

// <anything>_cache<digits>.db, the official list caches.
static int cache_file(const char *name) {
    const char *mark = strstr(name, "_cache");
    if (!mark) return 0;
    const char *p = mark + 6;
    if (!isdigit((unsigned char)*p)) return 0;
    while (isdigit((unsigned char)*p)) ++p;
    return strcmp(p, ".db") == 0;
}

static int clear_files(const struct mainui_fs *fs, const char *rompath, int kind) {
    char full[4096], actual[4096], mount[4096], card[4096];
    if (!fs || !rompath || !rompath[0] || snprintf(full, sizeof(full), "%s%s", fs->root ? fs->root : "", rompath) >= (int)sizeof(full)) return -1;
    if (!realpath(full, actual)) return errno == ENOENT || errno == ENOTDIR ? 0 : -1;
    static const char *const roots[] = {"/mnt/SDCARD", "/mnt/exUDISK", "/mnt/P2EXT"};
    int inside = 0;
    for (unsigned i = 0; i < sizeof(roots) / sizeof(roots[0]); ++i) {
        if (snprintf(mount, sizeof(mount), "%s%s", fs->root ? fs->root : "", roots[i]) >= (int)sizeof(mount) || !realpath(mount, card)) continue;
        size_t length = strlen(card);
        if (!strncmp(actual, card, length) && (actual[length] == '/' || !actual[length])) inside = 1;
    }
    if (!inside) return -1;
    DIR *directory = opendir(actual);
    if (!directory) return errno == ENOENT || errno == ENOTDIR ? 0 : -1;
    int count = 0, failed = 0;
    struct dirent *entry;
    while ((entry = readdir(directory))) {
        size_t length = strlen(entry->d_name);
        int matches = kind == 0 ? cache_file(entry->d_name) :
                      kind == 1 ? length >= 4 && !strcmp(entry->d_name + length - 4, ".cfg") :
                                  !strcmp(entry->d_name, "fceux.cfg");
        if (entry->d_name[0] == '.' || !matches) continue;
        struct stat st;
        if (fstatat(dirfd(directory), entry->d_name, &st, AT_SYMLINK_NOFOLLOW) != 0) { if (errno != ENOENT) failed = 1; continue; }
        if (S_ISDIR(st.st_mode)) continue;
        if (unlinkat(dirfd(directory), entry->d_name, 0) == 0) ++count;
        else if (errno != ENOENT) failed = 1;
    }
    closedir(directory);
    return failed ? -1 : count;
}

int mainui_catalog_clear_caches(const struct mainui_fs *fs, const char *rompath) {
    return clear_files(fs, rompath, 0);
}

int mainui_catalog_clear_keymaps(const struct mainui_fs *fs) {
    int count = 0, failed = 0;
    static const char *const folders[] = {
        "/mnt/SDCARD/Roms/ARCADE//.mame4all/configs",
        "/mnt/SDCARD/Roms/FC//.fceux",
        "/mnt/SDCARD/Roms/FC//.fceux/cfg"
    };
    for (unsigned i = 0; i < sizeof(folders) / sizeof(folders[0]); ++i) {
        int result = clear_files(fs, folders[i], i == 1 ? 2 : 1);
        if (result < 0) failed = 1;
        else count += result;
    }
    return failed ? -1 : count;
}

static int extension_listed(const char *extlist, const char *name) {
    if (!extlist[0]) return 1;
    const char *dot = strrchr(name, '.');
    if (!dot || !dot[1]) return 0;
    const char *extension = dot + 1;
    size_t length = strlen(extension);
    for (const char *p = extlist; *p;) {
        const char *end = strchr(p, '|');
        size_t n = end ? (size_t)(end - p) : strlen(p);
        if (n == length && strncasecmp(p, extension, n) == 0) return 1;
        if (!end) break;
        p = end + 1;
    }
    return 0;
}

static int listable_file(const char *extlist, const char *name) {
    return name[0] != '.' && !cache_file(name) && extension_listed(extlist, name);
}

// A folder without a listable descendant is omitted. Ancestor identities stop
// symlink cycles without hiding valid games merely because they are deeply nested.
struct folder_ancestor { dev_t device; ino_t inode; const struct folder_ancestor *parent; };
static int folder_has_games(const struct mainui_fs *fs, const char *path, const char *extlist,
                            const struct folder_ancestor *parent) {
    char (*names)[256];
    char resolved[PATH_BYTES]; struct stat directory;
    if (strlen(path) >= sizeof(((struct mainui_game *)0)->path)) return 0;
    device_path(fs, resolved, path);
    if (stat(resolved, &directory) || !S_ISDIR(directory.st_mode)) return 0;
    for (const struct folder_ancestor *p = parent; p; p = p->parent)
        if (p->device == directory.st_dev && p->inode == directory.st_ino) return 0;
    struct folder_ancestor current = {directory.st_dev, directory.st_ino, parent};
    int n = list_names(fs, path, &names);
    if (n < 0) return errno == ENOMEM || errno == EOVERFLOW ? -1 : 0;
    int found = 0;
    for (int i = 0; i < n && !found; ++i) {
        char child[PATH_BYTES], full[PATH_BYTES];
        struct stat st;
        if (names[i][0] == '.' || cat(child, sizeof(child), path, "/", names[i], "") < 0) continue;
        device_path(fs, full, child);
        if (stat(full, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) found = folder_has_games(fs, child, extlist, &current);
        else found = S_ISREG(st.st_mode) && listable_file(extlist, names[i]);
    }
    free(names);
    return found;
}

static char pinyin_initial(unsigned int cp) {
    if (cp < 128) return (char)cp;
    if (cp >= 0x3400 && cp < 0xa000) return mainui_pinyin_bmp[cp - 0x3400];
    size_t lo = 0, hi = sizeof(mainui_pinyin_extra) / sizeof(mainui_pinyin_extra[0]);
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (mainui_pinyin_extra[mid].codepoint < cp) lo = mid + 1;
        else hi = mid;
    }
    if (lo < sizeof(mainui_pinyin_extra) / sizeof(mainui_pinyin_extra[0]) &&
        mainui_pinyin_extra[lo].codepoint == cp) return mainui_pinyin_extra[lo].initial;
    return ' ';
}

int mainui_catalog_pinyin(const char *text, char *out, size_t capacity) {
    if (!out || !capacity) return -1;
    out[0] = 0;
    if (!text) return -1;
    const unsigned char *p = (const unsigned char *)text;
    size_t used = 0;
    while (*p) {
        unsigned int cp = *p, minimum = 0;
        int length = 1;
        if (*p >= 0xc2 && *p <= 0xdf) { cp &= 0x1f; length = 2; minimum = 0x80; }
        else if (*p >= 0xe0 && *p <= 0xef) { cp &= 0x0f; length = 3; minimum = 0x800; }
        else if (*p >= 0xf0 && *p <= 0xf4) { cp &= 7; length = 4; minimum = 0x10000; }
        else if (*p >= 0x80) goto invalid;
        for (int i = 1; i < length; ++i) {
            if (p[i] < 0x80 || p[i] > 0xbf) goto invalid;
            cp = (cp << 6) | (p[i] & 0x3f);
        }
        if (cp < minimum || cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff) ||
            used + 1 >= capacity) goto invalid;
        out[used++] = pinyin_initial(cp);
        out[used] = 0;
        p += length;
    }
    return (int)used;
invalid:
    out[0] = 0;
    return -1;
}

static int compare_games(const void *a, const void *b) {
    const struct mainui_game *x = a, *y = b;
    if (x->folder != y->folder) return y->folder - x->folder;
    int by_initials = strcasecmp(x->pinyin, y->pinyin);
    if (by_initials) return by_initials;
    int by_label = strcmp(x->label, y->label);
    return by_label ? by_label : strcmp(x->name, y->name);
}

static void filesystem_game(const struct mainui_system *system, const struct mainui_names *dictionary,
                            const char *language, const char *name, struct mainui_game *g) {
    cat(g->name, sizeof(g->name), name, "", "", "");
    cat(g->label, sizeof(g->label), name, "", "", "");
    char *dot = strrchr(g->label, '.');
    if (!g->folder && dot && dot != g->label) *dot = 0;
    mainui_catalog_pinyin(g->label, g->pinyin, sizeof(g->pinyin));
    memcpy(g->opinyin, g->pinyin, sizeof(g->opinyin));
    if (!g->folder && system->shortname) {
        const char *display = mainui_names_get(dictionary, language, g->name);
        if (display && display[0]) cat(g->display, sizeof(g->display), display, "", "", "");
        const char *initial_language = language && !strcmp(language, "ch.lang") ? "ch.lang" : "en.lang";
        const char *translated = mainui_names_get(dictionary, initial_language, g->name);
        if (translated && translated[0]) mainui_catalog_pinyin(translated, g->pinyin, sizeof(g->pinyin));
    }
}

int mainui_catalog_games_at(const struct mainui_fs *fs, const struct mainui_system *system,
                           const struct mainui_names *dictionary, const char *language,
                           const char *directory, struct mainui_game **games) {
    char (*names)[256];
    *games = NULL;
    int child = directory && directory[0];
    const char *rompath = child ? directory : system->rompath;
    int xml_count = child ? 0 : mainui_catalog_xml(fs, system, dictionary, language, games);
    if (xml_count > 0) {
        qsort(*games, (size_t)xml_count, sizeof(**games), compare_games);
        return xml_count;
    }
    int n = list_names(fs, rompath, &names);
    if (n < 0) return -1;
    struct mainui_game *out = calloc((size_t)(n ? n : 1), sizeof(*out));
    if (!out) { free(names); return -1; }
    int count = 0;
    for (int i = 0; i < n; ++i) {
        const char *name = names[i];
        if (name[0] == '.' || cache_file(name)) continue;
        char path[PATH_BYTES], full[PATH_BYTES];
        struct stat st;
        if (cat(path, sizeof(path), rompath, "/", name, "") < 0) continue;
        device_path(fs, full, path);
        if (stat(full, &st) != 0) continue;
        struct mainui_game *g = &out[count];
        if (S_ISDIR(st.st_mode)) {
            int found = folder_has_games(fs, path, system->extlist, NULL);
            if (found < 0) { free(names); free(out); return -1; }
            if (!found) continue;
            g->folder = 1;
        } else if (!S_ISREG(st.st_mode) || !extension_listed(system->extlist, name)) {
            continue;
        }
        if (child && cat(g->path, sizeof(g->path), path, "", "", "") < 0) continue;
        filesystem_game(system, dictionary, language, name, g);
        ++count;
    }
    free(names);
    qsort(out, (size_t)count, sizeof(*out), compare_games);
    *games = out;
    return count;
}

struct game_walk {
    const struct mainui_fs *fs;
    const struct mainui_system *system;
    const struct mainui_names *names;
    const char *language;
    const char *root;
    int (*visit)(void *, const struct mainui_game *);
    void *context;
};

static int walk_directory(const struct game_walk *walk, const char *path, const struct folder_ancestor *parent) {
    char full[PATH_BYTES]; struct stat directory;
    device_path(walk->fs, full, path);
    if (stat(full, &directory) || !S_ISDIR(directory.st_mode)) return 0;
    for (const struct folder_ancestor *p = parent; p; p = p->parent)
        if (p->device == directory.st_dev && p->inode == directory.st_ino) return 0;
    char *resolved = realpath(full, NULL);
    size_t base = strlen(walk->root);
    int inside = resolved && !strncmp(resolved, walk->root, base) &&
        (!resolved[base] || resolved[base] == '/' || (base == 1 && walk->root[0] == '/'));
    free(resolved);
    if (!inside) return 0;
    struct folder_ancestor current = {directory.st_dev, directory.st_ino, parent};
    char (*names)[256];
    int n = list_names(walk->fs, path, &names);
    if (n < 0) return errno == ENOMEM || errno == EOVERFLOW ? -1 : 0;
    int result = 0;
    for (int pass = 0; pass < 2 && !result; ++pass) for (int i = 0; i < n && !result; ++i) {
        char child[PATH_BYTES]; struct stat st;
        if (names[i][0] == '.' || cache_file(names[i]) || cat(child, sizeof(child), path, "/", names[i], "") < 0) continue;
        device_path(walk->fs, full, child);
        if (stat(full, &st)) continue;
        // Root files precede descendants when original labels compare equal.
        if (pass == 0 && S_ISREG(st.st_mode) && extension_listed(walk->system->extlist, names[i])) {
            struct mainui_game game = {0};
            filesystem_game(walk->system, walk->names, walk->language, names[i], &game);
            if (parent) memcpy(game.path, child, strlen(child) + 1);
            result = walk->visit(walk->context, &game);
        } else if (pass == 1 && S_ISDIR(st.st_mode)) result = walk_directory(walk, child, &current);
    }
    free(names);
    return result;
}

int mainui_catalog_walk_games(const struct mainui_fs *fs, const struct mainui_system *system,
                             const struct mainui_names *names, const char *language,
                             int (*visit)(void *, const struct mainui_game *), void *context) {
    if (!visit || !fs || !system) return -1;
    struct mainui_game *xml = NULL;
    int n = mainui_catalog_xml(fs, system, names, language, &xml);
    if (n > 0) {
        qsort(xml, (size_t)n, sizeof(*xml), compare_games);
        int result = 0;
        for (int i = 0; i < n && !result; ++i) if (!xml[i].folder) result = visit(context, &xml[i]);
        free(xml);
        return result;
    }
    free(xml);
    char full[PATH_BYTES]; device_path(fs, full, system->rompath);
    char *root = realpath(full, NULL);
    if (!root) return 0;
    struct game_walk walk = {fs, system, names, language, root, visit, context};
    int result = walk_directory(&walk, system->rompath, NULL);
    free(root);
    return result;
}

int mainui_catalog_games_named(const struct mainui_fs *fs, const struct mainui_system *system,
                              const struct mainui_names *dictionary, const char *language, struct mainui_game **games) {
    return mainui_catalog_games_at(fs, system, dictionary, language, NULL, games);
}

int mainui_catalog_games(const struct mainui_fs *fs, const struct mainui_system *system, struct mainui_game **games) {
    return mainui_catalog_games_named(fs, system, NULL, "en.lang", games);
}

const char *mainui_catalog_game_label(const struct mainui_game *game) {
    return game->display[0] ? game->display : game->label;
}

static int letter_group(const struct mainui_game *game) {
    if (game->folder) return 0;
    char fallback[256];
    const char *initials = game->pinyin;
    if (!initials[0]) {
        if (mainui_catalog_pinyin(game->label, fallback, sizeof(fallback)) < 0) return -1;
        initials = fallback;
    }
    unsigned char c = (unsigned char)initials[0];
    if (!c || c >= 128) return -1;
    return c >= 'a' && c <= 'z' ? c - 'a' + 'A' : c;
}

int mainui_catalog_letter_jump(const struct mainui_game *games, int count, int focus, int direction, char *letter) {
    if (!games || !letter || count <= 0 || focus < 0 || focus >= count ||
        (direction != -1 && direction != 1)) return -1;
    // The official derives initials from the sorted pinyin/cpinyin columns, not UTF-8 bytes.
    // Invalid UTF-8 cannot establish a group; valid unsupported codepoints use a space.
    for (int i = 0; i < count; ++i) if (letter_group(&games[i]) < 0) return -1;
    int current = letter_group(&games[focus]), next = focus;
    if (direction > 0) {
        while (next < count && letter_group(&games[next]) == current) ++next;
        if (next == count) return -1;
    } else {
        while (next >= 0 && letter_group(&games[next]) == current) --next;
        if (next < 0) return -1;
        int previous = letter_group(&games[next]);
        while (next > 0 && letter_group(&games[next - 1]) == previous) --next;
    }
    *letter = (char)letter_group(&games[next]);
    return next;
}

void mainui_catalog_game_paths(const struct mainui_system *system, const struct mainui_game *game,
                               char *rompath, size_t rompath_size, char *imgpath, size_t imgpath_size) {
    if (game->path[0]) cat(rompath, rompath_size, game->path, "", "", "");
    else cat(rompath, rompath_size, system->rompath, "/", game->name, "");
    if (game->image[0]) cat(imgpath, imgpath_size, game->image, "", "", "");
    else cat(imgpath, imgpath_size, system->imgpath, "/", game->label, ".png");
}

int mainui_catalog_has_cover(const struct mainui_fs *fs, const char *imgpath) {
    char full[PATH_BYTES];
    struct stat st;
    device_path(fs, full, imgpath);
    return stat(full, &st) == 0 && S_ISREG(st.st_mode);
}

// ---- Apps ----------------------------------------------------------------------------------

static int load_app(const struct mainui_fs *fs, const char *directory, struct mainui_app *app) {
    static struct mainui_json json;
    char path[PATH_BYTES], full[PATH_BYTES];
    size_t length;
    memset(app, 0, sizeof(*app));
    if (cat(app->directory, sizeof(app->directory), directory, "", "", "") < 0 ||
        cat(path, sizeof(path), directory, "/config.json", "", "") < 0)
        return -1;
    device_path(fs, full, path);
    char *text = read_text(full, &length);
    if (!text) return -1;
    int parsed = mainui_json_parse_fitting(&json, text, length);
    free(text);
    if (parsed < 0) return -1;
    copy_string(app->label, sizeof(app->label), &json, "label");
    copy_string(app->launch, sizeof(app->launch), &json, "launch");
    copy_string(app->description, sizeof(app->description), &json, "description");
    copy_string(app->icon, sizeof(app->icon), &json, "icontop");
    if (app->icon[0]) copy_string(app->icon_layer, sizeof(app->icon_layer), &json, "icon");
    if (!app->icon[0]) {
        copy_string(app->icon, sizeof(app->icon), &json, "icon");
        app->icon_only = app->icon[0] != 0;
    }
    backdrop(&app->backdrop, &json, app->directory);
    return app->label[0] ? 0 : -1;
}

int mainui_catalog_apps(const struct mainui_fs *fs, struct mainui_app **apps) {
    static const char *const roots[2] = { "/usr/trimui/apps", "/mnt/SDCARD/Apps" };
    static struct show_list show;
    static struct show_list stock_override;
    load_show(fs, MAINUI_STOCK_APPS_SHOW_FILE, &stock_override);
    *apps = NULL;
    struct mainui_app *out = NULL;
    int count = 0, capacity = 0;
    for (int r = 0; r < 2; ++r) {
        char show_path[PATH_BYTES];
        if (cat(show_path, sizeof(show_path), roots[r], "/show.json", "", "") < 0) continue;
        load_show(fs, show_path, &show);
        char (*names)[256];
        int n = list_names(fs, roots[r], &names);
        if (n < 0) continue;
        for (int i = 0; i < n; ++i) {
            if (names[i][0] == '.') continue;
            if (count == capacity) {
                capacity = capacity ? capacity * 2 : 32;
                struct mainui_app *grown = realloc(out, sizeof(*out) * (size_t)capacity);
                if (!grown) { free(names); free(out); return -1; }
                out = grown;
            }
            char directory[PATH_BYTES];
            if (cat(directory, sizeof(directory), roots[r], "/", names[i], "") < 0 ||
                load_app(fs, directory, &out[count]) < 0)
                continue;
            out[count].stock = r == 0;
            out[count].show = shown(&show, out[count].label);
            if (r == 0) for (int s = 0; s < stock_override.count; ++s)
                if (!strcmp(stock_override.labels[s], out[count].label)) {
                    out[count].show = stock_override.shows[s] != 0;
                    break;
                }
            ++count;
        }
        free(names);
    }
    *apps = out;
    return count;
}

int mainui_catalog_ports(const struct mainui_fs *fs, struct mainui_app **ports) {
    static const char *const roots[2] = { "/mnt/SDCARD/Ports", "/mnt/exUDISK/Ports" };
    static struct show_list show;
    *ports = NULL;
    struct mainui_app *out = NULL;
    int count = 0, capacity = 0;
    for (int r = 0; r < 2; ++r) {
        char path[PATH_BYTES], (*names)[256];
        if (cat(path, sizeof(path), roots[r], "/show.json", "", "") < 0) continue;
        load_show(fs, path, &show);
        int n = list_names(fs, roots[r], &names);
        if (n < 0) continue;
        for (int i = 0; i < n; ++i) {
            if (names[i][0] == '.') continue;
            if (count == capacity) {
                capacity = capacity ? capacity * 2 : 32;
                struct mainui_app *grown = realloc(out, sizeof(*out) * (size_t)capacity);
                if (!grown) { free(names); free(out); return -1; }
                out = grown;
            }
            char directory[PATH_BYTES];
            if (cat(directory, sizeof(directory), roots[r], "/", names[i], "") < 0 ||
                load_app(fs, directory, &out[count]) < 0)
                continue;
            out[count].show = shown(&show, out[count].label);
            ++count;
        }
        free(names);
    }
    *ports = out;
    return count;
}

// ---- Themes --------------------------------------------------------------------------------

int mainui_catalog_themes(const struct mainui_fs *fs, struct mainui_theme **themes) {
    static struct mainui_json json;
    char (*names)[256];
    *themes = NULL;
    // The firmware's internal Themes folder, when it exists, replaces the card's (static: main
    // loads one or the other).
    char base[PATH_BYTES], folder[PATH_BYTES];
    device_path(fs, base, "/mnt/UDISK/Themes");
    const char *themes_dir = access(base, F_OK) == 0 ? "/mnt/UDISK/Themes/" : "/mnt/SDCARD/Themes/";
    cat(folder, sizeof(folder), themes_dir, "", "", "");
    folder[strlen(folder) - 1] = 0;
    int n = list_names(fs, folder, &names);
    struct mainui_theme *out = calloc((size_t)(n > 0 ? n + 1 : 1), sizeof(*out));
    if (!out) { free(names); return -1; }
    cat(out[0].name, sizeof(out[0].name), "Default", "", "", "");
    cat(out[0].path, sizeof(out[0].path), "../res/", "", "", "");
    int count = 1;
    for (int i = 0; i < n; ++i) {
        char path[PATH_BYTES], full[PATH_BYTES];
        size_t length;
        if (names[i][0] == '.' || cat(path, sizeof(path), themes_dir, names[i], "/config.json", "") < 0) continue;
        device_path(fs, full, path);
        char *text = read_text(full, &length);
        if (!text) continue;
        int parsed = mainui_json_parse_fitting(&json, text, length);
        free(text);
        if (parsed < 0) continue;
        struct mainui_theme *t = &out[count];
        copy_string(t->name, sizeof(t->name), &json, "name");
        if (!t->name[0]) cat(t->name, sizeof(t->name), names[i], "", "", "");
        copy_string(t->font, sizeof(t->font), &json, "font");
        if (cat(t->path, sizeof(t->path), themes_dir, names[i], "/", "") < 0) continue;
        const struct mainui_json_member *preview = mainui_json_get(&json, "preview");
        // Stock stores a trailing slash in the theme folder, then showPreview adds
        // another slash for a relative preview. Preserve those producer bytes.
        if (preview && preview->type == MAINUI_JSON_STRING && preview->text[0] &&
            cat(t->preview, sizeof(t->preview), preview->text[0] == '/' ? "" : t->path,
                preview->text[0] == '/' ? "" : "/", preview->text, "") < 0) t->preview[0] = 0;
        ++count;
    }
    if (n >= 0) free(names);
    *themes = out;
    return count;
}
