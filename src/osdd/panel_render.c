#include "panel_render.h"

#include "../mainui/model.h"

#include <SDL_image.h>
#include <SDL_ttf.h>
#include <stdio.h>
#include <string.h>

struct panel_tile {
    int x, y;
    const char *background;
    const char *selected;
};

static const struct panel_tile tiles[OSDD_PANEL_FOCUS_COUNT] = {
    {90, 106, "block1x1.png", "block1x1_sel.png"},
    {234, 106, "block1x1.png", "block1x1_sel.png"},
    {378, 106, "block4x2.png", "block4x2_sel.png"},
    {90, 250, "block1x1.png", "block1x1_sel.png"},
    {234, 250, "block1x1.png", "block1x1_sel.png"},
    {90, 394, "block4x1.png", "block4x1_sel.png"},
    {666, 394, "block2x1.png", "block2x1_sel.png"},
    {90, 538, "block4x1.png", "block4x1_sel.png"},
    {666, 538, "block2x1.png", "block2x1_sel.png"},
};

// block4x2_sel.png, 544x264, is the largest image drawn at the music tile's origin.
SDL_Rect osdd_panel_music_rect(void) {
    SDL_Rect rect = {.x = tiles[OSDD_PANEL_MUSIC].x, .y = tiles[OSDD_PANEL_MUSIC].y, .w = 544, .h = 264};
    return rect;
}

static int path_to(char *path, size_t capacity, const char *root, const char *name) {
    int length = snprintf(path, capacity, "%s/%s", root, name);
    return length > 0 && (size_t)length < capacity ? 0 : -1;
}

static int blit_image(struct osdd_renderer *renderer, const char *root, const char *name,
                      int x, int y, char *error, size_t error_size) {
    char path[512];
    if (path_to(path, sizeof(path), root, name) < 0) {
        snprintf(error, error_size, "panel asset path too long");
        return -1;
    }
    SDL_Surface *asset = osdd_renderer_asset(renderer, path, error, error_size);
    if (asset == NULL) return -1;
    SDL_Rect target = {.x = x, .y = y};
    int result = SDL_BlitSurface(asset, NULL, renderer->frame, &target);
    if (result < 0) snprintf(error, error_size, "panel blit: %s", SDL_GetError());
    return result;
}

static int blit_text(struct osdd_renderer *renderer, const char *root, const char *text,
                     int x, int y, char *error, size_t error_size) {
    if (renderer->panel_font == NULL) {
        char path[512];
        if (path_to(path, sizeof(path), root, "regular.ttf") < 0) {
            snprintf(error, error_size, "panel font path too long");
            return -1;
        }
        renderer->panel_font = TTF_OpenFont(path, 32);
        if (renderer->panel_font == NULL) {
            snprintf(error, error_size, "panel font: %s", TTF_GetError());
            return -1;
        }
    }
    SDL_Color color = {255, 255, 255, 255};
    SDL_Surface *glyph = TTF_RenderUTF8_Blended(renderer->panel_font, text, color);
    if (glyph == NULL) {
        snprintf(error, error_size, "panel text: %s", TTF_GetError());
        return -1;
    }
    SDL_Rect target = {.x = x, .y = y};
    int result = SDL_BlitSurface(glyph, NULL, renderer->frame, &target);
    SDL_FreeSurface(glyph);
    if (result < 0) snprintf(error, error_size, "panel text blit: %s", SDL_GetError());
    return result;
}

