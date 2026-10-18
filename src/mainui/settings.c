#define _POSIX_C_SOURCE 200809L
#include "settings.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

const char *const mainui_usb_modes[3] = {"host", "dock", "data"};
const char *const mainui_dpad2axis_values[7] = {
    "", "/tmp/trimui_inputd/dpad2axis_hold_f1", "/tmp/trimui_inputd/dpad2axis_hold_f2",
    "/tmp/trimui_inputd/dpad2axis_hold_r2", "/tmp/trimui_inputd/dpad2axis_hold_l2",
    "/tmp/trimui_inputd/dpad2axis_hold_select", "/tmp/trimui_inputd/dpad2axis_hold_start",
};

enum key_kind { KEY_RANGE, KEY_CHOICE, KEY_LANGUAGE, KEY_THEME, KEY_STOREURL, KEY_USBMODE, KEY_DPAD2AXIS, KEY_TIMEZONE };

struct key {
    const char *name;
    enum key_kind kind;
    int setting;         // a mainui_setting, or -1 for storeurl and timezone
    int min, max;
    const int *choices;
    int choice_count;
};

static const int dim_times[6] = {120, 300, 600, 900, 1800, -1};
static const int picture_sizes[3] = {150, 50, 100};

#define RANGE(name, set, lo, hi) { name, KEY_RANGE, set, lo, hi, NULL, 0 }
#define FLAG(name, set) RANGE(name, set, 0, 1)

// The official MainUI's write order.
static const struct key keys[] = {
    RANGE("vol", MAINUI_SET_VOL, 0, 20),
    RANGE("keymap", MAINUI_SET_KEYMAP, 0, 2),
    FLAG("mute", MAINUI_SET_MUTE),
    RANGE("bgmvol", MAINUI_SET_BGMVOL, 0, 20),
    RANGE("brightness", MAINUI_SET_BRIGHTNESS, 0, 10),
    { "language", KEY_LANGUAGE, MAINUI_SET_LANGUAGE, 0, 0, NULL, 0 },
    { "theme", KEY_THEME, MAINUI_SET_THEME, 0, 0, NULL, 0 },
    FLAG("hostap", MAINUI_SET_HOSTAP),
    FLAG("netplay", MAINUI_SET_NETPLAY),
    FLAG("wifi", MAINUI_SET_WIFI),
    // Open: the range of cpufreq, which no MainUI screen changes.
    RANGE("cpufreq", MAINUI_SET_CPUFREQ, -1000000, 10000000),
    FLAG("ledswitch", MAINUI_SET_LEDSWITCH),
    RANGE("topled", MAINUI_SET_TOPLED, 0, 10),
    RANGE("shoulderled", MAINUI_SET_SHOULDERLED, 0, 10),
    RANGE("f1f2led", MAINUI_SET_F1F2LED, 0, 10),
    FLAG("ledpolicy", MAINUI_SET_LEDPOLICY),
    RANGE("menustylel1", MAINUI_SET_MENUSTYLEL1, 0, 2),
    RANGE("menustylel2", MAINUI_SET_MENUSTYLEL2, 0, 2),
    { "storeurl", KEY_STOREURL, -1, 0, 0, NULL, 0 },
    FLAG("bluetooth", MAINUI_SET_BLUETOOTH),
    FLAG("usenetworktime", MAINUI_SET_USENETWORKTIME),
    FLAG("24hourclock", MAINUI_SET_24HOURCLOCK),
    // Only when a zone is chosen (static: saveSystemState adds it between these two).
    { "timezone", KEY_TIMEZONE, -1, 0, 0, NULL, 0 },
    RANGE("rumblelevel", MAINUI_SET_RUMBLELEVEL, 0, 5),
    { "dimtime", KEY_CHOICE, MAINUI_SET_DIMTIME, 0, 0, dim_times, 6 },
    RANGE("colorbrightness", MAINUI_SET_COLORBRIGHTNESS, 0, 10),
    RANGE("colorcontrast", MAINUI_SET_COLORCONTRAST, 0, 10),
    RANGE("colortemperature", MAINUI_SET_COLORTEMPERATURE, 0, 20),
    RANGE("colorsaturation", MAINUI_SET_COLORSATURATION, 0, 10),
    { "usbmode", KEY_USBMODE, MAINUI_SET_USBMODE, 0, 2, NULL, 0 },
    { "dpad2axis", KEY_DPAD2AXIS, MAINUI_SET_DPAD2AXIS, 0, 6, NULL, 0 },
    FLAG("enablehome", MAINUI_SET_ENABLEHOME),
    FLAG("focusontab", MAINUI_SET_FOCUSONTAB),
    FLAG("bconfirm", MAINUI_SET_BCONFIRM),
    RANGE("osdshortcut", MAINUI_SET_OSDSHORTCUT, 0, MAINUI_OSD_SHORTCUTS - 1),
    { "picturesize", KEY_CHOICE, MAINUI_SET_PICTURESIZE, 0, 0, picture_sizes, 3 },
    FLAG("favoritetab", MAINUI_SET_FAVORITETAB),
    FLAG("recentplayedtab", MAINUI_SET_RECENTPLAYEDTAB),
    FLAG("beststab", MAINUI_SET_BESTSTAB),
    FLAG("retrogametab", MAINUI_SET_RETROGAMETAB),
    FLAG("portstab", MAINUI_SET_PORTSTAB),
    FLAG("appstab", MAINUI_SET_APPSTAB),
    FLAG("netplaytab", MAINUI_SET_NETPLAYTAB),
    FLAG("enablekeytone", MAINUI_SET_ENABLEKEYTONE),
    FLAG("enablessh", MAINUI_SET_ENABLESSH),
};
#define KEY_COUNT (sizeof(keys) / sizeof(keys[0]))

