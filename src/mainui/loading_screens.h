#ifndef BRICKOS_MAINUI_LOADING_SCREENS_H
#define BRICKOS_MAINUI_LOADING_SCREENS_H

#include <stddef.h>

#define MAINUI_LOADING_SCREEN_NAME 256
#define MAINUI_LOADING_SCREEN_PATH 1024
#define MAINUI_LOADING_SCREEN_FRAMES 99
// RetroArch reads its loading screen from this fixed folder, and BrickOS keeps presets here.
#define MAINUI_LOADING_SCREENS_DIRECTORY "/mnt/SDCARD/BrickOS/loading-screens/"
#define MAINUI_LOADING_SKIN_DIRECTORY "/mnt/SDCARD/trimui/res/skin/"
// mainui_loading_screens_current: no loading-screen images, so RetroArch draws its own logo.
#define MAINUI_LOADING_SCREEN_TRIMUI (-3)

// One preset: a background and the frames 01..frames, each with the size it had when listed.
struct mainui_loading_screen {
    int index;
    char name[MAINUI_LOADING_SCREEN_NAME];    // the folder name
    char folder[MAINUI_LOADING_SCREEN_PATH];  // device path of the folder, ending in '/'
    int frames;
    size_t sizes[MAINUI_LOADING_SCREEN_FRAMES + 1]; // [0] the background, [n] frame n
};

// Device path of a preset's background (frame 0) or frame n. Returns 0, or -1 on overflow or
// a frame outside the set.
int mainui_loading_screen_file(const struct mainui_loading_screen *screen, int frame, char *out, size_t size);

// Lists every valid preset folder, sorted by name. The caller owns *items and must free it.
// Returns the count (0 when the folder is missing), or -1 for an I/O error or overflow.
int mainui_loading_screens_catalog(const char *root, struct mainui_loading_screen **items);

// Compares the installed images with the listed presets. Returns the matching preset's index,
// MAINUI_LOADING_SCREEN_TRIMUI when neither the background nor frame 01 is installed, -1 for
// any other set (Custom), or -2 when the skin folder cannot be read.
int mainui_loading_screens_current(const char *root, const struct mainui_loading_screen *items, int count);

// Checks the preset again and installs exactly its set in the skin folder: copies are staged
// and verified, the old images are put aside, the new ones renamed into place, and the old ones
// deleted. Other files are never touched. Returns 0, or -1 with the previous images in place.
int mainui_loading_screens_apply(const char *root, const struct mainui_loading_screen *items, int count, int index);

// Removes the loading-screen images, then skin, res and trimui when each is empty. Returns 0,
// or -1 with the images in place.
int mainui_loading_screens_restore_default(const char *root);

// A nonempty root maps device paths to a host fixture, as for the boot visuals.

#ifdef MAINUI_LOADING_SCREENS_FAULTS
// Host tests only: a nonzero return fails that step for that file (0 is the background).
enum mainui_loading_step {
    MAINUI_LOADING_COPY, MAINUI_LOADING_COMPARE, MAINUI_LOADING_PUT_ASIDE, MAINUI_LOADING_SWAP,
};
extern int (*mainui_loading_screens_fault)(enum mainui_loading_step step, int file);
#endif

#endif
