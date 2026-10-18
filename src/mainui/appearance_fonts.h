#ifndef BRICKOS_MAINUI_APPEARANCE_FONTS_H
#define BRICKOS_MAINUI_APPEARANCE_FONTS_H

#include "appearance.h"
#include "catalog.h"

// The fonts folder of BrickOS's Appearance, for MainUI's font list.

// One valid font of the fonts folder: its file, and its name without the extension.
struct mainui_font_entry {
    char file[MAINUI_FONT_NAME];
    char name[MAINUI_FONT_NAME];
};

// The valid fonts of the fonts folder under fs's root, sorted by file name: regular .ttf or
// .otf files of at most MAINUI_FONT_FILE_BYTES that start with a TrueType or OpenType
// signature. Returns the count, with *fonts to free, or 0 with *fonts NULL.
int mainui_appearance_fonts(const struct mainui_fs *fs, struct mainui_font_entry **fonts);

#endif
