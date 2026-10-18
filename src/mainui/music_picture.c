#include "music_picture.h"

#include "../common/brickos_palette.h"

#include <SDL.h>
#include <SDL_image.h>
#include <stdio.h>
#include <unistd.h>

int mainui_music_picture_write(const char *source, const char *destination, const unsigned char seed[3], double level) {
    char temporary[1200];
    int length = snprintf(temporary, sizeof(temporary), "%s.tmp", destination);
    if (length < 0 || (size_t)length >= sizeof(temporary)) return -1;
    SDL_Surface *loaded = IMG_Load(source);
    if (!loaded) return -1;
    SDL_Surface *picture = SDL_ConvertSurfaceFormat(loaded, SDL_PIXELFORMAT_ARGB8888, 0);
    SDL_FreeSurface(loaded);
    if (!picture) return -1;
    if (SDL_LockSurface(picture) < 0) {
        SDL_FreeSurface(picture);
        return -1;
    }
    struct mainui_material_palette palette;
    brickos_palette_build(&palette, (SDL_Color){seed[0], seed[1], seed[2], 255}, level);
    for (int y = 0; y < picture->h; ++y) {
        Uint32 *row = (Uint32 *)((Uint8 *)picture->pixels + y * picture->pitch);
        for (int x = 0; x < picture->w; ++x) row[x] = brickos_recolor(&palette, row[x]);
    }
    SDL_UnlockSurface(picture);
    int result = IMG_SavePNG(picture, temporary) == 0 && rename(temporary, destination) == 0 ? 0 : -1;
    if (result < 0) unlink(temporary);
    SDL_FreeSurface(picture);
    return result;
}