static const struct key *key_for_setting(int setting) {
    for (size_t i = 0; i < KEY_COUNT; ++i)
        if (keys[i].setting == setting) return &keys[i];
    return NULL;
}

static const struct key *key_for_name(const char *name) {
    for (size_t i = 0; i < KEY_COUNT; ++i)
        if (strcasecmp(keys[i].name, name) == 0) return &keys[i];
    return NULL;
}

static int valid_number(const struct key *k, int value) {
    if (k->kind == KEY_CHOICE) {
        for (int i = 0; i < k->choice_count; ++i)
            if (k->choices[i] == value) return 1;
        return 0;
    }
    return value >= k->min && value <= k->max;
}

void mainui_settings_init(struct mainui_settings *s) {
    memset(s, 0, sizeof(*s));
    mainui_settings_defaults(s->values);
    snprintf(s->language, sizeof(s->language), "en.lang");
    snprintf(s->theme, sizeof(s->theme), "../res/");
    snprintf(s->storeurl, sizeof(s->storeurl), "f.trimui.com");
}

static int text_value(char *out, size_t size, const struct mainui_json_member *m) {
    if (m->type != MAINUI_JSON_STRING || m->text_length >= size || memchr(m->text, 0, m->text_length)) return -1;
    memcpy(out, m->text, m->text_length + 1);
    return 0;
}

static int choose_text(const struct mainui_json_member *m, const char *const *choices, int count) {
    if (m->type != MAINUI_JSON_STRING) return -1;
    for (int i = 0; i < count; ++i)
        if (strcmp(m->text, choices[i]) == 0) return i;
    return -1;
}

// The official startup's checks (static: loadSystemState). MAX takes 0..max (an unsigned
// comparison), NONNEG any value from 0, ANY any number, FLAG any number as 0 or 1.
enum startup_check { STARTUP_MAX, STARTUP_NONNEG, STARTUP_ANY, STARTUP_FLAG };
#define STARTUP_NONE (-1)  // no default: a missing value writes nothing

struct startup_key {
    const char *name;
    int index;               // keymon shared memory, or -1 for a colour
    int setting;
    enum startup_check check;
    int max;
    int fallback;            // written when missing or refused, or STARTUP_NONE
};

