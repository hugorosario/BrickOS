#define _DEFAULT_SOURCE
#include "settings.h"

#include "json.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>

static void step(struct tmenu_settings *s, enum tmenu_step_kind kind, int index, int value) {
    if (s->step_count < TMENU_STEPS) s->steps[s->step_count++] = (struct tmenu_step){kind, index, value};
}

// A keymon value that falls back to its default when the key is missing or not a number.
static void number_or_default(struct tmenu_settings *s, const struct tmenu_json *json, const char *key,
                              int index, int fallback) {
    const struct tmenu_json_member *m = tmenu_json_get(json, key);
    step(s, TMENU_STEP_KEYSHM, index, m && m->type == TMENU_JSON_NUMBER ? m->number : fallback);
}

// wifi, hostap, and bluetooth are written as booleans, and only when present.
static void flag_if_present(struct tmenu_settings *s, const struct tmenu_json *json, const char *key, int index) {
    const struct tmenu_json_member *m = tmenu_json_get(json, key);
    if (m && m->type == TMENU_JSON_NUMBER) step(s, TMENU_STEP_KEYSHM, index, m->number != 0);
}

static void color_if_present(struct tmenu_settings *s, const struct tmenu_json *json, const char *key,
                             enum tmenu_color color) {
    const struct tmenu_json_member *m = tmenu_json_get(json, key);
    if (m && m->type == TMENU_JSON_NUMBER) step(s, TMENU_STEP_COLOR, (int)color, m->number);
}

static void defaults(struct tmenu_settings *s) {
    static const int values[][2] = {
        {0, 14}, {2, 0}, {3, 0}, {5, 10}, {1, 6}, {6, 0}, {7, 0}, {8, 1}, {10, 4}, {17, 4},
        {18, 4}, {19, 1}, {9, 1}, {11, 0}, {12, 4}, {13, 300}, {14, 0}, {15, 0}, {16, 0}, {22, 0},
    };
    for (size_t i = 0; i < sizeof(values) / sizeof(values[0]); ++i)
        step(s, TMENU_STEP_KEYSHM, values[i][0], values[i][1]);
    step(s, TMENU_STEP_COLOR, TMENU_COLOR_BRIGHT, 5);
    step(s, TMENU_STEP_COLOR, TMENU_COLOR_CONTRAST, 5);
    step(s, TMENU_STEP_COLOR, TMENU_COLOR_TEMPERATURE, 10);
    step(s, TMENU_STEP_COLOR, TMENU_COLOR_SATURATION, 5);
    step(s, TMENU_STEP_KEYSHM, 21, 0);
}

void tmenu_settings_parse(struct tmenu_settings *s, const char *text, size_t length) {
    static struct tmenu_json json;
    memset(s, 0, sizeof(*s));
    if (!text || tmenu_json_parse(&json, text, length) < 0) {
        defaults(s);
        return;
    }
    s->loaded = 1;
    number_or_default(s, &json, "vol", 0, 14);
    number_or_default(s, &json, "keymap", 2, 0);
    number_or_default(s, &json, "mute", 3, 0);
    number_or_default(s, &json, "bgmvol", 5, 10);
    number_or_default(s, &json, "brightness", 1, 6);
    const struct tmenu_json_member *m = tmenu_json_get(&json, "language");
    if (m && m->type == TMENU_JSON_STRING) {
        snprintf(s->language, sizeof(s->language), "%.63s", m->text);
        s->has_language = 1;
        step(s, TMENU_STEP_LANGUAGE, 0, 0);
    }
    m = tmenu_json_get(&json, "theme");
    if (m && m->type == TMENU_JSON_STRING) {
        snprintf(s->theme, sizeof(s->theme), "%s", m->text);
        s->has_theme = 1;
        step(s, TMENU_STEP_THEME, 0, 0);
    }
    number_or_default(s, &json, "netplay", 6, 0);
    flag_if_present(s, &json, "wifi", 14);
    flag_if_present(s, &json, "hostap", 15);
    flag_if_present(s, &json, "bluetooth", 16);
    number_or_default(s, &json, "cpufreq", 7, 0);
    number_or_default(s, &json, "ledswitch", 8, 1);
    number_or_default(s, &json, "topled", 10, 4);
    number_or_default(s, &json, "shoulderled", 17, 4);
    number_or_default(s, &json, "f1f2led", 18, 4);
    number_or_default(s, &json, "ledpolicy", 19, 1);
    number_or_default(s, &json, "menustylel1", 9, 1);
    number_or_default(s, &json, "menustylel2", 11, 0);
    number_or_default(s, &json, "rumblelevel", 12, 4);
    number_or_default(s, &json, "dimtime", 13, 300);
    color_if_present(s, &json, "colortemperature", TMENU_COLOR_TEMPERATURE);
    color_if_present(s, &json, "colorbrightness", TMENU_COLOR_BRIGHT);
    color_if_present(s, &json, "colorcontrast", TMENU_COLOR_CONTRAST);
    color_if_present(s, &json, "colorsaturation", TMENU_COLOR_SATURATION);
    m = tmenu_json_get(&json, "bconfirm");
    s->bconfirm = m && m->type == TMENU_JSON_NUMBER ? m->number : 0;
    step(s, TMENU_STEP_KEYSHM, TMENU_KEYSHM_BCONFIRM, s->bconfirm);
    number_or_default(s, &json, "osdshortcut", 21, 0);
}

static int clamp(int value, int low, int high) {
    return value < low ? low : value > high ? high : value;
}

int tmenu_color_value(enum tmenu_color color, int setting) {
    switch (color) {
    case TMENU_COLOR_TEMPERATURE: return (clamp(setting, 0, 20) - 10) * 5;
    default: return clamp(setting, 0, 10) * 10;
    }
}

const char *tmenu_color_path(enum tmenu_color color) {
    switch (color) {
    case TMENU_COLOR_TEMPERATURE: return "/sys/devices/virtual/disp/disp/attr/color_temperature";
    case TMENU_COLOR_BRIGHT: return "/sys/devices/virtual/disp/disp/attr/enhance_bright";
    case TMENU_COLOR_CONTRAST: return "/sys/devices/virtual/disp/disp/attr/enhance_contrast";
    default: return "/sys/devices/virtual/disp/disp/attr/enhance_saturation";
    }
}

const char *tmenu_language_env(const char *language) {
    static const char *const map[][2] = {
        {"en.lang", "LANG=en_US.UTF8"}, {"ch.lang", "LANG=zh_CN.UTF8"}, {"es.lang", "LANG=es_ES.UTF8"},
        {"pt.lang", "LANG=pt_BR.UTF8"}, {"jp.lang", "LANG=ja_JP.UTF8"}, {"ko.lang", "LANG=ko_KR.UTF8"},
    };
    for (size_t i = 0; i < sizeof(map) / sizeof(map[0]); ++i)
        if (strcasecmp(language, map[i][0]) == 0) return map[i][1];
    return NULL;
}

const char *tmenu_language_font(const char *language) {
    static const char *const regular[] = {"en.lang", "ch.lang", "es.lang", "pt.lang", "it.lang", "fr.lang", "de.lang"};
    for (size_t i = 0; i < sizeof(regular) / sizeof(regular[0]); ++i)
        if (strcasecmp(language, regular[i]) == 0) return "regular.ttf";
    return "full.ttf";
}
