#include "render.h"
#include "display.h"
#include "style.h"
#include "../mainui/appearance.h"

#include <SDL_image.h>
#include <SDL_ttf.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fail(char *error, size_t length, const char *message) {
    if (error != NULL && length > 0) snprintf(error, length, "%s", message);
    return -1;
}

int osdd_renderer_open(struct osdd_renderer *renderer, char *error, size_t error_size) {
    if (renderer == NULL) return fail(error, error_size, "missing renderer");
    memset(renderer, 0, sizeof(*renderer));
    if (SDL_Init(0) < 0 || (IMG_Init(IMG_INIT_PNG) & IMG_INIT_PNG) == 0 ||
        TTF_Init() < 0) return fail(error, error_size, "SDL2 image/font init failed");
    renderer->frame = SDL_CreateRGBSurfaceWithFormat(0, OSDD_WIDTH, OSDD_HEIGHT,
                                                    32, SDL_PIXELFORMAT_ARGB8888);
    if (renderer->frame == NULL || renderer->frame->pitch != OSDD_STRIDE) {
        osdd_renderer_close(renderer);
        return fail(error, error_size, "cannot allocate ARGB8888 OSD surface");
    }
    return osdd_renderer_clear(renderer, error, error_size);
}

SDL_Surface *osdd_renderer_asset(struct osdd_renderer *renderer, const char *path,
                                 char *error, size_t error_size) {
    if (renderer == NULL || path == NULL) {
        fail(error, error_size, "missing asset");
        return NULL;
    }
    for (size_t index = 0; index < renderer->asset_count; ++index)
        if (strcmp(renderer->assets[index].path, path) == 0) return renderer->assets[index].surface;
    if (strlen(path) >= sizeof(renderer->assets[0].path)) {
        fail(error, error_size, "asset path too long");
        return NULL;
    }
    SDL_Surface *surface = IMG_Load(path);
    if (surface == NULL) {
        if (error != NULL && error_size > 0) snprintf(error, error_size, "%s: %s", path, IMG_GetError());
        return NULL;
    }
    if (renderer->asset_count == OSDD_RENDERER_ASSETS) {
        SDL_FreeSurface(surface);
        fail(error, error_size, "asset cache full");
        return NULL;
    }
    struct osdd_renderer_asset *asset = &renderer->assets[renderer->asset_count++];
    snprintf(asset->path, sizeof(asset->path), "%s", path);
    asset->surface = surface;
    return surface;
}

int osdd_renderer_clear(struct osdd_renderer *renderer, char *error, size_t error_size) {
    if (renderer == NULL || renderer->frame == NULL)
        return fail(error, error_size, "renderer is not open");
    if (SDL_FillRect(renderer->frame, NULL, 0) < 0)
        return fail(error, error_size, SDL_GetError());
    return 0;
}

static const char *icon_path(const struct osdd_toast *toast) {
    if (toast->icon[0] != 0) return toast->icon;
    if (strcmp(toast->type, "volume") == 0) return "/usr/trimui/osd/icon_volume.png";
    if (strcmp(toast->type, "backlight") == 0) return "/usr/trimui/osd/icon_backlight.png";
    if (strcmp(toast->type, "batterylow") == 0) return "/usr/trimui/osd/icon_battery_low.png";
    if (strcmp(toast->type, "chargeron") == 0) return "/usr/trimui/osd/icon_charger_on.png";
    if (strcmp(toast->type, "error") == 0) return "/usr/trimui/osd/icon_fail.png";
    if (strcmp(toast->type, "info") == 0) return "/usr/trimui/osd/icon_info.png";
    if (strcmp(toast->type, "success") == 0) return "/usr/trimui/osd/icon_success.png";
    if (strcmp(toast->type, "warning") == 0) return "/usr/trimui/osd/icon_alert.png";
    return NULL;
}

// The profile's typeface and image, opened with the daemon's SDL_ttf and SDL_image.
static void *open_profile_font(void *context, const char *path, int size) {
    (void)context;
    return TTF_OpenFont(path, size);
}

static void close_profile_font(void *context, void *font) {
    (void)context;
    TTF_CloseFont(font);
}

static void *open_profile_image(void *context, const char *path) {
    (void)context;
    return IMG_Load(path);
}

static void close_profile_image(void *context, void *image) {
    (void)context;
    SDL_FreeSurface(image);
}

const struct osdd_profile_assets osdd_renderer_profile_assets = {
    NULL, open_profile_font, close_profile_font, open_profile_image, close_profile_image
};

// The icon as a silhouette in one color: every pixel takes the color, and its own alpha scaled by
// the color's. Returns a new surface, or NULL.
static SDL_Surface *silhouette(SDL_Surface *icon, const unsigned char color[4]) {
    SDL_Surface *copy = SDL_ConvertSurfaceFormat(icon, SDL_PIXELFORMAT_ARGB8888, 0);
    if (copy == NULL) return NULL;
    if (SDL_LockSurface(copy) < 0) {
        SDL_FreeSurface(copy);
        return NULL;
    }
    for (int y = 0; y < copy->h; ++y) {
        Uint32 *row = (Uint32 *)((Uint8 *)copy->pixels + y * copy->pitch);
        for (int x = 0; x < copy->w; ++x) {
            Uint32 alpha = (row[x] >> 24) * color[3] / 255;
            row[x] = alpha << 24 | (Uint32)color[0] << 16 | (Uint32)color[1] << 8 | color[2];
        }
    }
    SDL_UnlockSurface(copy);
    SDL_SetSurfaceBlendMode(copy, SDL_BLENDMODE_BLEND);
    return copy;
}