// A focused slider tints its track; while it is being adjusted, the fill is mint too.
static int blit_progress(struct osdd_renderer *renderer, const char *root, int x, int y,
                         int value, int maximum, int selected, int adjusting,
                         char *error, size_t error_size) {
    const char *background = selected ? "progress_bg_sel.png" : "progress_bg.png";
    if (blit_image(renderer, root, background, x, y, error, error_size) < 0) return -1;
    char path[512];
    if (path_to(path, sizeof(path), root, adjusting ? "progress_fg_sel.png" : "progress_fg.png") < 0) {
        snprintf(error, error_size, "panel progress path too long");
        return -1;
    }
    SDL_Surface *fill = osdd_renderer_asset(renderer, path, error, error_size);
    if (fill == NULL) return -1;
    if (maximum <= 0) maximum = 1;
    if (value < 0) value = 0;
    if (value > maximum) value = maximum;
    // Stock keeps the 15-pixel rounded leading cap and distributes the
    // remaining 385 pixels across the value range.
    int width = value == 0 ? 0 : 15 + (fill->w - 15) * value / maximum;
    SDL_Rect source = {.x = 0, .y = 0, .w = width, .h = fill->h};
    SDL_Rect target = {.x = x, .y = y};
    int result = width == 0 ? 0 : SDL_BlitSurface(fill, &source, renderer->frame, &target);
    if (result < 0) snprintf(error, error_size, "panel progress blit: %s", SDL_GetError());
    return result;
}

// ---- The BrickOS look (docs/osdd.md) ------------------------------------------------------

// The stock tiles' sizes, at the same places: block1x1, block4x2, block4x1 and block2x1.
static const int tile_sizes[OSDD_PANEL_FOCUS_COUNT][2] = {
    {124, 124}, {124, 124}, {556, 268}, {124, 124}, {124, 124}, {556, 124}, {268, 124}, {556, 124}, {268, 124},
};

// A copy of a stock image in the BrickOS palette (brickos_recolor), or NULL.
static SDL_Surface *recolored(SDL_Surface *source, const struct mainui_material_palette *palette) {
    SDL_Surface *copy = SDL_ConvertSurfaceFormat(source, SDL_PIXELFORMAT_ARGB8888, 0);
    if (copy == NULL) return NULL;
    if (SDL_LockSurface(copy) < 0) {
        SDL_FreeSurface(copy);
        return NULL;
    }
    for (int y = 0; y < copy->h; ++y) {
        Uint32 *row = (Uint32 *)((Uint8 *)copy->pixels + y * copy->pitch);
        for (int x = 0; x < copy->w; ++x) row[x] = brickos_recolor(palette, row[x]);
    }
    SDL_UnlockSurface(copy);
    SDL_SetSurfaceBlendMode(copy, SDL_BLENDMODE_BLEND);
    return copy;
}

// A stock widget icon as a silhouette in one colour, keeping its alpha (an "off" icon stays dim).
static int brickos_icon(struct osdd_renderer *renderer, const char *root, const char *name, int x, int y,
                        SDL_Color color, char *error, size_t error_size) {
    char path[512];
    if (path_to(path, sizeof(path), root, name) < 0) {
        snprintf(error, error_size, "panel asset path too long");
        return -1;
    }
    SDL_Surface *icon = osdd_renderer_asset(renderer, path, error, error_size);
    if (icon == NULL) return -1;
    SDL_Surface *copy = SDL_ConvertSurfaceFormat(icon, SDL_PIXELFORMAT_ARGB8888, 0);
    if (copy == NULL || SDL_LockSurface(copy) < 0) {
        if (copy) SDL_FreeSurface(copy);
        snprintf(error, error_size, "panel icon: %s", SDL_GetError());
        return -1;
    }
    for (int row = 0; row < copy->h; ++row) {
        Uint32 *pixels = (Uint32 *)((Uint8 *)copy->pixels + row * copy->pitch);
        for (int column = 0; column < copy->w; ++column)
            pixels[column] = (pixels[column] & 0xff000000u) | (Uint32)color.r << 16 | (Uint32)color.g << 8 | color.b;
    }
    SDL_UnlockSurface(copy);
    SDL_SetSurfaceBlendMode(copy, SDL_BLENDMODE_BLEND);
    SDL_Rect target = {.x = x, .y = y};
    int result = SDL_BlitSurface(copy, NULL, renderer->frame, &target);
    SDL_FreeSurface(copy);
    return result;
}