// In the official order; the colours sit between dimtime and bconfirm.
static const struct startup_key startup_keys[] = {
    { "vol", 0, MAINUI_SET_VOL, STARTUP_MAX, 20, 14 },
    { "keymap", 2, MAINUI_SET_KEYMAP, STARTUP_MAX, 1, 0 },
    { "mute", 3, MAINUI_SET_MUTE, STARTUP_FLAG, 0, 0 },
    { "bgmvol", 5, MAINUI_SET_BGMVOL, STARTUP_MAX, 20, 10 },
    { "brightness", 1, MAINUI_SET_BRIGHTNESS, STARTUP_MAX, 10, 6 },
    { "netplay", 6, MAINUI_SET_NETPLAY, STARTUP_FLAG, 0, 0 },
    { "wifi", 14, MAINUI_SET_WIFI, STARTUP_FLAG, 0, STARTUP_NONE },
    { "hostap", 15, MAINUI_SET_HOSTAP, STARTUP_FLAG, 0, STARTUP_NONE },
    { "bluetooth", 16, MAINUI_SET_BLUETOOTH, STARTUP_FLAG, 0, STARTUP_NONE },
    { "cpufreq", 7, MAINUI_SET_CPUFREQ, STARTUP_NONNEG, 0, 0 },
    { "ledswitch", 8, MAINUI_SET_LEDSWITCH, STARTUP_FLAG, 0, 1 },
    { "topled", 10, MAINUI_SET_TOPLED, STARTUP_ANY, 0, 4 },
    { "shoulderled", 17, MAINUI_SET_SHOULDERLED, STARTUP_ANY, 0, 4 },
    { "f1f2led", 18, MAINUI_SET_F1F2LED, STARTUP_ANY, 0, 4 },
    { "ledpolicy", 19, MAINUI_SET_LEDPOLICY, STARTUP_ANY, 0, 1 },
    { "menustylel1", 9, MAINUI_SET_MENUSTYLEL1, STARTUP_ANY, 0, 1 },
    { "menustylel2", 11, MAINUI_SET_MENUSTYLEL2, STARTUP_ANY, 0, 0 },
    { "rumblelevel", 12, MAINUI_SET_RUMBLELEVEL, STARTUP_MAX, 5, 4 },
    { "dimtime", 13, MAINUI_SET_DIMTIME, STARTUP_ANY, 0, 300 },
    { "colortemperature", -1, MAINUI_SET_COLORTEMPERATURE, STARTUP_ANY, 0, STARTUP_NONE },
    { "colorbrightness", -1, MAINUI_SET_COLORBRIGHTNESS, STARTUP_ANY, 0, STARTUP_NONE },
    { "colorcontrast", -1, MAINUI_SET_COLORCONTRAST, STARTUP_ANY, 0, STARTUP_NONE },
    { "colorsaturation", -1, MAINUI_SET_COLORSATURATION, STARTUP_ANY, 0, STARTUP_NONE },
    { "bconfirm", 22, MAINUI_SET_BCONFIRM, STARTUP_FLAG, 0, 0 },
    { "osdshortcut", 21, MAINUI_SET_OSDSHORTCUT, STARTUP_ANY, 0, 0 },
};
#define STARTUP_KEY_COUNT (sizeof(startup_keys) / sizeof(startup_keys[0]))

// An unreadable file: every default, in the official order, with the colour setters' defaults
// between bconfirm and osdshortcut.
static const int startup_defaults[][2] = {
    {0, 14}, {2, 0}, {3, 0}, {5, 10}, {1, 6}, {6, 0}, {7, 0}, {8, 1}, {10, 4}, {17, 4}, {18, 4},
    {19, 1}, {9, 1}, {11, 0}, {12, 4}, {13, 300}, {14, 0}, {15, 0}, {16, 0}, {22, 0},
};
static const int startup_default_colours[4][2] = {
    {MAINUI_SET_COLORBRIGHTNESS, 5}, {MAINUI_SET_COLORCONTRAST, 5},
    {MAINUI_SET_COLORTEMPERATURE, 10}, {MAINUI_SET_COLORSATURATION, 5},
};

// The colour setters' ranges: Exposure and Contrast 1..10, Saturation 0..10, Temperature 0..20.
static int startup_colour(int setting, int value) {
    int low = setting == MAINUI_SET_COLORBRIGHTNESS || setting == MAINUI_SET_COLORCONTRAST ? 1 : 0;
    int high = setting == MAINUI_SET_COLORTEMPERATURE ? 20 : 10;
    return value < low ? low : value > high ? high : value;
}

// cJSON's valueint: the number truncated, saturated at the int limits.
static int startup_int(double number) {
    if (number >= 2147483647.0) return 2147483647;
    if (number <= -2147483648.0) return (-2147483647 - 1);
    return (int)number;
}