// Stretches an image over a rectangle, keeping its margins unscaled (a nine-patch). Margins that
// do not fit the image or the rectangle stretch the whole image.
static int nine_patch(SDL_Surface *image, const int margins[4], SDL_Surface *frame, SDL_Rect to) {
    int left = margins[0], top = margins[1], right = margins[2], bottom = margins[3];
    if (left + right >= image->w || top + bottom >= image->h || left + right > to.w || top + bottom > to.h)
        return SDL_BlitScaled(image, NULL, frame, &to);
    int sx[4] = {0, left, image->w - right, image->w}, sy[4] = {0, top, image->h - bottom, image->h};
    int dx[4] = {to.x, to.x + left, to.x + to.w - right, to.x + to.w};
    int dy[4] = {to.y, to.y + top, to.y + to.h - bottom, to.y + to.h};
    for (int row = 0; row < 3; ++row)
        for (int column = 0; column < 3; ++column) {
            SDL_Rect source = {sx[column], sy[row], sx[column + 1] - sx[column], sy[row + 1] - sy[row]};
            SDL_Rect target = {dx[column], dy[row], dx[column + 1] - dx[column], dy[row + 1] - dy[row]};
            if (source.w <= 0 || source.h <= 0 || target.w <= 0 || target.h <= 0) continue;
            if (SDL_BlitScaled(image, &source, frame, &target) < 0) return -1;
        }
    return 0;
}

// Scanline rounded surfaces retain straight alpha for the display engine's overlay blend.
static void material_round(SDL_Surface *frame, SDL_Rect area, int radius, SDL_Color color) {
    if (radius > area.h / 2) radius = area.h / 2;
    if (radius > area.w / 2) radius = area.w / 2;
    Uint32 ink = SDL_MapRGBA(frame->format, color.r, color.g, color.b, color.a);
    for (int y = 0; y < area.h; ++y) {
        double dy = y < radius ? radius - y - 0.5 : y >= area.h - radius ? y - (area.h - radius) + 0.5 : 0;
        int inset = dy > 0 ? (int)ceil(radius - sqrt(radius * radius - dy * dy)) : 0;
        SDL_Rect row = {area.x + inset, area.y + y, area.w - inset * 2, 1};
        SDL_FillRect(frame, &row, ink);
    }
}

void osdd_look_round(SDL_Surface *frame, SDL_Rect area, int radius, SDL_Color color) {
    material_round(frame, area, radius < 0 ? 0 : radius, color);
}

void osdd_look_palette(const struct osdd_look *look, struct mainui_material_palette *palette) {
    brickos_palette_build(palette, (SDL_Color){look->seed[0], look->seed[1], look->seed[2], 255}, look->background);
}

// Whether a pixel lies inside a card with rounded corners.
static int inside_round(int x, int y, SDL_Rect area, int radius) {
    if (x < area.x || y < area.y || x >= area.x + area.w || y >= area.y + area.h) return 0;
    if (radius <= 0) return 1;
    int cx = x < area.x + radius ? area.x + radius : x >= area.x + area.w - radius ? area.x + area.w - radius - 1 : x;
    int cy = y < area.y + radius ? area.y + radius : y >= area.y + area.h - radius ? area.y + area.h - radius - 1 : y;
    double dx = x - cx, dy = y - cy;
    return dx * dx + dy * dy <= (double)radius * radius;
}

// Draws color over one pixel, both with straight alpha, as the display engine composes them.
static void blend_pixel(SDL_Surface *frame, int x, int y, SDL_Color color) {
    SDL_Rect clip;
    SDL_GetClipRect(frame, &clip);
    if (x < clip.x || y < clip.y || x >= clip.x + clip.w || y >= clip.y + clip.h) return;
    Uint32 *pixel = (Uint32 *)((Uint8 *)frame->pixels + y * frame->pitch) + x;
    double sa = color.a / 255.0, da = (*pixel >> 24) / 255.0, oa = sa + da * (1 - sa);
    if (oa <= 0) return;
    double r = (color.r * sa + (*pixel >> 16 & 255) * da * (1 - sa)) / oa;
    double g = (color.g * sa + (*pixel >> 8 & 255) * da * (1 - sa)) / oa;
    double b = (color.b * sa + (*pixel & 255) * da * (1 - sa)) / oa;
    *pixel = (Uint32)lround(oa * 255) << 24 | (Uint32)lround(r) << 16 | (Uint32)lround(g) << 8 | (Uint32)lround(b);
}

void osdd_look_outline(SDL_Surface *frame, SDL_Rect area, int radius, int thickness, SDL_Color color) {
    if (thickness <= 0 || area.w <= 0 || area.h <= 0) return;
    SDL_Rect inner = {area.x + thickness, area.y + thickness, area.w - 2 * thickness, area.h - 2 * thickness};
    int inner_radius = radius - thickness > 0 ? radius - thickness : 0;
    if (SDL_MUSTLOCK(frame) && SDL_LockSurface(frame) < 0) return;
    for (int y = area.y; y < area.y + area.h; ++y) {
        // Rows away from the top and bottom bands hold the ring only near the sides.
        int band = y < area.y + thickness + radius || y >= area.y + area.h - thickness - radius;
        for (int x = area.x; x < area.x + area.w; ++x) {
            if (!band && x == area.x + thickness + radius) x = area.x + area.w - thickness - radius;
            if (inside_round(x, y, area, radius) && !inside_round(x, y, inner, inner_radius)) blend_pixel(frame, x, y, color);
        }
    }
    if (SDL_MUSTLOCK(frame)) SDL_UnlockSurface(frame);
}