// A slider: a rounded track, filled in the accent as the stock one fills (a 15-pixel lead,
// then the rest across the range).
static void brickos_slider(SDL_Surface *frame, const struct osdd_look *look, const struct mainui_material_palette *palette,
                           int x, int y, int value, int maximum) {
    int radius = look->shape == MAINUI_SHAPE_SQUARE ? 0 : look->shape == MAINUI_SHAPE_SUBTLE ? 4 : 15;
    osdd_look_round(frame, (SDL_Rect){x, y, 400, 30}, radius, palette->surface_variant);
    if (maximum <= 0) maximum = 1;
    if (value < 0) value = 0;
    if (value > maximum) value = maximum;
    int width = value == 0 ? 0 : 15 + (400 - 15) * value / maximum;
    if (width > 0)
        osdd_look_round(frame, (SDL_Rect){x, y, width, 30}, radius, palette->primary);
}

static int brickos_panel(struct osdd_renderer *renderer, const struct osdd_panel_state *state,
                         const struct osdd_panel_visual *values, const char *root, char *error, size_t error_size) {
    const struct osdd_look *look = values->look;
    struct mainui_material_palette palette;
    osdd_look_palette(look, &palette);
    if (osdd_renderer_clear(renderer, error, error_size) < 0) return -1;
    // The dimmed panel where the stock bg.png draws it (black at alpha 217), then a card for
    // each tile. The music picture covers most of its tile and has the card's colour after
    // recolouring, so the music tile stays in surface_container when focused too.
    SDL_Rect panel = {40, 56, 944, 656};
    SDL_Color background = palette.background;
    background.a = 217;
    osdd_look_round(renderer->frame, panel, osdd_look_corner(look, panel.w, panel.h), background);
    for (int index = 0; index < OSDD_PANEL_FOCUS_COUNT; ++index) {
        SDL_Rect tile = {tiles[index].x, tiles[index].y, tile_sizes[index][0], tile_sizes[index][1]};
        int focused = index == (int)state->focus && index != OSDD_PANEL_MUSIC;
        osdd_look_round(renderer->frame, tile, osdd_look_corner(look, tile.w, tile.h),
                        focused ? palette.primary_container : palette.surface_container);
    }
    struct icon { enum osdd_panel_focus focus; const char *path; int dx, dy; };
    const struct icon icons[] = {
        {OSDD_PANEL_LED, values->led_on ? "widgets/toggle_led/icon-led-on.png" : "widgets/toggle_led/icon-led-off.png", 22, 22},
        {OSDD_PANEL_MOTOR, values->motor_on ? "widgets/toggle_rumble/icon-vibrate-on.png" : "widgets/toggle_rumble/icon-vibrate-off.png", 22, 22},
        {OSDD_PANEL_WIFI, values->wifi_on ? "widgets/toggle_wifi/icon-wifi-on.png" : "widgets/toggle_wifi/icon-wifi-off.png", 22, 22},
        {OSDD_PANEL_BLUETOOTH, values->bluetooth_on ? "widgets/toggle_bt/icon-bluetooth-on.png" : "widgets/toggle_bt/icon-bluetooth-off.png", 22, 22},
        {OSDD_PANEL_VOLUME, "widgets/slider_volume_global/icon-volume.png", 30, 32},
        {OSDD_PANEL_CPU, "widgets/static_cpu_freq/icon-cpu-info.png", 22, 22},
        {OSDD_PANEL_BACKLIGHT, "widgets/slider_backlight/icon-brightness.png", 30, 32},
        {OSDD_PANEL_TEMPERATURE, "widgets/static_temperature/icon-temp-info.png", 22, 22},
    };
    for (size_t index = 0; index < sizeof(icons) / sizeof(icons[0]); ++index) {
        const struct icon *icon = &icons[index];
        SDL_Color color = icon->focus == state->focus ? palette.on_primary_container : palette.primary;
        if (brickos_icon(renderer, root, icon->path, tiles[icon->focus].x + icon->dx, tiles[icon->focus].y + icon->dy,
                         color, error, error_size) < 0) return -1;
    }
    // The music widget's picture, or the stock idle picture, recoloured.
    SDL_Surface *music = NULL;
    if (values->music_frame != NULL) {
        SDL_Surface *frame = SDL_CreateRGBSurfaceWithFormatFrom((void *)values->music_frame, OSDD_MUSIC_WIDTH,
                                                                OSDD_MUSIC_HEIGHT, 32, OSDD_MUSIC_STRIDE,
                                                                SDL_PIXELFORMAT_ARGB8888);
        if (frame) {
            music = recolored(frame, &palette);
            SDL_FreeSurface(frame);
        }
    } else {
        char path[512];
        SDL_Surface *idle = path_to(path, sizeof(path), root, "widgets/app_music/music_default.png") == 0 ?
                            osdd_renderer_asset(renderer, path, error, error_size) : NULL;
        if (idle == NULL) return -1;
        music = recolored(idle, &palette);
    }
    if (music == NULL) {
        snprintf(error, error_size, "music picture: %s", SDL_GetError());
        return -1;
    }
    SDL_Rect target = {.x = 378, .y = 106};
    int result = SDL_BlitSurface(music, NULL, renderer->frame, &target);
    SDL_FreeSurface(music);
    if (result < 0) {
        snprintf(error, error_size, "music blit: %s", SDL_GetError());
        return -1;
    }
    // The outlines of the tiles without the focus, after the music picture, which covers the
    // top and left edges of its tile.
    for (int index = 0; index < OSDD_PANEL_FOCUS_COUNT; ++index) {
        if (index == (int)state->focus) continue;
        SDL_Rect tile = {tiles[index].x, tiles[index].y, tile_sizes[index][0], tile_sizes[index][1]};
        osdd_look_outline(renderer->frame, tile, osdd_look_corner(look, tile.w, tile.h), 1, palette.outline);
    }
    brickos_slider(renderer->frame, look, &palette, 210, 441, values->volume, values->volume_max);
    brickos_slider(renderer->frame, look, &palette, 210, 585, values->backlight, values->backlight_max);
    // CPU and temperature, in the look's fonts at the panel's size, ended with "..." before the
    // panel's edge (the stock text is never clipped).
    const char *texts[2] = {values->cpu_text, values->temperature_text};
    const int places[2][2] = {{806, 430}, {806, 574}};
    const enum osdd_panel_focus owners[2] = {OSDD_PANEL_CPU, OSDD_PANEL_TEMPERATURE};
    for (int i = 0; i < 2; ++i) {
        if (texts[i] == NULL || !texts[i][0]) continue;
        SDL_Color ink = state->focus == owners[i] ? palette.on_primary_container : palette.on_surface;
        SDL_Surface *text = osdd_look_text(renderer, look, 32, texts[i], 168, ink);
        if (text == NULL) {
            snprintf(error, error_size, "panel text: %s", TTF_GetError());
            return -1;
        }
        SDL_Rect place = {.x = places[i][0], .y = places[i][1]};
        result = SDL_BlitSurface(text, NULL, renderer->frame, &place);
        SDL_FreeSurface(text);
        if (result < 0) return -1;
    }
    SDL_Rect focus = {tiles[state->focus].x, tiles[state->focus].y,
                      tile_sizes[state->focus][0], tile_sizes[state->focus][1]};
    osdd_look_focus(renderer->frame, focus, look, &palette);
    return 0;
}