static void startup_shm(struct mainui_startup_state *st, int index, int value) {
    if (st->shm_count >= MAINUI_STARTUP_SHM) return;
    st->shm_index[st->shm_count] = index;
    st->shm_value[st->shm_count++] = value;
}

static void startup_colour_call(struct mainui_startup_state *st, int setting, int value) {
    if (st->colour_count >= 4) return;
    st->colour_setting[st->colour_count] = setting;
    st->colour_value[st->colour_count++] = startup_colour(setting, value);
}

// Fills s->startup for json (NULL: an unreadable file), then moves the model to the values it
// writes where the model can show them.
static void startup_state(struct mainui_settings *s, const struct mainui_json *json) {
    struct mainui_startup_state *st = &s->startup;
    memset(st, 0, sizeof(*st));
    if (!json) {
        for (size_t i = 0; i < sizeof(startup_defaults) / sizeof(startup_defaults[0]); ++i)
            startup_shm(st, startup_defaults[i][0], startup_defaults[i][1]);
        for (int i = 0; i < 4; ++i) startup_colour_call(st, startup_default_colours[i][0], startup_default_colours[i][1]);
        startup_shm(st, 21, 0);  // osdshortcut
    } else {
        for (size_t i = 0; i < STARTUP_KEY_COUNT; ++i) {
            const struct startup_key *k = &startup_keys[i];
            const struct mainui_json_member *m = mainui_json_get(json, k->name);
            int present = m && m->type == MAINUI_JSON_NUMBER, value = present ? startup_int(m->number) : 0;
            if (present && k->check == STARTUP_MAX && (value < 0 || value > k->max)) present = 0;
            if (present && k->check == STARTUP_NONNEG && value < 0) present = 0;
            if (present && k->check == STARTUP_FLAG) value = value != 0;
            if (k->index < 0) {
                if (present) startup_colour_call(st, k->setting, value);
            } else if (present) {
                startup_shm(st, k->index, value);
            } else if (k->fallback != STARTUP_NONE) {
                startup_shm(st, k->index, k->fallback);
            }
        }
    }
    for (int i = 0; i < st->shm_count; ++i)
        for (size_t j = 0; j < STARTUP_KEY_COUNT; ++j) {
            const struct key *k = key_for_setting(startup_keys[j].setting);
            // OSD Shortcut keeps its own migration of the legacy index (see below).
            if (startup_keys[j].index != st->shm_index[i] || startup_keys[j].setting == MAINUI_SET_OSDSHORTCUT || !k ||
                !valid_number(k, st->shm_value[i])) continue;
            s->values[startup_keys[j].setting] = st->shm_value[i];
        }
    for (int i = 0; i < st->colour_count; ++i) {
        int setting = st->colour_setting[i];
        if (s->values[setting] != st->colour_value[i] && json) s->changed |= 1ULL << setting;
        s->values[setting] = st->colour_value[i];
    }
}

int mainui_settings_parse(struct mainui_settings *s, const char *text, size_t length) {
    static struct mainui_json json;
    mainui_settings_init(s);
    if (mainui_json_parse(&json, text, length) < 0) {
        startup_state(s, NULL);
        return -1;
    }
    s->loaded = 1;
    int seen[KEY_COUNT] = {0};
    for (size_t i = 0; i < json.count; ++i) {
        const struct mainui_json_member *m = &json.members[i];
        const struct key *k = key_for_name(m->key);
        if (!k) {
            if (s->unknown.count < MAINUI_JSON_MEMBERS) s->unknown.members[s->unknown.count++] = *m;
            continue;
        }
        // The first member with a name counts, as cJSON_GetObjectItem finds it.
        size_t index = (size_t)(k - keys);
        if (seen[index]++) continue;
        int ok = 0;
        switch (k->kind) {
        case KEY_RANGE:
        case KEY_CHOICE:
            // cJSON's valueint: the number truncated.
            if (m->type == MAINUI_JSON_NUMBER && m->number > -2e9 && m->number < 2e9 && valid_number(k, (int)m->number)) {
                s->values[k->setting] = (int)m->number;
                ok = 1;
            }
            break;
        case KEY_LANGUAGE: ok = text_value(s->language, sizeof(s->language), m) == 0; break;
        case KEY_THEME: ok = text_value(s->theme, sizeof(s->theme), m) == 0; break;
        case KEY_STOREURL: ok = text_value(s->storeurl, sizeof(s->storeurl), m) == 0; break;
        case KEY_TIMEZONE: ok = text_value(s->timezone, sizeof(s->timezone), m) == 0; break;
        case KEY_USBMODE: {
            int index_value = choose_text(m, mainui_usb_modes, 3);
            if (index_value >= 0) { s->values[k->setting] = index_value; ok = 1; }
            break;
        }
        case KEY_DPAD2AXIS: {
            int index_value = choose_text(m, mainui_dpad2axis_values, 7);
            if (index_value > 0) { s->values[k->setting] = index_value; ok = 1; }
            break;
        }
        }
        if (!ok) ++s->repaired;
    }
    startup_state(s, &json);
    if (s->values[MAINUI_SET_OSDSHORTCUT] == 0) {
        s->values[MAINUI_SET_OSDSHORTCUT] = 1;
        s->changed |= 1ULL << MAINUI_SET_OSDSHORTCUT;
        s->osd_migrated = 1;
    }
    return 0;
}

