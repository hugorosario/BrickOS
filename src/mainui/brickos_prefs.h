#ifndef BRICKOS_MAINUI_BRICKOS_PREFS_H
#define BRICKOS_MAINUI_BRICKOS_PREFS_H

#include "json.h"
#include "model.h"

#define MAINUI_BRICKOS_PREFERENCES_PATH "/mnt/SDCARD/BrickOS/mainui/brickos/preferences.json"
#define MAINUI_BRICKOS_PREFERENCES_BYTES 16384

struct mainui_brickos_preferences {
    enum mainui_brickos_initial_view initial_view;
    enum mainui_presentation presentation;
    int presentation_present;
    int present;
    struct mainui_json document; // unknown compatible members retained across writes
};

const char *mainui_brickos_initial_view_name(enum mainui_brickos_initial_view view);
int mainui_brickos_initial_view_parse(const char *name, enum mainui_brickos_initial_view *view);
void mainui_brickos_preferences_init(struct mainui_brickos_preferences *preferences);
// A missing file selects the documented desktop default. Invalid or unsupported files return -1
// and are left untouched; preferences still contains the safe default.
int mainui_brickos_preferences_load(struct mainui_brickos_preferences *preferences, const char *path);
// Publishes atomically and updates the in-memory document only after rename succeeds.
int mainui_brickos_preferences_save(struct mainui_brickos_preferences *preferences, const char *path,
                                   enum mainui_brickos_initial_view view);
int mainui_brickos_preferences_save_presentation(struct mainui_brickos_preferences *preferences,
                                                const char *path, enum mainui_presentation presentation);
enum mainui_presentation mainui_brickos_preferences_presentation(const struct mainui_brickos_preferences *preferences,
                                                                enum mainui_presentation fallback, int explicit_choice);

#endif