void osdd_look_focus(SDL_Surface *frame, SDL_Rect area, const struct osdd_look *look,
                     const struct mainui_material_palette *palette) {
    SDL_Rect inner = {area.x + 2, area.y + 2, area.w - 4, area.h - 4};
    SDL_Color color = palette->primary;
    if (look->halo != MAINUI_HALO_OUTLINE) {
        SDL_Rect wide = {inner.x - 5, inner.y - 5, inner.w + 10, inner.h + 10};
        SDL_Rect near = {inner.x - 2, inner.y - 2, inner.w + 4, inner.h + 4};
        color.a = 34;
        osdd_look_outline(frame, wide, osdd_look_corner(look, wide.w, wide.h), 5, color);
        color.a = 72;
        osdd_look_outline(frame, near, osdd_look_corner(look, near.w, near.h), 4, color);
    }
    color.a = 255;
    osdd_look_outline(frame, inner, osdd_look_corner(look, inner.w, inner.h), 3, color);
}

int osdd_look_corner(const struct osdd_look *look, int w, int h) {
    return mainui_appearance_shape_corner(look->shape, w, h);
}

// The look's fonts stay open by path and size; a path that does not open is remembered too, so
// it is not tried again for every frame.
static TTF_Font *look_font(struct osdd_renderer *renderer, const char *path, int size) {
    if (!path || !path[0] || strlen(path) >= sizeof(renderer->look_fonts[0].path)) return NULL;
    for (int i = 0; i < OSDD_LOOK_FONTS; ++i) {
        struct osdd_look_font *entry = &renderer->look_fonts[i];
        if ((entry->font || entry->failed) && entry->size == size && !strcmp(entry->path, path)) return entry->font;
    }
    struct osdd_look_font *entry = &renderer->look_fonts[renderer->look_font_next];
    renderer->look_font_next = (renderer->look_font_next + 1) % OSDD_LOOK_FONTS;
    if (entry->font) TTF_CloseFont(entry->font);
    snprintf(entry->path, sizeof(entry->path), "%s", path);
    entry->size = size;
    entry->font = TTF_OpenFont(path, size);
    entry->failed = entry->font == NULL;
    return entry->font;
}

static int next_codepoint(const unsigned char **cursor, unsigned int *codepoint) {
    const unsigned char *p = *cursor;
    unsigned int value;
    int bytes;
    if (*p < 0x80) { value = *p; bytes = 1; }
    else if (*p >= 0xc2 && *p <= 0xdf) { value = *p & 0x1f; bytes = 2; }
    else if (*p >= 0xe0 && *p <= 0xef) { value = *p & 0x0f; bytes = 3; }
    else if (*p >= 0xf0 && *p <= 0xf4) { value = *p & 0x07; bytes = 4; }
    else return 0;
    for (int i = 1; i < bytes; ++i) {
        if ((p[i] & 0xc0) != 0x80) return 0;
        value = (value << 6) | (p[i] & 0x3f);
    }
    *cursor = p + bytes;
    *codepoint = value;
    return 1;
}

// Whether font draws the character itself; one it cannot test (beyond the Basic Multilingual
// Plane, or invalid UTF-8) stays with font.
static int own_glyph(TTF_Font *font, TTF_Font *fallback, unsigned int codepoint) {
    return codepoint > 0xffff || TTF_GlyphIsProvided(font, (Uint16)codepoint) || !TTF_GlyphIsProvided(fallback, (Uint16)codepoint);
}

// The next run of text that one font draws, copied into out (at most size - 1 bytes). Returns its
// length in text.
static size_t look_run(TTF_Font *font, TTF_Font *fallback, const char *text, char *out, size_t size, TTF_Font **run_font) {
    const unsigned char *p = (const unsigned char *)text;
    int own = -1;
    while (*p) {
        const unsigned char *start = p;
        unsigned int codepoint = '?';
        if (!next_codepoint(&p, &codepoint)) p = start + 1;
        int mine = !fallback || own_glyph(font, fallback, codepoint);
        if ((own >= 0 && mine != own) || (size_t)(p - (const unsigned char *)text) >= size) {
            p = start;
            break;
        }
        own = mine;
    }
    size_t length = (size_t)(p - (const unsigned char *)text);
    memcpy(out, text, length);
    out[length] = 0;
    *run_font = own != 0 ? font : fallback;
    return length;
}

static int look_width(TTF_Font *font, TTF_Font *fallback, const char *text) {
    char run[512];
    int total = 0;
    TTF_Font *run_font;
    for (size_t n; *text; text += n) {
        int w = 0;
        n = look_run(font, fallback, text, run, sizeof(run), &run_font);
        if (!n) break;
        if (TTF_SizeUTF8(run_font, run, &w, NULL) == 0) total += w;
    }
    return total;
}