static char *read_all(const char *path, size_t *length) {
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

int mainui_settings_load(struct mainui_settings *s, const char *path) {
    size_t length = 0;
    char *text = read_all(path, &length);
    if (!text) {
        mainui_settings_init(s);
        startup_state(s, NULL);
        return -1;
    }
    int result = mainui_settings_parse(s, text, length);
    free(text);
    return result;
}

int mainui_settings_set(struct mainui_settings *s, enum mainui_setting setting, int value) {
    if (setting == MAINUI_SET_OSDSHORTCUT && value == 0) return -1;
    const struct key *k = key_for_setting((int)setting);
    if (!k) return -1;
    if (k->kind == KEY_RANGE || k->kind == KEY_CHOICE) {
        if (!valid_number(k, value)) return -1;
    } else if (k->kind == KEY_USBMODE || k->kind == KEY_DPAD2AXIS) {
        if (value < k->min || value > k->max) return -1;
    } else {
        return -1;  // language and theme are text
    }
    s->values[setting] = value;
    if (setting == MAINUI_SET_OSDSHORTCUT) s->osd_migrated = 0;
    s->changed |= 1ULL << setting;
    return 0;
}

static int set_text(char *out, size_t size, const char *value, int *changed) {
    if (!value || strlen(value) >= size) return -1;
    snprintf(out, size, "%s", value);
    *changed = 1;
    return 0;
}

int mainui_settings_set_language(struct mainui_settings *s, const char *language) {
    return set_text(s->language, sizeof(s->language), language, &s->language_changed);
}

int mainui_settings_set_theme(struct mainui_settings *s, const char *theme) {
    return set_text(s->theme, sizeof(s->theme), theme, &s->theme_changed);
}

int mainui_settings_set_timezone(struct mainui_settings *s, const char *zone) {
    return set_text(s->timezone, sizeof(s->timezone), zone, &s->timezone_changed);
}

// ---- Output --------------------------------------------------------------------------------

struct buffer {
    char *text;
    size_t length, capacity;
    int failed;
};

static void append(struct buffer *b, const char *text, size_t length) {
    if (b->failed) return;
    if (b->length + length + 1 > b->capacity) {
        size_t capacity = b->capacity ? b->capacity * 2 : 4096;
        while (capacity < b->length + length + 1) capacity *= 2;
        char *grown = realloc(b->text, capacity);
        if (!grown) { b->failed = 1; return; }
        b->text = grown;
        b->capacity = capacity;
    }
    memcpy(b->text + b->length, text, length);
    b->length += length;
    b->text[b->length] = 0;
}

static void append_member(struct buffer *b, int *first, const char *name, const char *value) {
    char escaped[MAINUI_JSON_KEY * 6 + 8];
    append(b, *first ? "\n\t" : ",\n\t", *first ? 2 : 3);
    *first = 0;
    if (mainui_json_escape(escaped, sizeof(escaped), name, strlen(name)) < 0) { b->failed = 1; return; }
    append(b, escaped, strlen(escaped));
    append(b, ":\t", 2);
    append(b, value, strlen(value));
}

static void append_text_member(struct buffer *b, int *first, const char *name, const char *text, size_t length) {
    char escaped[MAINUI_JSON_TEXT * 6 + 8];
    if (mainui_json_escape(escaped, sizeof(escaped), text, length) < 0) { b->failed = 1; return; }
    append_member(b, first, name, escaped);
}

char *mainui_settings_format(const struct mainui_settings *s, size_t *length) {
    struct buffer b = {0};
    int first = 1;
    char number[32];
    append(&b, "{", 1);
    for (size_t i = 0; i < KEY_COUNT; ++i) {
        const struct key *k = &keys[i];
        switch (k->kind) {
        case KEY_RANGE:
        case KEY_CHOICE:
            snprintf(number, sizeof(number), "%d", s->values[k->setting]);
            append_member(&b, &first, k->name, number);
            break;
        case KEY_LANGUAGE: append_text_member(&b, &first, k->name, s->language, strlen(s->language)); break;
        case KEY_THEME: append_text_member(&b, &first, k->name, s->theme, strlen(s->theme)); break;
        case KEY_STOREURL: append_text_member(&b, &first, k->name, s->storeurl, strlen(s->storeurl)); break;
        case KEY_TIMEZONE:
            if (s->timezone[0]) append_text_member(&b, &first, k->name, s->timezone, strlen(s->timezone));
            break;
        case KEY_USBMODE: {
            const char *mode = mainui_usb_modes[s->values[k->setting]];
            append_text_member(&b, &first, k->name, mode, strlen(mode));
            break;
        }
        case KEY_DPAD2AXIS:
            // Off leaves the key out.
            if (s->values[k->setting] > 0) {
                const char *value = mainui_dpad2axis_values[s->values[k->setting]];
                append_text_member(&b, &first, k->name, value, strlen(value));
            }
            break;
        }
    }
    for (size_t i = 0; i < s->unknown.count; ++i) {
        const struct mainui_json_member *m = &s->unknown.members[i];
        if (m->type == MAINUI_JSON_STRING) append_text_member(&b, &first, m->key, m->text, m->text_length);
        else append_member(&b, &first, m->key, m->text);
    }
    append(&b, "\n}", 2);
    if (b.failed) { free(b.text); return NULL; }
    *length = b.length;
    return b.text;
}

int mainui_settings_save(struct mainui_settings *s, const char *path) {
    // Keys the frontend did not change keep the value on disk, where keymon may have written.
    struct mainui_settings *merged = malloc(sizeof(*merged));
    if (!merged) return -1;
    if (mainui_settings_load(merged, path) < 0 || !merged->loaded) *merged = *s;
    for (int setting = 0; setting < MAINUI_SET_COUNT; ++setting)
        if ((s->changed & (1ULL << setting)) &&
            !(setting == MAINUI_SET_OSDSHORTCUT && s->osd_migrated && !merged->osd_migrated))
            merged->values[setting] = s->values[setting];
    if (s->language_changed) snprintf(merged->language, sizeof(merged->language), "%s", s->language);
    if (s->theme_changed) snprintf(merged->theme, sizeof(merged->theme), "%s", s->theme);
    if (s->timezone_changed) snprintf(merged->timezone, sizeof(merged->timezone), "%s", s->timezone);
    merged->changed = 0;
    merged->osd_migrated = 0;
    merged->language_changed = merged->theme_changed = merged->storeurl_changed = merged->timezone_changed = 0;
    size_t length;
    char *text = mainui_settings_format(merged, &length);
    if (!text) { free(merged); return -1; }
    char temporary[1100];
    int ok = snprintf(temporary, sizeof(temporary), "%s.tmp", path) < (int)sizeof(temporary);
    FILE *file = ok ? fopen(temporary, "wb") : NULL;
    ok = file && fwrite(text, 1, length, file) == length;
    if (file && (fflush(file) != 0 || fsync(fileno(file)) != 0)) ok = 0;
    if (file && fclose(file) != 0) ok = 0;
    free(text);
    if (!ok || rename(temporary, path) != 0) {
        unlink(temporary);
        free(merged);
        return -1;
    }
    *s = *merged;
    free(merged);
    return 0;
}

int mainui_settings_key(const char *name) {
    const struct key *k = name ? key_for_name(name) : NULL;
    return k ? k->setting : -1;
}
