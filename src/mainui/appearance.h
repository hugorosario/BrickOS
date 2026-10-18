#ifndef BRICKOS_MAINUI_APPEARANCE_H
#define BRICKOS_MAINUI_APPEARANCE_H

#include "json.h"
#include "model.h"

#include <stddef.h>

// The BrickOS Themes > Appearance choices (docs/material-you-frontend.md). They live in their
// own file, beside preferences.json, which the OSD and the in-game menu read too. Defaults are
// today's look; the stock theme never takes part. This part needs only json.c, so the OSD reads
// the same file with the same rules; the fonts folder's lister is in appearance_fonts.h.

#define MAINUI_APPEARANCE_PATH "/mnt/SDCARD/BrickOS/mainui/brickos/appearance.json"
#define MAINUI_APPEARANCE_BYTES 16384
#define MAINUI_FONTS_DIR "/mnt/SDCARD/BrickOS/fonts"
#define MAINUI_FONT_FILE_BYTES (16L * 1024 * 1024)
#define MAINUI_FONT_NAME 256

struct mainui_appearance {
    int values[MAINUI_APPEAR_SETTINGS];  // by setting - MAINUI_SET_APPEAR_ACCENT
    char font[MAINUI_FONT_NAME];         // a file of MAINUI_FONTS_DIR, or "" for Default
    struct mainui_json document;         // the file's members, kept across writes
};

struct mainui_rgb {
    unsigned char r, g, b;
};

// The shade over the BrickOS wallpapers: an overall shade and tint, and the darkening behind
// the footer (48 bands of 8 pixels, alpha band_base + band_step * band).
struct mainui_dimming_values {
    int shade, tint, band_base, band_step;
};

void mainui_appearance_defaults(struct mainui_appearance *appearance);
// Reads the file. Returns 1 when it was read, 0 when it is missing, and -1 when it is not a
// JSON object of at most MAINUI_APPEARANCE_BYTES; the last two give the defaults. In a file that
// was read, a missing or unknown value gives that option's default.
int mainui_appearance_load(struct mainui_appearance *appearance, const char *path);
// Version 1 with every choice, then the members the file held that are not choices. Returns 0,
// or -1 when it does not fit size or MAINUI_APPEARANCE_BYTES.
int mainui_appearance_format(const struct mainui_appearance *appearance, char *text, size_t size, size_t *length);

// The choice for a MAINUI_SET_APPEAR_* setting, and its number of options and default.
int mainui_appearance_get(const struct mainui_appearance *appearance, int setting);
void mainui_appearance_set(struct mainui_appearance *appearance, int setting, int value);
int mainui_appearance_options(int setting);
int mainui_appearance_default(int setting);
// The name an option is stored under in the file.
const char *mainui_appearance_name(int setting, int value);
// A row's label and an option's label, in English or Portuguese.
const char *mainui_appearance_label(int setting, int portuguese);
const char *mainui_appearance_value_label(int setting, int value, int portuguese);
// The screens' other texts, by their English text: "Appearance", "Font", "Default",
// "Reset appearance", "Reset appearance?", "Every option returns to its default.",
// "Appearance not saved". Other text comes back unchanged.
const char *mainui_appearance_text(const char *english, int portuguese);

// What the renderer draws from the choices.
struct mainui_rgb mainui_appearance_seed(const struct mainui_appearance *appearance);
double mainui_appearance_background_level(const struct mainui_appearance *appearance);
double mainui_appearance_text_scale(const struct mainui_appearance *appearance);
// The corner radius of a w x h shape (Rounded: min(h/5, 24, w/5)).
int mainui_appearance_corner(const struct mainui_appearance *appearance, int w, int h);
// The same for a card shape (enum mainui_shape), as the OSD keeps it.
int mainui_appearance_shape_corner(int shape, int w, int h);
struct mainui_dimming_values mainui_appearance_dimming(const struct mainui_appearance *appearance);
enum mainui_halo mainui_appearance_halo(const struct mainui_appearance *appearance);
// 1 for Normal, 0.5 for Fast (every motion takes half the time), 0 for Off.
double mainui_appearance_motion(const struct mainui_appearance *appearance);

// A font of the folder is named by a plain file name, never a path.
int mainui_appearance_font_name_valid(const char *name, size_t length);

#endif