SDL_Surface *osdd_look_text(struct osdd_renderer *renderer, const struct osdd_look *look, int size,
                            const char *message, int width, SDL_Color color) {
    if (!renderer || !look || !message || !message[0]) return NULL;
    TTF_Font *fallback = look_font(renderer, look->fallback, size);
    if (!fallback) fallback = look_font(renderer, OSDD_PROFILE_STOCK_FONT, size);
    TTF_Font *font = look_font(renderer, look->font, size);
    if (!font) font = fallback;
    if (!font) return NULL;
    if (fallback == font) fallback = NULL;
    char text[sizeof(((struct osdd_toast *)0)->message) + 4];
    snprintf(text, sizeof(text), "%s", message);
    size_t n = strlen(text);
    if (look_width(font, fallback, text) > width) {
        while (n) {
            --n;
            while (n && ((unsigned char)text[n] & 0xc0) == 0x80) --n;
            memcpy(text + n, "...", 4);
            if (look_width(font, fallback, text) <= width) break;
        }
    }
    // One font for the whole text: drawn at once.
    char run[512];
    TTF_Font *run_font;
    size_t first = look_run(font, fallback, text, run, sizeof(run), &run_font);
    if (!text[first] && first < sizeof(run) - 1) return TTF_RenderUTF8_Blended(run_font, text, color);
    // Runs in two fonts, side by side on one baseline.
    int ascent = TTF_FontAscent(font), descent = TTF_FontDescent(font);
    if (fallback && TTF_FontAscent(fallback) > ascent) ascent = TTF_FontAscent(fallback);
    if (fallback && TTF_FontDescent(fallback) < descent) descent = TTF_FontDescent(fallback);
    int total = look_width(font, fallback, text);
    SDL_Surface *out = SDL_CreateRGBSurfaceWithFormat(0, total > 0 ? total : 1, ascent - descent, 32, SDL_PIXELFORMAT_ARGB8888);
    if (!out) return NULL;
    SDL_FillRect(out, NULL, 0);
    int x = 0;
    for (const char *p = text; *p;) {
        size_t length = look_run(font, fallback, p, run, sizeof(run), &run_font);
        if (!length) break;
        p += length;
        SDL_Surface *piece = TTF_RenderUTF8_Blended(run_font, run, color);
        if (!piece) continue;
        // Runs never overlap, so each is copied as it is, alpha included.
        SDL_SetSurfaceBlendMode(piece, SDL_BLENDMODE_NONE);
        SDL_Rect to = {x, ascent - TTF_FontAscent(run_font), piece->w, piece->h};
        SDL_BlitSurface(piece, NULL, out, &to);
        x += piece->w;
        SDL_FreeSurface(piece);
    }
    SDL_SetSurfaceBlendMode(out, SDL_BLENDMODE_BLEND);
    return out;
}

static void material_line(SDL_Surface *frame, int x1, int y1, int x2, int y2, SDL_Color color) {
    int dx = x2 - x1, dy = y2 - y1;
    int steps = abs(dx) > abs(dy) ? abs(dx) : abs(dy);
    for (int step = 0; step <= steps; ++step) {
        int x = steps ? x1 + dx * step / steps : x1;
        int y = steps ? y1 + dy * step / steps : y1;
        material_round(frame, (SDL_Rect){x - 1, y - 1, 3, 3}, 1, color);
    }
}

static void material_ring(SDL_Surface *frame, int cx, int cy, int radius, SDL_Color color, SDL_Color base) {
    material_round(frame, (SDL_Rect){cx - radius, cy - radius, radius * 2, radius * 2}, radius, color);
    material_round(frame, (SDL_Rect){cx - radius + 3, cy - radius + 3, radius * 2 - 6, radius * 2 - 6}, radius - 3, base);
}

// Stock icons can contain an opaque colored disc with a white mark. Tinting that disc loses
// the mark, so built-in toast types use small vector symbols; explicit producer icons keep
// their own alpha shape.
static void material_symbol(SDL_Surface *frame, const char *type, SDL_Rect badge, SDL_Color color, SDL_Color base) {
    int cx = badge.x + 24, cy = badge.y + 24;
#define LINE(x1, y1, x2, y2) material_line(frame, cx + (x1), cy + (y1), cx + (x2), cy + (y2), color)
    if (!strcmp(type, "volume")) {
        LINE(-14, -5, -8, -5); LINE(-8, -5, -1, -12); LINE(-1, -12, -1, 12);
        LINE(-1, 12, -8, 5); LINE(-8, 5, -14, 5); LINE(-14, 5, -14, -5);
        LINE(5, -6, 8, -3); LINE(8, -3, 8, 3); LINE(8, 3, 5, 6);
        LINE(10, -12, 14, -7); LINE(14, -7, 16, 0); LINE(16, 0, 14, 7); LINE(14, 7, 10, 12);
    } else if (!strcmp(type, "backlight")) {
        material_ring(frame, cx, cy, 8, color, base);
        for (int i = 0; i < 8; ++i) {
            double angle = i * 3.141592653589793 / 4;
            LINE((int)lround(cos(angle) * 12), (int)lround(sin(angle) * 12),
                 (int)lround(cos(angle) * 16), (int)lround(sin(angle) * 16));
        }
    } else if (!strcmp(type, "batterylow") || !strcmp(type, "chargeron")) {
        LINE(-11, -12, 11, -12); LINE(11, -12, 11, 12); LINE(11, 12, -11, 12); LINE(-11, 12, -11, -12);
        LINE(-4, -16, 4, -16);
        if (!strcmp(type, "chargeron")) { LINE(3, -8, -4, 1); LINE(-4, 1, 4, 1); LINE(4, 1, -3, 9); }
        else { LINE(0, -6, 0, 3); LINE(0, 8, 0, 8); }
    } else if (!strcmp(type, "warning")) {
        LINE(0, -15, 16, 13); LINE(16, 13, -16, 13); LINE(-16, 13, 0, -15);
        LINE(0, -5, 0, 3); LINE(0, 8, 0, 8);
    } else {
        material_ring(frame, cx, cy, 16, color, base);
        if (!strcmp(type, "success")) { LINE(-8, 0, -2, 6); LINE(-2, 6, 9, -6); }
        else if (!strcmp(type, "error")) { LINE(-6, -6, 6, 6); LINE(-6, 6, 6, -6); }
        else { LINE(0, -7, 0, -7); LINE(0, -1, 0, 8); }
    }
#undef LINE
}

