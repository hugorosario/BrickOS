#ifndef BRICKOS_MATERIAL_PALETTE_H
#define BRICKOS_MATERIAL_PALETTE_H

#include <SDL.h>
#include <math.h>

// Shared tonal palette: keep frontend and OSD colors in step.
struct mainui_material_palette {
    SDL_Color background, surface, surface_container, surface_variant;
    SDL_Color primary, primary_container, on_primary, on_primary_container;
    SDL_Color on_surface, on_surface_variant, outline, disabled;
};

static inline SDL_Color material_tone(SDL_Color seed, double lightness, double saturation) {
    double red = seed.r / 255.0, green = seed.g / 255.0, blue = seed.b / 255.0;
    double maximum = fmax(red, fmax(green, blue)), minimum = fmin(red, fmin(green, blue));
    double delta = maximum - minimum, hue = 0.0;
    if (delta > 0.0001) {
        if (maximum == red) hue = 60.0 * fmod((green - blue) / delta, 6.0);
        else if (maximum == green) hue = 60.0 * ((blue - red) / delta + 2.0);
        else hue = 60.0 * ((red - green) / delta + 4.0);
        if (hue < 0.0) hue += 360.0;
    }
    double chroma = (1.0 - fabs(2.0 * lightness - 1.0)) * saturation;
    double sector = hue / 60.0;
    double x = chroma * (1.0 - fabs(fmod(sector, 2.0) - 1.0));
    double rr = 0.0, gg = 0.0, bb = 0.0;
    if (sector < 1.0) { rr = chroma; gg = x; }
    else if (sector < 2.0) { rr = x; gg = chroma; }
    else if (sector < 3.0) { gg = chroma; bb = x; }
    else if (sector < 4.0) { gg = x; bb = chroma; }
    else if (sector < 5.0) { rr = x; bb = chroma; }
    else { rr = chroma; bb = x; }
    double offset = lightness - chroma / 2.0;
    return (SDL_Color){(Uint8)lround((rr + offset) * 255.0),
                       (Uint8)lround((gg + offset) * 255.0),
                       (Uint8)lround((bb + offset) * 255.0), 255};
}

static inline double material_luminance(SDL_Color color) {
    double channels[3] = {color.r / 255.0, color.g / 255.0, color.b / 255.0};
    for (int i = 0; i < 3; ++i)
        channels[i] = channels[i] <= 0.04045 ? channels[i] / 12.92 : pow((channels[i] + 0.055) / 1.055, 2.4);
    return 0.2126 * channels[0] + 0.7152 * channels[1] + 0.0722 * channels[2];
}

static inline double material_contrast(SDL_Color foreground, SDL_Color background) {
    double a = material_luminance(foreground), b = material_luminance(background);
    return (fmax(a, b) + 0.05) / (fmin(a, b) + 0.05);
}

static inline int material_seed_valid(SDL_Color seed) {
    Uint8 minimum = seed.r < seed.g ? seed.r : seed.g;
    if (seed.b < minimum) minimum = seed.b;
    Uint8 maximum = seed.r > seed.g ? seed.r : seed.g;
    if (seed.b > maximum) maximum = seed.b;
    return seed.a >= 128 && maximum - minimum >= 8 && maximum >= 18 && minimum <= 248;
}

static inline SDL_Color material_on_color(SDL_Color background) {
    const SDL_Color black = {24, 22, 28, 255}, white = {255, 255, 255, 255};
    return material_contrast(white, background) >= material_contrast(black, background) ? white : black;
}

static inline void material_palette_build(struct mainui_material_palette *out, SDL_Color seed) {
    if (!material_seed_valid(seed))
        seed = (SDL_Color){103, 80, 164, 255};

    out->background = material_tone(seed, 0.085, 0.180);
    out->surface = material_tone(seed, 0.115, 0.165);
    out->surface_container = material_tone(seed, 0.165, 0.190);
    out->surface_variant = material_tone(seed, 0.225, 0.175);
    out->primary = material_tone(seed, 0.745, 0.580);
    out->primary_container = material_tone(seed, 0.345, 0.510);
    out->on_primary = material_on_color(out->primary);
    out->on_primary_container = material_on_color(out->primary_container);
    out->on_surface = material_tone(seed, 0.935, 0.025);
    out->on_surface_variant = material_tone(seed, 0.735, 0.060);
    out->outline = material_tone(seed, 0.485, 0.085);
    out->disabled = material_tone(seed, 0.545, 0.045);
}

#endif
