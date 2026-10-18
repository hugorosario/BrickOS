#ifndef BRICKOS_MAINUI_SETTINGS_H
#define BRICKOS_MAINUI_SETTINGS_H

#include "json.h"
#include "model.h"
#include "catalog.h"

#include <stddef.h>

// /mnt/UDISK/system.json, the settings the frontend shares with keymon, inputd, and the
// in-game menu. Measured on the official MainUI:
// - It writes tab-indented cJSON with no final newline, in its own key order, with dpad2axis
//   (only when set) between usbmode and enablehome.
// - It drops keys it does not know. BrickOS keeps them, after the stock keys, in file order.
// - keymon also rewrites the file (volume and Bluetooth), keeping other keys. A save merges:
//   keys the frontend did not change since it loaded keep their value on disk.

// Text settings the model holds as an index.
extern const char *const mainui_usb_modes[3];        // host, dock, data
extern const char *const mainui_dpad2axis_values[7]; // "" (Off, key absent), then f1 ... start

// What the official MainUI applies from the file each time it starts (static: loadSystemState,
// which main runs after InitKeyShm): keymon's shared-memory integers, then the display
// enhancement through its setters. keymon loads its own copy only when it starts, so this
// puts back what a game, an app, or a session left changed. The shared-memory writes keep the
// official order and checks: a missing, mistyped, or out-of-range value writes its default,
// wifi, hostap, and bluetooth write nothing when missing, and an unreadable file writes every
// default. The colours are clamped as the setters do and only written when present (or all
// four defaults for an unreadable file).
#define MAINUI_STARTUP_SHM 24
struct mainui_startup_state {
    int shm_count;
    int shm_index[MAINUI_STARTUP_SHM], shm_value[MAINUI_STARTUP_SHM];
    int colour_count;
    int colour_setting[4], colour_value[4];  // mainui_setting, clamped value; in call order
};

struct mainui_settings {
    int values[MAINUI_SET_COUNT];  // numbers; usbmode and dpad2axis as indexes
    char language[64];
    // Match catalog/audio/renderer paths. A valid 255-byte SD folder name gives
    // a 275-byte theme value, including /mnt/SDCARD/Themes/ and its trailing slash.
    char theme[MAINUI_THEME_PATH_BYTES];
    char storeurl[128];
    char timezone[128];            // "timezone", relative to zoneinfo; "" (Default) leaves it out
    unsigned long long changed;    // bit per mainui_setting set since the load
    int storeurl_changed, language_changed, theme_changed, timezone_changed;
    struct mainui_json unknown;    // members that are not stock keys, in file order
    int loaded;                    // 1 when the file parsed as an object
    int repaired;                  // number of values replaced by their default
    int osd_migrated;              // legacy index 0 mapped to 1; not a user edit
    // The official startup's writes for this file. The values above follow them where the
    // model can show the value, as the official screens read keymon's memory and the clamped
    // colours; a clamped colour counts as changed, so the next save writes it, as the official
    // save writes its clamped globals.
    struct mainui_startup_state startup;
};
_Static_assert(MAINUI_SET_COUNT <= 64, "changed holds one bit per mainui_setting");

// The values of a fresh file (those seen on the reference device).
void mainui_settings_init(struct mainui_settings *s);
// Parses file text over the defaults. A value of the wrong type or out of its range keeps the
// default and counts in repaired. Returns 0, or -1 when the text is not an object.
// BrickOS reads text that is not a flat object as an unreadable file.
int mainui_settings_parse(struct mainui_settings *s, const char *text, size_t length);
// Reads the file; a missing or unreadable file leaves the defaults, with the official
// startup's defaults for the values it writes. Returns as parse.
int mainui_settings_load(struct mainui_settings *s, const char *path);

// Sets a value and marks it changed. Returns 0, or -1 when the value is out of range.
int mainui_settings_set(struct mainui_settings *s, enum mainui_setting key, int value);
int mainui_settings_set_language(struct mainui_settings *s, const char *language);
int mainui_settings_set_theme(struct mainui_settings *s, const char *theme);
// The Time Zone screen stores "timezone"; "" (Default) leaves the key out.
int mainui_settings_set_timezone(struct mainui_settings *s, const char *zone);

// The setting for a system.json key name, or -1 (storeurl and unknown keys).
int mainui_settings_key(const char *name);

// The file text as the official MainUI writes it, followed by the unknown keys.
char *mainui_settings_format(const struct mainui_settings *s, size_t *length);
// Merges with the file on disk (see above) and replaces it through PATH.tmp and a rename.
// Returns 0 or -1.
int mainui_settings_save(struct mainui_settings *s, const char *path);

#endif