static int material_toast(struct osdd_renderer *renderer, const struct osdd_toast *toast,
                           char *error, size_t error_size) {
    static const int widths[] = {300, 400, 600, 1000};
    if (toast->size < 0 || toast->size >= 4) return fail(error, error_size, "unsupported stock toast size");
    // The look's palette, corners and fonts, as the record snapshot them.
    struct mainui_material_palette palette;
    osdd_look_palette(&toast->look, &palette);
    SDL_Color accent = palette.primary;
    if (!strcmp(toast->type, "error") || !strcmp(toast->type, "batterylow")) accent = (SDL_Color){255, 180, 171, 255};
    else if (!strcmp(toast->type, "warning")) accent = (SDL_Color){244, 207, 129, 255};
    else if (!strcmp(toast->type, "success")) accent = (SDL_Color){159, 218, 175, 255};
    SDL_Rect card = {toast->x, toast->y, widths[toast->size], 80};
    SDL_Rect old_clip, clip;
    SDL_GetClipRect(renderer->frame, &old_clip);
    if (!SDL_IntersectRect(&old_clip, &card, &clip)) return 0;
    SDL_SetClipRect(renderer->frame, &clip);
    int radius = osdd_look_corner(&toast->look, card.w, card.h);
    material_round(renderer->frame, card, radius, palette.outline);
    SDL_Rect inner = {card.x + 1, card.y + 1, card.w - 2, card.h - 2};
    material_round(renderer->frame, inner, radius > 0 ? radius - 1 : 0, palette.surface_container);
    int text_x = card.x + 24;
    const char *file = icon_path(toast);
    if (file) {
        SDL_Rect badge = {card.x + 16, card.y + 16, 48, 48};
        material_round(renderer->frame, badge, osdd_look_corner(&toast->look, badge.w, badge.h), palette.primary_container);
        SDL_Surface *source = toast->icon[0] ? IMG_Load(file) : NULL;
        unsigned char color[4] = {accent.r, accent.g, accent.b, accent.a};
        SDL_Surface *icon = source ? silhouette(source, color) : NULL;
        if (source) SDL_FreeSurface(source);
        if (icon) {
            int width = 34, height = 34;
            if (icon->w > icon->h) height = icon->h * width / icon->w;
            else width = icon->w * height / icon->h;
            SDL_Rect target = {badge.x + (48 - width) / 2, badge.y + (48 - height) / 2, width, height};
            SDL_BlitScaled(icon, NULL, renderer->frame, &target);
            SDL_FreeSurface(icon);
        } else material_symbol(renderer->frame, toast->type, badge, accent, palette.primary_container);
        text_x = card.x + 80;
    }
    int result = 0;
    if (toast->message[0]) {
        SDL_Surface *text = osdd_look_text(renderer, &toast->look, 32, toast->message, card.x + card.w - 24 - text_x,
                                           palette.on_surface);
        if (!text) result = -1;
        else {
            SDL_Rect target = {text_x, card.y + (card.h - text->h) / 2, text->w, text->h};
            result = SDL_BlitSurface(text, NULL, renderer->frame, &target);
            SDL_FreeSurface(text);
        }
    }
    SDL_SetClipRect(renderer->frame, &old_clip);
    if (result < 0) return fail(error, error_size, TTF_GetError());
    return 0;
}

