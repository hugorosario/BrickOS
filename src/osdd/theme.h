#ifndef BRICKOS_OSDD_THEME_H
#define BRICKOS_OSDD_THEME_H

#include "look.h"
#include "profile.h"
#include "state.h"
#include "toast.h"

#include <stdint.h>

// Each consumed record snapshots the frontend and its look. Stock profile assets live on while an
// occupied slot carries them; BrickOS copies its look (docs/osdd.md) into the record.
struct osdd_theme {
    struct osdd_profile_loader loader;
    const struct osdd_toast_state *toasts;
    const char *preferences, *current, *settings, *override;
    // BrickOS's Appearance file, its fonts folder, and the folder of the system fonts.
    const char *appearance, *fonts, *system_fonts;
    // The process tree where EmulationStation is looked for.
    const char *processes;
    uint64_t frontend_check_ms;
    // emulationstation: 1 while EmulationStation, which writes the profile and leaves it behind, runs.
    int frontend_checked, material, emulationstation;
    // 1 when the system language is Portuguese, read at the frontend check in either
    // presentation: the power dialog speaks it in every look.
    int portuguese;
    struct osdd_look look;
    // The Appearance file as it was last read, and its font: it is read again when its
    // modification time, size or inode change.
    int appearance_read, appearance_present;
    long long appearance_sec, appearance_nsec, appearance_size;
    unsigned long long appearance_ino;
    char appearance_font[256];
};

void osdd_theme_init(struct osdd_theme *theme, const char *path,
                     const struct osdd_profile_assets *assets,
                     const struct osdd_toast_state *toasts);
// Looks at the profile at the loader's bounded rate. Returns 1 when the active profile changed.
int osdd_theme_tick(struct osdd_theme *theme, uint64_t now_ms);
// Read-only frontend selection, checked at most once per second. NULL paths disable it.
void osdd_theme_frontend(struct osdd_theme *theme, const char *preferences,
                         const char *current, const char *settings, const char *override);
// Where the BrickOS look comes from: the Appearance file, the fonts folder and the folder of the
// system fonts (regular.ttf and full.ttf). The defaults are the device's.
void osdd_theme_appearance(struct osdd_theme *theme, const char *appearance, const char *fonts,
                           const char *system_fonts);
// The process tree where EmulationStation is looked for, at the frontend's cadence. The default
// is /proc.
void osdd_theme_processes(struct osdd_theme *theme, const char *processes);
// The Stock++ profile in force, which the toasts and the power-off dialog use: the loaded one
// while EmulationStation runs and BrickOS is not selected, otherwise NULL for the stock look.
const struct osdd_profile *osdd_theme_profile(const struct osdd_theme *theme);
// Gives a just-consumed record its frontend snapshot or the Stock++ profile in force.
void osdd_theme_attach(const struct osdd_theme *theme, struct osdd_toast *toast);
void osdd_theme_close(struct osdd_theme *theme);

#endif
