#ifndef BRICKOS_TMENU_SYSTEM_H
#define BRICKOS_TMENU_SYSTEM_H

#include "settings.h"

#include <stddef.h>

// The official library's effects outside the menu: keymon's shared memory, the
// environment, the display color attributes, and the rumble GPIO. Paths are fields
// so host tests can point them at a temporary tree.
struct tmenu_system_paths {
    const char *system_json;   // /mnt/UDISK/system.json
    const char *res;           // /usr/trimui/res/
    const char *rumble;        // /sys/class/gpio/gpio227/value
    const char *color_dir;     // NULL: the real attribute paths
    int keyshm_key;            // 0x4e4f4d4b
};

extern const struct tmenu_system_paths tmenu_system_default_paths;

struct tmenu_system {
    struct tmenu_system_paths paths;
    int *keyshm;               // 23 values, or NULL when keymon's memory is unavailable
    struct tmenu_settings settings;
    // The theme folder, when system.json names one that exists; NULL otherwise.
    const char *theme;
};

// The language the official library keeps between calls; "en.lang" until one is set.
const char *tmenu_system_language(void);

// Attaches the key memory, reads system.json, and applies it in the official order.
void tmenu_system_open(struct tmenu_system *system, const struct tmenu_system_paths *paths);
int tmenu_system_keyshm_get(struct tmenu_system *system, int index);
void tmenu_system_keyshm_set(struct tmenu_system *system, int index, int value);
void tmenu_system_rumble_off(struct tmenu_system *system);
void tmenu_system_close(struct tmenu_system *system);

// Reads a whole file; returns a malloc'ed NUL-terminated buffer or NULL.
char *tmenu_read_file(const char *path, size_t *length);

#endif