int osdd_renderer_toast(struct osdd_renderer *renderer,
                        const struct osdd_toast *toast, char *error, size_t error_size) {
    if (renderer == NULL || renderer->frame == NULL || toast == NULL)
        return fail(error, error_size, "invalid toast renderer input");
    // An explicit background is image content (for example a theme preview), kept whole.
    if (toast->material && !toast->bg[0]) return material_toast(renderer, toast, error, error_size);
    struct osdd_toast_style style;
    if (osdd_toast_style(toast, &style, error, error_size) < 0) return -1;
    // Without a profile the stock background is loaded for each toast and drawn; with one it
    // only gives the toast its stock size, so the decoded image is kept.
    SDL_Surface *bg = style.themed ? osdd_renderer_asset(renderer, style.stock_background, error, error_size)
                                   : IMG_Load(style.stock_background);
    if (bg == NULL) return style.themed ? -1 : fail(error, error_size, IMG_GetError());
    // A background that runs past the frame is clipped at its edge, as the official
    // daemon draws MainUI's 869-pixel theme preview at x=690. The blits clip.

    const char *icon_file = icon_path(toast);
    SDL_Surface *icon = icon_file == NULL ? NULL : IMG_Load(icon_file);
    if (icon_file != NULL && icon == NULL) {
        if (!style.themed) SDL_FreeSurface(bg);
        return fail(error, error_size, IMG_GetError());
    }
    if (icon != NULL && style.tint_icon) {
        SDL_Surface *tinted = silhouette(icon, style.icon_color);
        SDL_FreeSurface(icon);
        icon = tinted;
        if (icon == NULL) return fail(error, error_size, SDL_GetError());
    }
    TTF_Font *font = style.themed ? style.font : TTF_OpenFont(style.font_path, style.font_px);
    if (font == NULL) {
        if (icon != NULL) SDL_FreeSurface(icon);
        if (!style.themed) SDL_FreeSurface(bg);
        return fail(error, error_size, TTF_GetError());
    }
    SDL_Color color = {style.color[0], style.color[1], style.color[2], style.color[3]};
    SDL_Surface *text = TTF_RenderUTF8_Blended(font, toast->message, color);
    if (!style.themed) TTF_CloseFont(font);
    if (text == NULL) {
        if (icon != NULL) SDL_FreeSurface(icon);
        if (!style.themed) SDL_FreeSurface(bg);
        return fail(error, error_size, TTF_GetError());
    }

    SDL_Rect destination = {.x = toast->x, .y = toast->y};
    int result;
    if (!style.themed || (style.background == NULL && !style.solid)) {
        result = SDL_BlitSurface(bg, NULL, renderer->frame, &destination);
    } else if (style.background != NULL) {
        SDL_Rect area = {toast->x, toast->y, bg->w, bg->h};
        result = nine_patch(style.background, style.margins, renderer->frame, area);
    } else {
        SDL_Rect area = {toast->x, toast->y, bg->w, bg->h};
        result = SDL_FillRect(renderer->frame, &area,
                              SDL_MapRGBA(renderer->frame->format, style.solid_color[0], style.solid_color[1],
                                          style.solid_color[2], style.solid_color[3]));
    }
    if (result == 0) {
        int content_width = text->w + (icon == NULL ? 0 : icon->w);
        // Wide toasts put an icon at the left edge; without one, as for an unknown type,
        // the official daemon centers the text like the narrower sizes.
        int content_x = toast->size >= 2 && icon != NULL ? toast->x + style.padding :
                        toast->x + (bg->w - content_width) / 2;
        if (icon != NULL) {
            destination.x = content_x;
            destination.y = toast->y + (bg->h - icon->h) / 2;
            result = SDL_BlitSurface(icon, NULL, renderer->frame, &destination);
            content_x += icon->w;
        }
        if (result == 0) {
            destination.x = content_x + (icon == NULL ? 4 : 8);
            destination.y = toast->y + (bg->h - text->h) / 2;
            result = SDL_BlitSurface(text, NULL, renderer->frame, &destination);
        }
    }
    SDL_FreeSurface(text);
    if (icon != NULL) SDL_FreeSurface(icon);
    if (!style.themed) SDL_FreeSurface(bg);
    if (result < 0) return fail(error, error_size, SDL_GetError());
    return 0;
}

// The power dialog's texts, in English and Portuguese (docs/osdd.md).
struct power_texts {
    const char *title, *question, *off, *reboot, *cancel, *powering_off, *rebooting;
};
static const struct power_texts power_english = {
    "Power", "Power off or reboot?", "Power off", "Reboot", "Cancel", "Powering off...", "Rebooting..."};
static const struct power_texts power_portuguese = {
    "Energia", "Desligar ou reiniciar?", "Desligar", "Reiniciar", "Cancelar", "A desligar...", "A reiniciar..."};

// The question while it asks, then the progress of the chosen action.
static const char *power_body(const struct osdd_power_dialog *power, const struct power_texts *texts) {
    if (power->phase == OSDD_POWER_CONFIRM) return texts->question;
    return power->action == OSDD_POWER_ACTION_REBOOT ? texts->rebooting : texts->powering_off;
}

// The three buttons' places in the card's button row: equal widths, 36 pixels apart.
static void power_buttons(int left, int right, int y, SDL_Rect buttons[3]) {
    int gap = 36, width = (right - left - 2 * gap) / 3;
    for (int index = 0; index < 3; ++index) buttons[index] = (SDL_Rect){left + index * (width + gap), y, width, 68};
}

static int power_text(struct osdd_renderer *renderer, TTF_Font *font, const char *value,
                      int center_x, int y, SDL_Color color) {
    if (value == NULL || value[0] == 0) return 0;
    SDL_Surface *surface = TTF_RenderUTF8_Blended(font, value, color);
    if (surface == NULL) return -1;
    SDL_Rect target = {center_x - surface->w / 2, y, surface->w, surface->h};
    int result = SDL_BlitSurface(surface, NULL, renderer->frame, &target);
    SDL_FreeSurface(surface);
    return result;
}

static void power_color(SDL_Color *out, const unsigned char rgba[4]) {
    *out = (SDL_Color){rgba[0], rgba[1], rgba[2], rgba[3]};
}

static int power_button(struct osdd_renderer *renderer, TTF_Font *font, SDL_Rect rect,
                        const char *label, int focused, SDL_Color selector,
                        SDL_Color secondary, SDL_Color selected) {
    Uint32 fill = SDL_MapRGBA(renderer->frame->format,
                              focused ? selector.r : 24,
                              focused ? selector.g : 24,
                              focused ? selector.b : 28,
                              focused ? selector.a : 220);
    if (SDL_FillRect(renderer->frame, &rect, fill) < 0) return -1;
    if (!focused) {
        Uint32 border = SDL_MapRGBA(renderer->frame->format, secondary.r, secondary.g, secondary.b, 150);
        SDL_Rect top = {rect.x, rect.y, rect.w, 2}, bottom = {rect.x, rect.y + rect.h - 2, rect.w, 2};
        SDL_Rect left = {rect.x, rect.y, 2, rect.h}, right = {rect.x + rect.w - 2, rect.y, 2, rect.h};
        if (SDL_FillRect(renderer->frame, &top, border) < 0 ||
            SDL_FillRect(renderer->frame, &bottom, border) < 0 ||
            SDL_FillRect(renderer->frame, &left, border) < 0 ||
            SDL_FillRect(renderer->frame, &right, border) < 0) return -1;
    }
    int height = TTF_FontHeight(font);
    return power_text(renderer, font, label, rect.x + rect.w / 2,
                      rect.y + (rect.h - height) / 2, focused ? selected : secondary);
}

