#define _POSIX_C_SOURCE 200809L
#include "assets.h"
#include "json.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#define PATH_BYTES 1024

// Joins up to three strings; returns -1, leaving out empty, when they do not fit.
static int cat(char *out, size_t size, const char *a, const char *b, const char *c) {
    const char *parts[3] = {a, b, c};
    size_t n = 0;
    for (int i = 0; i < 3; ++i) {
        size_t length = strlen(parts[i]);
        if (n + length >= size) { out[0] = 0; return -1; }
        memcpy(out + n, parts[i], length);
        n += length;
    }
    out[n] = 0;
    return 0;
}

static int is_file(const struct mainui_fs *fs, const char *path) {
    char full[PATH_BYTES];
    struct stat st;
    return cat(full, sizeof(full), fs->root ? fs->root : "", path, "") == 0 && stat(full, &st) == 0 && S_ISREG(st.st_mode);
}

int mainui_asset_theme_dir(const char *theme, char *out, size_t size) {
    // "../res/" and an empty value are the Default theme, relative to /usr/trimui/osd.
    if (!theme || !theme[0] || strcmp(theme, "../res/") == 0 || strcmp(theme, "../res") == 0)
        return cat(out, size, MAINUI_DEFAULT_THEME_DIR, "", "");
    if (theme[0] != '/') return cat(out, size, "/usr/trimui/osd/", theme, theme[strlen(theme) - 1] == '/' ? "" : "/");
    return cat(out, size, theme, theme[strlen(theme) - 1] == '/' ? "" : "/", "");
}

int mainui_asset_find(const struct mainui_fs *fs, const char *theme, const char *relative, char *out, size_t size) {
    char directory[PATH_BYTES];
    if (mainui_asset_theme_dir(theme, directory, sizeof(directory)) == 0 &&
        cat(out, size, directory, relative, "") == 0 && is_file(fs, out))
        return strcmp(directory, MAINUI_DEFAULT_THEME_DIR) != 0;
    if (cat(out, size, MAINUI_DEFAULT_THEME_DIR, relative, "") == 0 && is_file(fs, out)) return 0;
    out[0] = 0;
    return -1;
}

int mainui_asset_language(const struct mainui_fs *fs, const char *language, char *out, size_t size) {
    if (language && language[0] && !strchr(language, '/') &&
        cat(out, size, MAINUI_DEFAULT_THEME_DIR "lang/", language, "") == 0 && is_file(fs, out))
        return 1;
    if (cat(out, size, MAINUI_DEFAULT_THEME_DIR "lang/", "en.lang", "") == 0 && is_file(fs, out)) return 0;
    out[0] = 0;
    return -1;
}

static int regular_language(const char *language) {
    // static: init_lang_font; simplified Chinese draws with regular.ttf too (round 8).
    static const char *const regular[] = { "en.lang", "ch.lang", "es.lang", "pt.lang", "it.lang", "fr.lang", "de.lang" };
    if (!language || !language[0]) return 1;
    for (size_t i = 0; i < sizeof(regular) / sizeof(regular[0]); ++i)
        if (strcmp(language, regular[i]) == 0) return 1;
    return 0;
}

int mainui_asset_font(const struct mainui_fs *fs, const char *theme, const char *theme_font, const char *language,
                      char *out, size_t size) {
    char directory[PATH_BYTES];
    if (theme_font && theme_font[0] && !strchr(theme_font, '/') &&
        mainui_asset_theme_dir(theme, directory, sizeof(directory)) == 0 &&
        strcmp(directory, MAINUI_DEFAULT_THEME_DIR) != 0 &&
        cat(out, size, directory, theme_font, "") == 0 && is_file(fs, out))
        return 1;
    const char *font = regular_language(language) ? "regular.ttf" : "full.ttf";
    if (cat(out, size, MAINUI_DEFAULT_THEME_DIR, font, "") == 0 && is_file(fs, out)) return 0;
    out[0] = 0;
    return -1;
}

// ---- Language strings ----------------------------------------------------------------------

static int keep_string(void *context, const struct mainui_json_member *m) {
    struct mainui_lang *lang = context;
    if (m->type != MAINUI_JSON_STRING) return 0;
    if (strcmp(m->key, "lang") == 0) { cat(lang->name, sizeof(lang->name), m->text, "", ""); return 0; }
    if (strcmp(m->key, "icon") == 0) { cat(lang->icon, sizeof(lang->icon), m->text, "", ""); return 0; }
    char *end = NULL;
    long index = strtol(m->key, &end, 10);
    if (end == m->key || *end || index < 0 || index >= MAINUI_LANG_STRINGS) return 0;
    char *copy = malloc(m->text_length + 1);
    if (!copy) return -1;
    memcpy(copy, m->text, m->text_length + 1);
    if (lang->strings[index]) free(lang->strings[index]);
    else ++lang->count;
    lang->strings[index] = copy;
    return 0;
}

int mainui_lang_load(struct mainui_lang *lang, const char *path) {
    memset(lang, 0, sizeof(*lang));
    FILE *file = fopen(path, "rb");
    if (!file) return -1;
    size_t capacity = 16384, n = 0;
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
    if (!text) return -1;
    int result = mainui_json_each(text, n, keep_string, lang);
    free(text);
    if (result < 0) { mainui_lang_free(lang); return -1; }
    return lang->count;
}

const char *mainui_lang_get(const struct mainui_lang *lang, int index) {
    return index >= 0 && index < MAINUI_LANG_STRINGS ? lang->strings[index] : NULL;
}

void mainui_lang_free(struct mainui_lang *lang) {
    for (int i = 0; i < MAINUI_LANG_STRINGS; ++i) free(lang->strings[i]);
    memset(lang, 0, sizeof(*lang));
}

static int compare_names(const void *a, const void *b) { return strcmp(a, b); }

int mainui_asset_languages(const struct mainui_fs *fs, char names[][32], int max) {
    char directory[PATH_BYTES];
    char (*entries)[256] = malloc(sizeof(*entries) * 256);
    if (!entries || max <= 0 || cat(directory, sizeof(directory), fs->root ? fs->root : "", MAINUI_DEFAULT_THEME_DIR "lang", "") < 0) {
        free(entries);
        return 0;
    }
    int n = (fs->list ? fs->list : mainui_readdir)(fs->context, directory, entries, 256);
    int count = 0, english = 0;
    for (int i = 0; i < n; ++i) {
        size_t length = strlen(entries[i]);
        if (length < 6 || length >= 32 || strcmp(entries[i] + length - 5, ".lang") != 0) continue;
        if (strcmp(entries[i], "en.lang") == 0) { english = 1; continue; }
        if (count < max) memcpy(names[count++], entries[i], length + 1);
    }
    free(entries);
    qsort(names, (size_t)count, sizeof(names[0]), compare_names);
    if (english) {
        if (count == max) --count;
        memmove(names[1], names[0], sizeof(names[0]) * (size_t)count);
        memcpy(names[0], "en.lang", 8);
        ++count;
    }
    return count;
}
