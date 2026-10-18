#ifndef BRICKOS_MAINUI_ASSETS_H
#define BRICKOS_MAINUI_ASSETS_H

#include "catalog.h"

#include <stddef.h>

// Where the frontend finds the installed theme, language, font, and sound files; nothing is
// copied into BrickOS. On the reference device:
// - The Default theme, "../res/" in system.json, is relative to MainUI's working directory
//   /usr/trimui/osd: /usr/trimui/res/, with skin/, sound/ (click.wav, bgm.mp3), lang/, and the
//   fonts regular.ttf and full.ttf.
// - A theme is /mnt/SDCARD/Themes/<name>/ with skin/, sound/, a font named by its config.json,
//   and config.json. A theme file that is missing falls back to the Default theme's file (the
//   official "%s/%s open fail, use %s/%s").
// - A language is /usr/trimui/res/lang/<code>.lang, a JSON object of numbered strings; a
//   missing one falls back to en.lang. English uses regular.ttf (seen open in the official
//   MainUI).

#define MAINUI_DEFAULT_THEME_DIR "/usr/trimui/res/"
#define MAINUI_LANG_STRINGS 512

// The theme folder, with a final slash, for a system.json theme value; returns 0 or -1.
int mainui_asset_theme_dir(const char *theme, char *out, size_t size);

// Finds theme/relative, else the Default theme's file. Returns 1 when the theme's own file was
// found, 0 for the Default's, -1 when neither exists (out is then empty).
int mainui_asset_find(const struct mainui_fs *fs, const char *theme, const char *relative, char *out, size_t size);

// The language file for a system.json language value, else en.lang. Returns 1 for the file
// itself, 0 for the English fallback, -1 when neither exists.
int mainui_asset_language(const struct mainui_fs *fs, const char *language, char *out, size_t size);

// The font: the theme's config font when that file exists, else the Default font for the
// language: regular.ttf for English and the languages the official lists with it (simplified
// Chinese, es, pt, it, fr, de; static: init_lang_font), full.ttf for the others. Returns 1 for
// the theme's font, 0 for a Default font, -1 when none exists.
int mainui_asset_font(const struct mainui_fs *fs, const char *theme, const char *theme_font, const char *language,
                      char *out, size_t size);

// The Language screen's entries: en.lang first, then the other lang/*.lang files by name (e13b:
// English, 简体中文, 繁體中文, Deutsch, Español, ...). Returns the count.
int mainui_asset_languages(const struct mainui_fs *fs, char names[][32], int max);

struct mainui_lang {
    char *strings[MAINUI_LANG_STRINGS];  // by number; NULL when absent
    char name[64];                        // "lang"
    char icon[64];                        // "icon"
    int count;
};

// Loads a .lang file; returns the number of strings, or -1.
int mainui_lang_load(struct mainui_lang *lang, const char *path);
const char *mainui_lang_get(const struct mainui_lang *lang, int index);
void mainui_lang_free(struct mainui_lang *lang);

#endif
