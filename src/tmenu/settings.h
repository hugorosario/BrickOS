#ifndef BRICKOS_TMENU_SETTINGS_H
#define BRICKOS_TMENU_SETTINGS_H

#include <stddef.h>

// What the official library derives from /mnt/UDISK/system.json at every menu open,
// in the order it applies it (docs/tmenu-abi.md, "Start").
#define TMENU_KEYSHM_VALUES 23
#define TMENU_KEYSHM_BCONFIRM 22
#define TMENU_KEYSHM_VOLUME 0

enum tmenu_step_kind {
    TMENU_STEP_KEYSHM,    // write value at index
    TMENU_STEP_LANGUAGE,  // setSystemLanguage(language)
    TMENU_STEP_THEME,     // access(theme)
    TMENU_STEP_COLOR,     // write a display color attribute
};

enum tmenu_color { TMENU_COLOR_TEMPERATURE, TMENU_COLOR_BRIGHT, TMENU_COLOR_CONTRAST, TMENU_COLOR_SATURATION };

struct tmenu_step {
    enum tmenu_step_kind kind;
    int index;  // keymon index or color
    int value;
};

#define TMENU_STEPS 32

struct tmenu_settings {
    struct tmenu_step steps[TMENU_STEPS];
    size_t step_count;
    // Set by a language step; the library keeps it for later calls without one.
    char language[64];
    int has_language;
    char theme[256];
    int has_theme;
    int bconfirm;
    int loaded;  // 1 when system.json parsed as an object
};

// text may be NULL for an unreadable file, which applies the library's defaults.
void tmenu_settings_parse(struct tmenu_settings *settings, const char *text, size_t length);

// The value the library writes to a display color attribute for a setting.
int tmenu_color_value(enum tmenu_color color, int setting);
const char *tmenu_color_path(enum tmenu_color color);

// The putenv string for a language, or NULL when the library sets none.
const char *tmenu_language_env(const char *language);
// "regular.ttf" or "full.ttf".
const char *tmenu_language_font(const char *language);

#endif
