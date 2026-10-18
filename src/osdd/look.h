#ifndef BRICKOS_OSDD_LOOK_H
#define BRICKOS_OSDD_LOOK_H

#include <string.h>

// The BrickOS look while the BrickOS frontend is selected (docs/osdd.md): BrickOS's Appearance
// accent, background tone, card shape and focus halo, and the fonts the OSD's texts use. The
// renderer builds its palette from seed and background with brickos_palette_build, as MainUI
// does, and its corners with mainui_appearance_shape_corner.
struct osdd_look {
    unsigned char seed[3];  // the accent's seed
    double background;      // 1 Dark, 0.55 Darker, 0 Black
    int shape, halo;        // enum mainui_shape, enum mainui_halo
    char font[256];         // the Appearance font, or the system font for the language
    char fallback[256];     // the system font for the language: the characters font lacks
    int portuguese;         // the system language is Portuguese (pt.lang): the dialog's texts
};

// Whether two looks draw the same.
static inline int osdd_look_same(const struct osdd_look *a, const struct osdd_look *b) {
    return !memcmp(a->seed, b->seed, sizeof(a->seed)) && a->background == b->background && a->shape == b->shape &&
           a->halo == b->halo && !strcmp(a->font, b->font) && !strcmp(a->fallback, b->fallback) &&
           a->portuguese == b->portuguese;
}

#endif