// top drawn over bottom, both with straight alpha, as the display engine composes them.
static SDL_Color over(SDL_Color top, SDL_Color bottom) {
    double ta = top.a / 255.0, ba = bottom.a / 255.0, oa = ta + ba * (1 - ta);
    if (oa <= 0) return (SDL_Color){0, 0, 0, 0};
    return (SDL_Color){(Uint8)lround((top.r * ta + bottom.r * ba * (1 - ta)) / oa),
                       (Uint8)lround((top.g * ta + bottom.g * ba * (1 - ta)) / oa),
                       (Uint8)lround((top.b * ta + bottom.b * ba * (1 - ta)) / oa), (Uint8)lround(oa * 255)};
}

// One line in the look's fonts, centred on center_x, ended with "..." to fit width.
static int look_centered(struct osdd_renderer *renderer, const struct osdd_look *look, const char *value,
                         int center_x, int y, int width, SDL_Color color) {
    if (value == NULL || value[0] == 0) return 0;
    SDL_Surface *surface = osdd_look_text(renderer, look, OSDD_PROFILE_DEFAULT_FONT_SIZE, value, width, color);
    if (surface == NULL) return -1;
    SDL_Rect target = {center_x - surface->w / 2, y, surface->w, surface->h};
    int result = SDL_BlitSurface(surface, NULL, renderer->frame, &target);
    SDL_FreeSurface(surface);
    return result;
}

// A dialog button as MainUI draws it: primary_container with the focus outline when focused,
// surface_container otherwise.
static int brickos_power_button(struct osdd_renderer *renderer, const struct osdd_look *look,
                                const struct mainui_material_palette *palette, SDL_Rect rect,
                                const char *label, int focused) {
    osdd_look_round(renderer->frame, rect, osdd_look_corner(look, rect.w, rect.h),
                    focused ? palette->primary_container : palette->surface_container);
    if (focused) osdd_look_focus(renderer->frame, rect, look, palette);
    SDL_Surface *surface = osdd_look_text(renderer, look, OSDD_PROFILE_DEFAULT_FONT_SIZE, label, rect.w - 8,
                                          focused ? palette->on_primary_container : palette->on_surface);
    if (surface == NULL) return -1;
    SDL_Rect target = {rect.x + (rect.w - surface->w) / 2, rect.y + (rect.h - surface->h) / 2, surface->w, surface->h};
    int result = SDL_BlitSurface(surface, NULL, renderer->frame, &target);
    SDL_FreeSurface(surface);
    return result;
}

// The BrickOS look's power-off dialog (docs/osdd.md): MainUI's scrim, panel and buttons at the
// stock dialog's places and sizes, in English or Portuguese. The theme profile takes no part.
static int brickos_power(struct osdd_renderer *renderer, const struct osdd_power_dialog *power,
                         const struct osdd_look *look, char *error, size_t error_size) {
    struct mainui_material_palette palette;
    osdd_look_palette(look, &palette);
    SDL_Surface *frame = renderer->frame;
    SDL_Color scrim = {3, 6, 14, 184};
    SDL_Rect fullscreen = {0, 0, frame->w, frame->h};
    if (SDL_FillRect(frame, &fullscreen, SDL_MapRGBA(frame->format, scrim.r, scrim.g, scrim.b, scrim.a)) < 0)
        return fail(error, error_size, SDL_GetError());
    // MainUI's panel: its shadow, then surface_container with a 2-pixel outline.
    SDL_Rect card = {(frame->w - 844) / 2, (frame->h - 428) / 2, 844, 428};
    int radius = osdd_look_corner(look, card.w, card.h);
    osdd_look_round(frame, (SDL_Rect){card.x + 5, card.y + 8, card.w, card.h}, radius,
                    over((SDL_Color){0, 0, 0, 96}, scrim));
    osdd_look_round(frame, card, radius, palette.surface_container);
    osdd_look_outline(frame, card, radius, 2, palette.outline);
    const struct power_texts *texts = look->portuguese ? &power_portuguese : &power_english;
    int center = frame->w / 2, padding = OSDD_PROFILE_DEFAULT_PADDING;
    int width = card.w - 2 * padding;
    if (look_centered(renderer, look, texts->title, center, card.y + 48, width, palette.on_surface) < 0 ||
        look_centered(renderer, look, power_body(power, texts), center, card.y + 142, width,
                      palette.on_surface_variant) < 0 ||
        look_centered(renderer, look, power->diagnostic, center, card.y + 202, width,
                      (SDL_Color){255, 180, 171, 255}) < 0)
        return fail(error, error_size, TTF_GetError());
    if (power->phase == OSDD_POWER_CONFIRM) {
        SDL_Rect buttons[3];
        power_buttons(card.x + padding, card.x + card.w - padding, card.y + 302, buttons);
        const char *labels[3] = {texts->off, texts->reboot, texts->cancel};
        for (int index = 0; index < 3; ++index)
            if (brickos_power_button(renderer, look, &palette, buttons[index], labels[index],
                                     (int)power->focus == index) < 0)
                return fail(error, error_size, TTF_GetError());
    }
    return 0;
}

