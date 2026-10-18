#ifndef BRICKOS_PALETTE_H
#define BRICKOS_PALETTE_H

#include "material_palette.h"

// BrickOS's palette: the shared tonal palette grown from the Appearance accent seed, with the
// Appearance background level (1 Dark, 0.55 Darker, 0 Black) lowering the background tones.
// MainUI and the OSD both build it here, so the same choices give the same colours. Black makes
// the background pure black; surfaces and containers keep a small lift, so panels, rows and
// dialogs stay apart from it. material_palette_build alone keeps the tones of Dark, which the
// OSD's Stock++ toasts and the in-game menu use.
static inline void brickos_palette_build(struct mainui_material_palette *out, SDL_Color seed, double level) {
    if (!material_seed_valid(seed)) seed = (SDL_Color){103, 80, 164, 255};
    material_palette_build(out, seed);
    if (level >= 1) return;
    if (level < 0) level = 0;
    out->background = level > 0 ? material_tone(seed, 0.085 * level, 0.180) : (SDL_Color){0, 0, 0, 255};
    double lift = 0.6 + 0.4 * level;
    out->surface = material_tone(seed, 0.115 * lift, 0.165);
    out->surface_container = material_tone(seed, 0.165 * lift, 0.190);
    out->surface_variant = material_tone(seed, 0.225 * lift, 0.175);
}

// One pixel (0xAARRGGBB) of the stock OSD art in the BrickOS palette, its alpha kept. The stock
// art is neutral greys and whites with one accent, the stock green. Neutral pixels (a spread of
// at most 24 between their channels) follow their luminance along the palette's ramp: the
// background at 0, surface_container at 48 (the stock cards), surface_variant at 96,
// on_surface_variant at 200 and on_surface at 255. Stock-green pixels become the accent at
// their brightness. Every other colour, such as an album cover or the favourite heart, is kept.
static inline Uint32 brickos_recolor(const struct mainui_material_palette *palette, Uint32 argb) {
    int r = (int)(argb >> 16 & 255), g = (int)(argb >> 8 & 255), b = (int)(argb & 255);
    int high = r > g ? (r > b ? r : b) : (g > b ? g : b), low = r < g ? (r < b ? r : b) : (g < b ? g : b);
    SDL_Color out;
    if (high - low <= 24) {
        static const int stops[5] = {0, 48, 96, 200, 255};
        const SDL_Color tones[5] = {palette->background, palette->surface_container, palette->surface_variant,
                                    palette->on_surface_variant, palette->on_surface};
        int y = (r * 299 + g * 587 + b * 114) / 1000, i = 0;
        while (i < 3 && y > stops[i + 1]) ++i;
        double t = (double)(y - stops[i]) / (stops[i + 1] - stops[i]);
        if (t < 0) t = 0;
        if (t > 1) t = 1;
        out.r = (Uint8)lround(tones[i].r + (tones[i + 1].r - tones[i].r) * t);
        out.g = (Uint8)lround(tones[i].g + (tones[i + 1].g - tones[i].g) * t);
        out.b = (Uint8)lround(tones[i].b + (tones[i + 1].b - tones[i].b) * t);
    } else if (g == high && high - low > 96 && b > r) {
        // The stock green (0, 255, 163): hue about 158 degrees, from 140 to 175.
        double hue = 60.0 * ((double)(b - r) / (high - low) + 2.0);
        if (hue < 140 || hue > 175) return argb;
        out.r = (Uint8)(palette->primary.r * high / 255);
        out.g = (Uint8)(palette->primary.g * high / 255);
        out.b = (Uint8)(palette->primary.b * high / 255);
    } else {
        return argb;
    }
    return (argb & 0xff000000u) | (Uint32)out.r << 16 | (Uint32)out.g << 8 | out.b;
}

#endif