int osdd_renderer_panel(struct osdd_renderer *renderer,
                        const struct osdd_panel_state *state,
                        const struct osdd_panel_visual *values,
                        const char *asset_root,
                        char *error, size_t error_size) {
    if (error != NULL && error_size > 0) error[0] = 0;
    if (renderer == NULL || renderer->frame == NULL || state == NULL ||
        values == NULL || asset_root == NULL || state->focus < 0 ||
        state->focus >= OSDD_PANEL_FOCUS_COUNT) {
        if (error != NULL && error_size > 0) snprintf(error, error_size, "invalid panel render input");
        return -1;
    }
    if (values->look != NULL) return brickos_panel(renderer, state, values, asset_root, error, error_size);
    if (osdd_renderer_clear(renderer, error, error_size) < 0 ||
        blit_image(renderer, asset_root, "bg.png", 0, 0, error, error_size) < 0)
        return -1;
    for (int index = 0; index < OSDD_PANEL_FOCUS_COUNT; ++index) {
        if (blit_image(renderer, asset_root, tiles[index].background,
                       tiles[index].x, tiles[index].y, error, error_size) < 0) return -1;
    }
    struct icon { enum osdd_panel_focus focus; const char *path; int dx, dy; };
    const struct icon icons[] = {
        {OSDD_PANEL_LED, values->led_on ? "widgets/toggle_led/icon-led-on.png" : "widgets/toggle_led/icon-led-off.png", 22, 22},
        {OSDD_PANEL_MOTOR, values->motor_on ? "widgets/toggle_rumble/icon-vibrate-on.png" : "widgets/toggle_rumble/icon-vibrate-off.png", 22, 22},
        {OSDD_PANEL_WIFI, values->wifi_on ? "widgets/toggle_wifi/icon-wifi-on.png" : "widgets/toggle_wifi/icon-wifi-off.png", 22, 22},
        {OSDD_PANEL_BLUETOOTH, values->bluetooth_on ? "widgets/toggle_bt/icon-bluetooth-on.png" : "widgets/toggle_bt/icon-bluetooth-off.png", 22, 22},
        {OSDD_PANEL_VOLUME, "widgets/slider_volume_global/icon-volume.png", 30, 32},
        {OSDD_PANEL_CPU, "widgets/static_cpu_freq/icon-cpu-info.png", 22, 22},
        {OSDD_PANEL_BACKLIGHT, "widgets/slider_backlight/icon-brightness.png", 30, 32},
        {OSDD_PANEL_TEMPERATURE, "widgets/static_temperature/icon-temp-info.png", 22, 22},
    };
    for (size_t index = 0; index < sizeof(icons) / sizeof(icons[0]); ++index) {
        const struct icon *icon = &icons[index];
        if (blit_image(renderer, asset_root, icon->path,
                       tiles[icon->focus].x + icon->dx,
                       tiles[icon->focus].y + icon->dy, error, error_size) < 0) return -1;
    }
    if (values->music_frame != NULL) {
        SDL_Surface *music = SDL_CreateRGBSurfaceWithFormatFrom((void *)values->music_frame,
                            OSDD_MUSIC_WIDTH, OSDD_MUSIC_HEIGHT, 32,
                            OSDD_MUSIC_STRIDE, SDL_PIXELFORMAT_ARGB8888);
        if (music == NULL) {
            snprintf(error, error_size, "music frame surface: %s", SDL_GetError());
            return -1;
        }
        SDL_SetSurfaceBlendMode(music, SDL_BLENDMODE_BLEND);
        SDL_Rect target = {.x = 378, .y = 106};
        int result = SDL_BlitSurface(music, NULL, renderer->frame, &target);
        SDL_FreeSurface(music);
        if (result < 0) {
            snprintf(error, error_size, "music frame blit: %s", SDL_GetError());
            return -1;
        }
    } else if (blit_image(renderer, asset_root,
                          "widgets/app_music/music_default.png", 378, 106,
                          error, error_size) < 0) return -1;
    int adjusting = state->mode == OSDD_PANEL_SLIDER_CONTROL;
    if (blit_progress(renderer, asset_root, 210, 441,
                      values->volume, values->volume_max,
                      state->focus == OSDD_PANEL_VOLUME,
                      adjusting && state->focus == OSDD_PANEL_VOLUME,
                      error, error_size) < 0 ||
        blit_progress(renderer, asset_root, 210, 585,
                      values->backlight, values->backlight_max,
                      state->focus == OSDD_PANEL_BACKLIGHT,
                      adjusting && state->focus == OSDD_PANEL_BACKLIGHT,
                      error, error_size) < 0) return -1;
    if (blit_text(renderer, asset_root,
                  values->cpu_text == NULL ? "" : values->cpu_text,
                  806, 430, error, error_size) < 0 ||
        blit_text(renderer, asset_root,
                  values->temperature_text == NULL ? "" : values->temperature_text,
                  806, 574, error, error_size) < 0) return -1;
    if (blit_image(renderer, asset_root, tiles[state->focus].selected,
                   tiles[state->focus].x, tiles[state->focus].y,
                   error, error_size) < 0) return -1;
    return 0;
}