int osdd_renderer_power(struct osdd_renderer *renderer,
                        const struct osdd_power_dialog *power,
                        const struct osdd_profile *profile,
                        const struct osdd_look *look, int portuguese,
                        char *error, size_t error_size) {
    if (power == NULL || power->phase == OSDD_POWER_CLOSED) return 0;
    if (renderer == NULL || renderer->frame == NULL)
        return fail(error, error_size, "renderer is not open");
    if (look != NULL) return brickos_power(renderer, power, look, error, error_size);

    SDL_Color white = {255, 255, 255, 255};
    SDL_Color title = white, message = {210, 214, 218, 255};
    SDL_Color selector = {20, 105, 82, 255};
    SDL_Color secondary = {185, 190, 195, 255}, selected = white;
    int font_size = profile != NULL ? profile->font_size : OSDD_PROFILE_DEFAULT_FONT_SIZE;
    TTF_Font *font = profile != NULL ? profile->font : NULL;
    int opened_font = 0;
    if (font == NULL) {
        const char *path = profile != NULL && profile->font_path[0] != 0
                         ? profile->font_path : OSDD_PROFILE_STOCK_FONT;
        font = TTF_OpenFont(path, font_size);
        if (font == NULL) return fail(error, error_size, TTF_GetError());
        opened_font = 1;
    }
    if (profile != NULL) {
        power_color(&message, profile->font_color);
        if (profile->has_selector_color) power_color(&selector, profile->selector_color);
        if (profile->has_secondary_color) power_color(&secondary, profile->secondary_color);
        if (profile->has_selected_color) power_color(&selected, profile->selected_color);
        // The title sits on the profile's background, as the message does; the selected color
        // is for the focused label over the selector.
        title = message;
    }

    SDL_Rect fullscreen = {0, 0, renderer->frame->w, renderer->frame->h};
    Uint32 dim = SDL_MapRGBA(renderer->frame->format, 0, 0, 0, 150);
    int result = SDL_FillRect(renderer->frame, &fullscreen, dim);
    SDL_Rect card = {(renderer->frame->w - 844) / 2, (renderer->frame->h - 428) / 2, 844, 428};
    if (result == 0 && profile != NULL && profile->background != NULL) {
        result = nine_patch(profile->background, profile->margins, renderer->frame, card);
    } else if (result == 0 && profile != NULL && profile->has_background_color) {
        Uint32 background = SDL_MapRGBA(renderer->frame->format,
                                        profile->background_color[0], profile->background_color[1],
                                        profile->background_color[2], profile->background_color[3]);
        result = SDL_FillRect(renderer->frame, &card, background);
    } else if (result == 0) {
        Uint32 background = SDL_MapRGBA(renderer->frame->format, 8, 10, 14, 242);
        Uint32 edge = SDL_MapRGBA(renderer->frame->format, 142, 150, 158, 255);
        result = SDL_FillRect(renderer->frame, &card, background);
        SDL_Rect top = {card.x, card.y, card.w, 2}, bottom = {card.x, card.y + card.h - 2, card.w, 2};
        SDL_Rect left = {card.x, card.y, 2, card.h}, right = {card.x + card.w - 2, card.y, 2, card.h};
        if (result == 0 && (SDL_FillRect(renderer->frame, &top, edge) < 0 ||
                            SDL_FillRect(renderer->frame, &bottom, edge) < 0 ||
                            SDL_FillRect(renderer->frame, &left, edge) < 0 ||
                            SDL_FillRect(renderer->frame, &right, edge) < 0)) result = -1;
    }

    int padding = profile != NULL ? profile->padding : OSDD_PROFILE_DEFAULT_PADDING;
    if (padding < 0) padding = 0;
    if (padding > card.w / 3) padding = card.w / 3;
    int left = card.x + padding, right = card.x + card.w - padding;
    int center = renderer->frame->w / 2;
    const struct power_texts *texts = portuguese ? &power_portuguese : &power_english;
    if (result == 0) result = power_text(renderer, font, texts->title, center, card.y + 48, title);
    if (result == 0) result = power_text(renderer, font, power_body(power, texts), center, card.y + 142, message);
    if (result == 0 && power->diagnostic[0] != 0)
        result = power_text(renderer, font, power->diagnostic, center, card.y + 202, secondary);
    if (result == 0 && power->phase == OSDD_POWER_CONFIRM) {
        SDL_Rect buttons[3];
        power_buttons(left, right, card.y + 302, buttons);
        const char *labels[3] = {texts->off, texts->reboot, texts->cancel};
        for (int index = 0; index < 3 && result == 0; ++index)
            if (power_button(renderer, font, buttons[index], labels[index], (int)power->focus == index,
                             selector, secondary, selected) < 0) result = -1;
    }
    if (opened_font) TTF_CloseFont(font);
    if (result < 0) return fail(error, error_size, SDL_GetError());
    return 0;
}

void osdd_renderer_close(struct osdd_renderer *renderer) {
    if (renderer == NULL) return;
    if (renderer->frame != NULL) SDL_FreeSurface(renderer->frame);
    renderer->frame = NULL;
    for (size_t index = 0; index < renderer->asset_count; ++index)
        SDL_FreeSurface(renderer->assets[index].surface);
    renderer->asset_count = 0;
    if (renderer->panel_font != NULL) TTF_CloseFont(renderer->panel_font);
    renderer->panel_font = NULL;
    for (int i = 0; i < OSDD_LOOK_FONTS; ++i)
        if (renderer->look_fonts[i].font != NULL) TTF_CloseFont(renderer->look_fonts[i].font);
    memset(renderer->look_fonts, 0, sizeof(renderer->look_fonts));
    TTF_Quit();
    IMG_Quit();
    SDL_Quit();
}
