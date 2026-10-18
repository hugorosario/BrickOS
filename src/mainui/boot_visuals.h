#ifndef BRICKOS_MAINUI_BOOT_VISUALS_H
#define BRICKOS_MAINUI_BOOT_VISUALS_H

#include <stddef.h>
#include <stdint.h>

#define MAINUI_BOOT_VISUAL_NAME 256
#define MAINUI_BOOT_VISUAL_PATH 1024
// Each selector's mode, random or fixed, shared by both presentations and kept as user data
// next to the image folders.
#define MAINUI_BOOT_VISUALS_MODES "/mnt/SDCARD/BrickOS/boot-visuals.json"

enum mainui_boot_visual_kind { MAINUI_BOOTLOGO, MAINUI_SPLASHSCREEN };

struct mainui_boot_visual {
    int index;
    char name[MAINUI_BOOT_VISUAL_NAME];
    char path[MAINUI_BOOT_VISUAL_PATH];
    size_t size;
};

// Lists every valid BMP/PNG image in the matching BrickOS directory, sorted by filename.
// Names retain their filename and extension. The caller owns *items and must free it.
// Returns the count, or -1 for an invalid argument, I/O error, or path overflow.
int mainui_boot_visuals_catalog(const char *root, enum mainui_boot_visual_kind kind,
                                struct mainui_boot_visual **items);

// Compares the active destination with the listed assets. Returns its catalog index,
// -1 for a custom/missing bitmap, or -2 when the destination cannot be read.
int mainui_boot_visuals_current(const char *root, enum mainui_boot_visual_kind kind,
                                const struct mainui_boot_visual *items, int count);

// Copies one listed bitmap to the firmware destination after revalidating and verifying it.
// Returns 0 on success and -1 on any failure. A nonempty root maps device paths to a host fixture
// and skips mount operations; an empty root is the actual device.
int mainui_boot_visuals_apply(const char *root, enum mainui_boot_visual_kind kind,
                              const struct mainui_boot_visual *items, int count, int index);

// The catalog index a random write takes: uniform over the listed images but the applied one
// (active, as mainui_boot_visuals_current returns it; -1 and -2 leave none out), chosen by
// random. Returns -1 when there is nothing to write: no images, or only the applied one.
int mainui_boot_visuals_pick(const struct mainui_boot_visual *items, int count, int active, uint32_t random);

// Reads which selectors are random into random[MAINUI_BOOTLOGO] and random[MAINUI_SPLASHSCREEN]
// (1 random, 0 fixed). A missing file reads as fixed and returns 0; an unreadable or invalid one
// also reads as fixed and returns -1.
int mainui_boot_visuals_modes_load(const char *root, int random[2]);
// Writes both modes atomically (temporary file, rename, sync), keeping the members it does not
// use. Returns 0, or -1 with the file unchanged.
int mainui_boot_visuals_modes_save(const char *root, const int random[2]);

#endif
