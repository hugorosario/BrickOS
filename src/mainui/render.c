#define _POSIX_C_SOURCE 200809L
#include "render.h"

#include "json.h"
#include "input.h"
#include "../common/brickos_palette.h"

#include <SDL_image.h>
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define PATH_BYTES 1100

static const SDL_Color white = {255, 255, 255, 255};
// The Default theme's accent, for shapes drawn when a skin image is missing.
static const SDL_Color green = {54, 255, 160, 255};

// ---- Resources -----------------------------------------------------------------------------

// The text roles, their config.json keys, and the Default sizes (TRIMUI Green's config, which
// match the Default captures: tab names 32, content 36, secondary 24, status and hints 24,
// popup rows 36, dialog title 44, message and buttons 36). Two roles differ from Green's: the
// focused grid tile's label is content_font5, which Default draws at 36 (r3) where Blue, Energy,
// and Green set 40 (round 7); the status bar is nav_font2, 32 in Syber (round 7). The last role
// is content_font2 again, for BrickOS's footer hints and status bar.
#define FONT_ROLES 12
enum { FONT_ROLE_STAT = 3, FONT_ROLE_STATUS_TEXT = 9, FONT_ROLE_HINT = 10 };
static struct mainui_font *font_role(struct mainui_renderer *r, int role) {
    struct mainui_font *fonts[FONT_ROLES] = {&r->nav, &r->content, &r->small, &r->stat, &r->menu, &r->title,
                                             &r->popup_text, &r->popup_button, &r->focus_label, &r->status_text,
                                             &r->hint, &r->tile};
    return fonts[role];
}
static const char *const font_keys[FONT_ROLES] = {
    "nav_font1", "content_font1", "content_font2", "stat_font1", "menu_font1", "popup_font1", "popup_font2", "popup_font3",
    "content_font5", "nav_font2", "content_font2", "brickos_tile_font",
};
static const int font_defaults[FONT_ROLES] = {32, 36, 24, 24, 36, 44, 36, 36, 36, 24, 24, 28};

// "#AARRGGBB" as the theme writes its colors.
static SDL_Color parse_color(const char *text, SDL_Color fallback) {
    if (!text || text[0] != '#' || strlen(text) != 9) return fallback;
    unsigned long v = strtoul(text + 1, NULL, 16);
    return (SDL_Color){(Uint8)(v >> 16), (Uint8)(v >> 8), (Uint8)v, (Uint8)(v >> 24)};
}

// The Appearance choices BrickOS draws with: the frontend's, or the defaults (all zero) before
// the first frame.
static const struct mainui_appearance *look(const struct mainui_renderer *r) {
    static const struct mainui_appearance defaults;
    return r->appearance ? r->appearance : &defaults;
}

// BrickOS takes nothing from the stock theme: its palette grows from the Appearance accent
// (Violet is the BrickOS seed), with the background tone the Appearance chooses. The OSD builds
// the same palette (brickos_palette.h).
static void material_palette_update(struct mainui_renderer *r) {
    struct mainui_rgb accent = mainui_appearance_seed(look(r));
    brickos_palette_build(&r->material, (SDL_Color){accent.r, accent.g, accent.b, 255},
                          mainui_appearance_background_level(look(r)));
}

static void close_fonts(struct mainui_renderer *r) {
    for (int role = 0; role < FONT_ROLES; ++role) {
        struct mainui_font *f = font_role(r, role);
        if (f->font) TTF_CloseFont(f->font);
        if (f->fallback) TTF_CloseFont(f->fallback);
        memset(f, 0, sizeof(*f));
    }
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
    if ((bytes == 2 && value < 0x80) || (bytes == 3 && value < 0x800) ||
        (bytes == 4 && value < 0x10000) || value > 0x10ffff ||
        (value >= 0xd800 && value <= 0xdfff)) return 0;
    *cursor = p + bytes;
    *codepoint = value;
    return 1;
}

static int font_supports_text(TTF_Font *font, const char *s) {
    if (!font || !s) return 0;
    const unsigned char *p = (const unsigned char *)s;
    while (*p) {
        unsigned int codepoint;
        if (!next_codepoint(&p, &codepoint)) return 0;
        if (codepoint == '\n' || codepoint == '\r' || codepoint == '\t') continue;
        if (codepoint > 0xffff || !TTF_GlyphIsProvided(font, (Uint16)codepoint)) return 0;
    }
    return 1;
}

// Whether a role draws every character of s, in its font or in its fallback (the mixed Appearance
// fonts draw by runs).
static int font_draws_text(const struct mainui_font *font, const char *s) {
    if (!font->font || !s) return 0;
    const unsigned char *p = (const unsigned char *)s;
    while (*p) {
        unsigned int codepoint;
        if (!next_codepoint(&p, &codepoint)) return 0;
        if (codepoint == '\n' || codepoint == '\r' || codepoint == '\t') continue;
        if (codepoint > 0xffff) return 0;
        if (!TTF_GlyphIsProvided(font->font, (Uint16)codepoint) &&
            !(font->fallback && TTF_GlyphIsProvided(font->fallback, (Uint16)codepoint)))
            return 0;
    }
    return 1;
}

static TTF_Font *font_for_text(const struct mainui_font *font, const char *s, int *shift) {
    if (!font->fallback) {
        if (shift) *shift = font->shift;
        return font->font;
    }
    if (font_supports_text(font->font, s)) {
        if (shift) *shift = font->shift;
        return font->font;
    }
    if (font_supports_text(font->fallback, s)) {
        if (shift) *shift = font->fallback_shift;
        return font->fallback;
    }
    if (shift) *shift = font->shift;
    return font->font ? font->font : font->fallback;
}

// Opens the theme's font at the sizes its config.json names, and takes its colors (f04).
// BrickOS passes its Appearance font, a path under the fonts folder ("" for the Default font),
// and its Text size scale, which every role but the hints and the status bar follows; the
// language's Default font then supplies the glyphs the chosen font lacks. Stock++ passes "" and 1.
static int load_theme(struct mainui_renderer *r, const char *theme_font, const char *chosen_font, double scale) {
    char path[PATH_BYTES], full[PATH_BYTES], config[PATH_BYTES], text[8192];
    char fallback_path[PATH_BYTES], fallback_full[PATH_BYTES];
    struct mainui_json sizes, colors;
    const char *value;
    size_t length, n = 0;
    sizes.count = colors.count = 0;
    close_fonts(r);
    if (mainui_asset_find(r->fs, r->theme, "config.json", config, sizeof(config)) == 1) {
        snprintf(full, sizeof(full), "%s%s", r->fs->root, config);
        FILE *f = fopen(full, "rb");
        if (f) {
            n = fread(text, 1, sizeof(text) - 1, f);
            fclose(f);
        }
        text[n] = 0;
        if (mainui_json_find(text, n, "fontsize", &value, &length) == 0) mainui_json_parse(&sizes, value, length);
        if (mainui_json_find(text, n, "fontcolor", &value, &length) == 0) mainui_json_parse(&colors, value, length);
    }
    if (chosen_font && chosen_font[0]) snprintf(path, sizeof(path), "%s", chosen_font);
    else if (mainui_asset_font(r->fs, r->theme, theme_font, r->language, path, sizeof(path)) < 0) return -1;
    snprintf(full, sizeof(full), "%s%s", r->fs->root, path);
    fallback_full[0] = 0;
    if (mainui_asset_font(r->fs, MAINUI_DEFAULT_THEME_DIR, "", r->language,
                          fallback_path, sizeof(fallback_path)) >= 0 && strcmp(path, fallback_path) != 0)
        snprintf(fallback_full, sizeof(fallback_full), "%s%s", r->fs->root, fallback_path);
    // The positions here were measured with the Default theme's font in English.
    char reference_path[PATH_BYTES], reference_full[PATH_BYTES];
    reference_full[0] = 0;
    if (mainui_asset_font(r->fs, MAINUI_DEFAULT_THEME_DIR, "", "en.lang", reference_path, sizeof(reference_path)) >= 0)
        snprintf(reference_full, sizeof(reference_full), "%s%s", r->fs->root, reference_path);
    for (int role = 0; role < FONT_ROLES; ++role) {
        struct mainui_font *f = font_role(r, role);
        const struct mainui_json_member *m = mainui_json_get(&sizes, font_keys[role]);
        f->size = m && m->type == MAINUI_JSON_NUMBER && m->number >= 8 && m->number <= 128 ? (int)m->number : font_defaults[role];
        if (scale != 1 && role != FONT_ROLE_STAT && role != FONT_ROLE_STATUS_TEXT && role != FONT_ROLE_HINT) {
            f->size = (int)lround(f->size * scale);
            f->size = f->size < 8 ? 8 : f->size > 128 ? 128 : f->size;
        }
        f->font = TTF_OpenFont(full, f->size);
        if (!f->font) return -1;
        if (fallback_full[0]) f->fallback = TTF_OpenFont(fallback_full, f->size);
        f->mixed = chosen_font && chosen_font[0] && f->fallback;
        // The official frontend centers each text in its rectangle by the font height (static:
        // DrawString draws the surface at rect.y + (rect.h - height) / 2). So text in another
        // font or size moves by half the difference in height, rounded down: 4 for Syber Dark's
        // 32 against 36 and -2 for its hints at 28 against 24 (f04), -1 for full.ttf at 36 (f05).
        TTF_Font *reference = reference_full[0] ? TTF_OpenFont(reference_full, font_defaults[role]) : NULL;
        int d = reference ? TTF_FontHeight(reference) - TTF_FontHeight(f->font) : 0;
        f->shift = d >= 0 ? d / 2 : -((1 - d) / 2);
        if (f->fallback) {
            int fallback_d = reference ? TTF_FontHeight(reference) - TTF_FontHeight(f->fallback) : 0;
            f->fallback_shift = fallback_d >= 0 ? fallback_d / 2 : -((1 - fallback_d) / 2);
        }
        if (reference) TTF_CloseFont(reference);
    }
    // Secondary text is white at half opacity over whatever is under it (n03, n05, n08).
    static const SDL_Color half = {255, 255, 255, 128};
#define COLOR(key, fallback) parse_color(mainui_json_get(&colors, key) ? mainui_json_get(&colors, key)->text : NULL, fallback)
    const struct mainui_json_member *accent = mainui_json_get(&colors, "highlight_color1");
    SDL_Color material_seed = accent && accent->type == MAINUI_JSON_STRING ?
        parse_color(accent->text, (SDL_Color){0, 0, 0, 0}) : (SDL_Color){0, 0, 0, 0};
    r->material_accent_valid = material_seed_valid(material_seed);
    r->nav_dim = COLOR("nav_color1", half);
    r->nav_focus = COLOR("nav_color2", white);
    r->home_dim = COLOR("nav_color3", half);
    r->status_color = COLOR("nav_color3", white);
    r->dim = COLOR("content_color2", half);
    r->number = COLOR("content_color3", green);
    r->title_color = COLOR("popup_color1", green);
    r->highlight = COLOR("highlight_color1", green);
#undef COLOR
    return 0;
}

int mainui_render_open(struct mainui_renderer *r, SDL_Renderer *sdl, const struct mainui_fs *fs,
                       const char *theme, const char *theme_font, const char *language) {
    memset(r, 0, sizeof(*r));
    r->sdl = sdl;
    r->fs = fs;
    snprintf(r->theme, sizeof(r->theme), "%s", theme ? theme : "../res/");
    snprintf(r->language, sizeof(r->language), "%s", language ? language : "en.lang");
    snprintf(r->status.clock, sizeof(r->status.clock), "--:--");
    r->status.battery = 100;
    return load_theme(r, theme_font, "", 1);
}

static void home_cache_clear(struct mainui_renderer *r) {
    for (int tab = 0; tab < MAINUI_HOME_COUNT; ++tab) {
        if (r->home_cards[tab].normal) SDL_DestroyTexture(r->home_cards[tab].normal);
        if (r->home_cards[tab].focused) SDL_DestroyTexture(r->home_cards[tab].focused);
        SDL_FreeSurface(r->home_cards[tab].icon);
    }
    memset(r->home_cards, 0, sizeof(r->home_cards));
}

static void loading_preview_clear(struct mainui_renderer *r) {
    struct mainui_loading_preview *p = &r->loading_preview;
    for (int i = 0; i <= MAINUI_LOADING_SCREEN_FRAMES; ++i)
        if (p->images[i]) SDL_DestroyTexture(p->images[i]);
    memset(p, 0, sizeof(*p));
}

static void font_samples_clear(struct mainui_renderer *r) {
    for (int i = 0; i < r->font_sample_count; ++i)
        if (r->font_samples[i].font) TTF_CloseFont(r->font_samples[i].font);
    memset(r->font_samples, 0, sizeof(r->font_samples));
    r->font_sample_count = 0;
}

// The font a font list row's name is drawn in, at the content size: a font of the fonts folder,
// or for Default ("") the language's font. A file that starts with "/" is a device path, as a
// Language row names its language's Default font. NULL when it does not open.
static TTF_Font *font_sample(struct mainui_renderer *r, const char *file) {
    for (int i = 0; i < r->font_sample_count; ++i)
        if (strcmp(r->font_samples[i].file, file) == 0 && r->font_samples[i].size == r->content.size)
            return r->font_samples[i].font;
    if (r->font_sample_count == MAINUI_FONT_SAMPLES) font_samples_clear(r);
    struct mainui_font_sample *sample = &r->font_samples[r->font_sample_count++];
    snprintf(sample->file, sizeof(sample->file), "%s", file);
    sample->size = r->content.size;
    char path[PATH_BYTES], full[PATH_BYTES];
    if (file[0] == '/') snprintf(path, sizeof(path), "%s", file);
    else if (file[0]) snprintf(path, sizeof(path), "%s/%s", MAINUI_FONTS_DIR, file);
    else if (mainui_asset_font(r->fs, MAINUI_DEFAULT_THEME_DIR, "", r->language, path, sizeof(path)) < 0) path[0] = 0;
    snprintf(full, sizeof(full), "%s%s", r->fs->root, path);
    sample->font = path[0] && sample->size > 0 ? TTF_OpenFont(full, sample->size) : NULL;
    return sample->font;
}

static void prefetch_close(struct mainui_render_prefetch *p);

void mainui_render_close(struct mainui_renderer *r) {
    prefetch_close(&r->prefetch);
    home_cache_clear(r);
    loading_preview_clear(r);
    font_samples_clear(r);
    for (int radius = 0; radius < 25; ++radius)
        if (r->focus_aura[radius]) SDL_DestroyTexture(r->focus_aura[radius]);
    for (int i = 0; i < r->cache_count; ++i)
        if (r->cache[i].texture) SDL_DestroyTexture(r->cache[i].texture);
    close_fonts(r);
    memset(r, 0, sizeof(*r));
}

static int same_file(const struct stat *old, const struct stat *stamp) {
    return old->st_dev == stamp->st_dev && old->st_ino == stamp->st_ino && old->st_size == stamp->st_size &&
           old->st_mtim.tv_sec == stamp->st_mtim.tv_sec && old->st_mtim.tv_nsec == stamp->st_mtim.tv_nsec &&
           old->st_ctim.tv_sec == stamp->st_ctim.tv_sec && old->st_ctim.tv_nsec == stamp->st_ctim.tv_nsec;
}

static int image_unchanged(const struct mainui_render_image *entry, const struct stat *stamp) {
    return same_file(&entry->source_stamp, stamp);
}

// Resized image styles: a monochrome mask, and a size fitted in w x h with the aspect kept.
enum { RESIZED_MONOCHROME = 1, RESIZED_FIT = 2 };
static void resize_bilinear(const Uint32 *src, Uint32 *dst, int w, int h, int w2, int h2);

// Decodes a picture: as it is when w x h is 0 x 0, else resized to w x h the official way in that
// style. w and h become the size made. It uses no renderer state, so the prefetch worker runs it too.
static SDL_Surface *decoded_surface(const char *full, int *w, int *h, int style) {
    if (*w <= 0 || *h <= 0) {
        SDL_Surface *surface = IMG_Load(full);
        if (surface) {
            *w = surface->w;
            *h = surface->h;
        }
        return surface;
    }
    SDL_Surface *loaded = IMG_Load(full), *source = loaded ? SDL_ConvertSurfaceFormat(loaded, SDL_PIXELFORMAT_ARGB8888, 0) : NULL;
    if (loaded) SDL_FreeSurface(loaded);
    if (!source) return NULL;
    if ((style & RESIZED_FIT) && source->w > 0 && source->h > 0) {
        // The largest size of the picture's aspect inside w x h.
        if ((long)source->w * *h > (long)source->h * *w) *h = (int)((long)source->h * *w / source->w);
        else *w = (int)((long)source->w * *h / source->h);
        if (*w < 1) *w = 1;
        if (*h < 1) *h = 1;
    }
    int unchanged_size = source->w == *w && source->h == *h;
    SDL_Surface *sized = unchanged_size ? source : SDL_CreateRGBSurfaceWithFormat(0, *w, *h, 32, SDL_PIXELFORMAT_ARGB8888);
    if (!unchanged_size && sized && source->w >= 2 && source->h >= 2 && source->pitch == source->w * 4 && sized->pitch == *w * 4) {
        resize_bilinear(source->pixels, sized->pixels, source->w, source->h, *w, *h);
    } else if (!unchanged_size && sized) {
        SDL_BlitScaled(source, NULL, sized, NULL);
    }
    if (!unchanged_size) SDL_FreeSurface(source);
    if (sized && (style & RESIZED_MONOCHROME)) {
        // Use the generated silhouette's alpha as a mask; the theme supplies its ink.
        for (int y = 0; y < sized->h; ++y) {
            Uint32 *pixels = (Uint32 *)((Uint8 *)sized->pixels + y * sized->pitch);
            for (int x = 0; x < sized->w; ++x) pixels[x] = (pixels[x] & 0xff000000U) | 0x00ffffffU;
        }
    }
    return sized;
}

// Makes a decoded picture its cache entry's texture: blended when resized, else smoothed where
// drawn scaled (l2a box art shows smoothed, not with blocky pixels). A NULL surface records that
// the file at stamp did not decode, so it is not decoded again until it changes.
static struct mainui_render_image *install_picture(struct mainui_renderer *r, struct mainui_render_image *entry,
                                                   SDL_Surface *surface, int w, int h, int resized,
                                                   const struct stat *stamp) {
    entry->failed = !surface;
    entry->source_stamp = *stamp;
    if (!surface) return NULL;
    entry->texture = SDL_CreateTextureFromSurface(r->sdl, surface);
    if (entry->texture) {
        if (resized) SDL_SetTextureBlendMode(entry->texture, SDL_BLENDMODE_BLEND);
        else SDL_SetTextureScaleMode(entry->texture, SDL_ScaleModeLinear);
    }
    entry->w = w;
    entry->h = h;
    SDL_FreeSurface(surface);
    return entry->texture ? entry : NULL;
}

// Both original dimensions and resized pixels must follow an SD image's lifecycle.
// A missing or temporarily undecodable file must not become a permanent cache miss.
static struct mainui_render_image *image(struct mainui_renderer *r, const char *path) {
    if (!path || !path[0]) return NULL;
    char full[PATH_BYTES];
    int n = snprintf(full, sizeof(full), "%s%s", r->fs->root, path);
    if (n < 0 || n >= (int)sizeof(full)) return NULL;
    struct mainui_render_image *entry = NULL;
    for (int i = 0; i < r->cache_count; ++i)
        if (strcmp(r->cache[i].path, path) == 0) { entry = &r->cache[i]; break; }
    struct stat stamp;
    int present = stat(full, &stamp) == 0 && S_ISREG(stamp.st_mode);
    if (entry && entry->texture) {
        if (present && image_unchanged(entry, &stamp)) return entry;
        SDL_DestroyTexture(entry->texture);
        entry->texture = NULL;
        entry->w = entry->h = 0;
    }
    if (!present) return NULL;
    // A file that did not decode, such as the stock card's empty system backgrounds, is not
    // decoded again until it changes.
    if (entry && entry->failed && image_unchanged(entry, &stamp)) return NULL;
    if (!entry) {
        if (r->cache_count == MAINUI_RENDER_CACHE) return NULL;
        entry = &r->cache[r->cache_count++];
        memset(entry, 0, sizeof(*entry));
        snprintf(entry->path, sizeof(entry->path), "%s", path);
    }
    int w = 0, h = 0;
    Uint64 start = SDL_GetPerformanceCounter();
    SDL_Surface *surface = decoded_surface(full, &w, &h, 0);
    struct mainui_render_image *made = install_picture(r, entry, surface, w, h, 0, &stamp);
    r->decode_ticks += SDL_GetPerformanceCounter() - start;
    return made;
}

// The official loader resizes box art on the CPU before drawing it at its own size (static:
// LoadTrimuiSurfaceSpecSized calls resizeBilinear_cpu): the classic bilinear with ratios
// (w - 1) / w2 and (h - 1) / h2, single-precision weights, and each channel truncated.
static void resize_bilinear(const Uint32 *src, Uint32 *dst, int w, int h, int w2, int h2) {
    float x_ratio = (float)(w - 1) / (float)w2, y_ratio = (float)(h - 1) / (float)h2;
    for (int i = 0; i < h2; ++i) {
        float fy = y_ratio * (float)i;
        int y = (int)fy;
        float yd = fy - (float)y;
        for (int j = 0; j < w2; ++j) {
            float fx = x_ratio * (float)j;
            int x = (int)fx;
            float xd = fx - (float)x;
            int index = y * w + x;
            Uint32 a = src[index], b = src[index + 1], c = src[index + w], d = src[index + w + 1], out = 0;
            for (int shift = 0; shift < 32; shift += 8) {
                float v = (float)((a >> shift) & 0xff) * (1 - xd) * (1 - yd) + (float)((b >> shift) & 0xff) * xd * (1 - yd) +
                          (float)((c >> shift) & 0xff) * yd * (1 - xd) + (float)((d >> shift) & 0xff) * xd * yd;
                out |= (Uint32)((int)v & 0xff) << shift;
            }
            dst[i * w2 + j] = out;
        }
    }
}

// A picture resized to w x h the official way, cached apart from the unscaled image.
static int resized_key(char *key, size_t size, const char *path, int w, int h, int style) {
    int n = snprintf(key, size, "%s@%dx%d%s%s", path, w, h, style & RESIZED_MONOCHROME ? "-mono" : "",
                     style & RESIZED_FIT ? "-fit" : "");
    return n < 0 || n >= (int)size ? -1 : 0;
}

static struct mainui_render_image *image_resized_style(struct mainui_renderer *r, const char *path, int w, int h,
                                                      int style) {
    char key[1100], full[PATH_BYTES];
    if (!path || !path[0] || w <= 0 || h <= 0) return NULL;
    if (resized_key(key, sizeof(key), path, w, h, style) < 0) return NULL;
    int n;
    n = snprintf(full, sizeof(full), "%s%s", r->fs->root, path);
    if (n < 0 || n >= (int)sizeof(full)) return NULL;
    struct mainui_render_image *entry = NULL;
    for (int i = 0; i < r->cache_count; ++i) {
        if (strcmp(r->cache[i].path, key) == 0) { entry = &r->cache[i]; break; }
    }
    struct stat stamp;
    int present = stat(full, &stamp) == 0 && S_ISREG(stamp.st_mode);
    if (entry && entry->texture) {
        if (present && image_unchanged(entry, &stamp)) return entry;
        SDL_DestroyTexture(entry->texture);
        entry->texture = NULL;
    }
    // Do not remember a failed decode as permanent: a cover can arrive after its first frame.
    // The same unchanged file is not decoded again, though.
    if (!present) return NULL;
    if (entry && entry->failed && image_unchanged(entry, &stamp)) return NULL;
    if (!entry) {
        if (r->cache_count == MAINUI_RENDER_CACHE) return NULL;
        entry = &r->cache[r->cache_count++];
        memset(entry, 0, sizeof(*entry));
        snprintf(entry->path, sizeof(entry->path), "%s", key);
    }
    Uint64 start = SDL_GetPerformanceCounter();
    SDL_Surface *sized = decoded_surface(full, &w, &h, style);
    struct mainui_render_image *made = install_picture(r, entry, sized, w, h, 1, &stamp);
    r->decode_ticks += SDL_GetPerformanceCounter() - start;
    return made;
}

static struct mainui_render_image *image_resized(struct mainui_renderer *r, const char *path, int w, int h) {
    return image_resized_style(r, path, w, h, 0);
}

// 1 when drawing the fitted image would decode nothing: it is held and unchanged, it did not
// decode as it is, or there is no file.
// What drawing the picture of path cached under key would do: 0 decode it, 1 draw the held
// texture, 2 nothing (no file, or the unchanged file did not decode).
static int image_state(struct mainui_renderer *r, const char *key, const char *path) {
    char full[PATH_BYTES];
    int n = snprintf(full, sizeof(full), "%s%s", r->fs->root, path);
    struct stat stamp;
    if (n < 0 || n >= (int)sizeof(full) || stat(full, &stamp) != 0 || !S_ISREG(stamp.st_mode)) return 2;
    for (int i = 0; i < r->cache_count; ++i)
        if (strcmp(r->cache[i].path, key) == 0) {
            if (!image_unchanged(&r->cache[i], &stamp)) return 0;
            return r->cache[i].texture ? 1 : r->cache[i].failed ? 2 : 0;
        }
    return 0;
}

static int image_fitted_ready(struct mainui_renderer *r, const char *path, int w, int h) {
    char key[1100];
    if (resized_key(key, sizeof(key), path, w, h, RESIZED_FIT) < 0) return 1;
    return image_state(r, key, path) != 0;
}

// A Grid or Carousel cover, fitted in req_w x req_h. A live frame decodes at most
// MAINUI_COVERS_PER_FRAME covers it does not hold yet; the others keep their placeholder until the
// next frames, which follow at once. Static captures decode every cover.
#define MAINUI_COVERS_PER_FRAME 2
static struct mainui_render_image *view_cover(struct mainui_renderer *r, const char *path, int req_w, int req_h) {
    if (!path || !path[0] || req_w <= 0 || req_h <= 0) return NULL;
    if (r->animations_enabled && !image_fitted_ready(r, path, req_w, req_h)) {
        if (r->covers_left <= 0) {
            r->covers_pending = 1;
            return NULL;
        }
        --r->covers_left;
    }
    return image_resized_style(r, path, req_w, req_h, RESIZED_FIT);
}

// A system icon: the Games grid shows them unsmoothed (n05), unlike box art and app icons.
static struct mainui_render_image *system_icon(struct mainui_renderer *r, const char *path) {
    struct mainui_render_image *img = image(r, path);
    if (img) SDL_SetTextureScaleMode(img->texture, SDL_ScaleModeNearest);
    return img;
}

// A skin image of the theme, falling back to the Default theme's.
static struct mainui_render_image *skin(struct mainui_renderer *r, const char *name) {
    char relative[256], path[PATH_BYTES];
    snprintf(relative, sizeof(relative), "skin/%s", name);
    if (mainui_asset_find(r->fs, r->theme, relative, path, sizeof(path)) < 0) return NULL;
    return image(r, path);
}

// ---- Drawing -------------------------------------------------------------------------------

static void rounded_fill(SDL_Renderer *sdl, int x, int y, int w, int h, int radius) {
    if (radius <= 0) {
        SDL_Rect rect = {x, y, w, h};
        SDL_RenderFillRect(sdl, &rect);
        return;
    }
    int r2 = radius * radius;
    if (h < 2 * radius) {
        for (int row = 0; row < h; ++row) {
            int edge = row < h - row - 1 ? row : h - row - 1;
            int dy = radius - edge;
            int inset = radius - (int)sqrt((double)(r2 - dy * dy));
            SDL_RenderDrawLine(sdl, x + inset, y + row, x + w - inset - 1, y + row);
        }
        return;
    }
    SDL_Rect middle = {x, y + radius, w, h - 2 * radius};
    if (middle.h > 0) SDL_RenderFillRect(sdl, &middle);
    for (int row = 0; row < radius; ++row) {
        int dy = radius - row;
        int inset = radius - (int)sqrt((double)(r2 - dy * dy));
        SDL_RenderDrawLine(sdl, x + inset, y + row, x + w - inset - 1, y + row);
        SDL_RenderDrawLine(sdl, x + inset, y + h - row - 1, x + w - inset - 1, y + h - row - 1);
    }
}

static void rounded_outline(SDL_Renderer *sdl, int x, int y, int w, int h, int radius, int thickness, SDL_Color c) {
    SDL_SetRenderDrawColor(sdl, c.r, c.g, c.b, c.a);
    for (int i = 0; i < thickness && radius - i >= 1; ++i) {
        int rr = radius - i;
        SDL_RenderDrawLine(sdl, x + radius, y + i, x + w - radius - 1, y + i);
        SDL_RenderDrawLine(sdl, x + radius, y + h - i - 1, x + w - radius - 1, y + h - i - 1);
        SDL_RenderDrawLine(sdl, x + i, y + radius, x + i, y + h - radius - 1);
        SDL_RenderDrawLine(sdl, x + w - i - 1, y + radius, x + w - i - 1, y + h - radius - 1);
        const int starts[4] = {180, 270, 0, 90};
        const int cx[4] = {x + radius, x + w - radius - 1, x + w - radius - 1, x + radius};
        const int cy[4] = {y + radius, y + radius, y + h - radius - 1, y + h - radius - 1};
        for (int corner = 0; corner < 4; ++corner) {
            int previous_x = 0, previous_y = 0;
            for (int angle = starts[corner]; angle <= starts[corner] + 90; angle += 3) {
                double radians = angle * 3.14159265358979323846 / 180.0;
                int px = cx[corner] + (int)lround(cos(radians) * rr);
                int py = cy[corner] + (int)lround(sin(radians) * rr);
                if (angle != starts[corner]) SDL_RenderDrawLine(sdl, previous_x, previous_y, px, py);
                previous_x = px;
                previous_y = py;
            }
        }
    }
}

static void fill(struct mainui_renderer *r, int x, int y, int w, int h, SDL_Color c) {
    SDL_SetRenderDrawBlendMode(r->sdl, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(r->sdl, c.r, c.g, c.b, c.a);
    if (r->presentation == MAINUI_PRESENTATION_MATERIAL_YOU && w >= 32 && h >= 24 &&
        !(w >= MAINUI_SCREEN_W && h >= MAINUI_SCREEN_H)) {
        // The Card shape's corners (Rounded: min(h/5, 24, w/5); Square: none).
        rounded_fill(r->sdl, x, y, w, h, mainui_appearance_corner(look(r), w, h));
    } else {
        SDL_Rect rect = {x, y, w, h};
        SDL_RenderFillRect(r->sdl, &rect);
    }
}

static void outline(struct mainui_renderer *r, int x, int y, int w, int h, int thickness, SDL_Color c) {
    if (!c.a) return;
    SDL_SetRenderDrawBlendMode(r->sdl, SDL_BLENDMODE_BLEND);
    int radius = mainui_appearance_corner(look(r), w, h);
    if (r->presentation == MAINUI_PRESENTATION_MATERIAL_YOU && w >= 32 && h >= 24 && radius >= 2) {
        rounded_outline(r->sdl, x, y, w, h, radius, thickness, c);
        return;
    }
    SDL_SetRenderDrawColor(r->sdl, c.r, c.g, c.b, c.a);
    for (int i = 0; i < thickness; ++i) {
        SDL_Rect rect = {x + i, y + i, w - 2 * i, h - 2 * i};
        SDL_RenderDrawRect(r->sdl, &rect);
    }
}

// Draws an image into a box, scaled to fit and centered.
static void draw_fit(struct mainui_renderer *r, struct mainui_render_image *img, int x, int y, int w, int h) {
    if (!img || img->w <= 0 || img->h <= 0) return;
    double scale = (double)w / img->w;
    if ((double)h / img->h < scale) scale = (double)h / img->h;
    int dw = (int)(img->w * scale), dh = (int)(img->h * scale);
    SDL_Rect to = {x + (w - dw) / 2, y + (h - dh) / 2, dw, dh};
    SDL_RenderCopy(r->sdl, img->texture, NULL, &to);
}

// IconMenu's flow images use reciprocal single-precision ratios before integer
// truncation. A 256px icon in a 500px box becomes 499px, as measured on stock.
static void draw_flow_icon(struct mainui_renderer *r, struct mainui_render_image *img, int x, int y, int w, int h) {
    if (!img || img->w <= 0 || img->h <= 0) return;
    float scale = 1.f / fmaxf((float)img->w / (float)w, (float)img->h / (float)h);
    int dw = (int)((float)img->w * scale), dh = (int)((float)img->h * scale);
    SDL_Rect to = {x + (w - dw) / 2, y + (h - dh) / 2, dw, dh};
    SDL_RenderCopy(r->sdl, img->texture, NULL, &to);
}

// Draws an image centered in a box at its own size, scaled down only when it is larger (the
// official grid shows 300-pixel system icons at 240 and 100-pixel setting icons as they are).
static void draw_down(struct mainui_renderer *r, struct mainui_render_image *img, int x, int y, int w, int h) {
    if (!img || img->w <= 0 || img->h <= 0) return;
    if (img->w > w || img->h > h) { draw_fit(r, img, x, y, w, h); return; }
    SDL_Rect to = {x + (w - img->w) / 2, y + (h - img->h) / 2, img->w, img->h};
    SDL_RenderCopy(r->sdl, img->texture, NULL, &to);
}

static void draw_stretch(struct mainui_renderer *r, struct mainui_render_image *img, int x, int y, int w, int h) {
    if (!img) return;
    SDL_Rect to = {x, y, w, h};
    SDL_RenderCopy(r->sdl, img->texture, NULL, &to);
}

// Draws an image at its own size with its top left at x, y.
static void draw_at(struct mainui_renderer *r, struct mainui_render_image *img, int x, int y) {
    if (img) draw_stretch(r, img, x, y, img->w, img->h);
}

enum align { LEFT, CENTER, RIGHT };

// ---- Mixed text: a BrickOS Appearance font and the language's Default font ----

static int mixed_text(const struct mainui_font *font, const char *s) {
    return font->mixed && font->font && font->fallback && s && s[0] && !font_supports_text(font->font, s);
}

// The next run of s that one font draws, at most size - 1 bytes, copied into out: the chosen
// font's characters, or the ones only the fallback has. Returns its length in s.
static size_t mixed_run(const struct mainui_font *font, const char *s, char *out, size_t size, TTF_Font **run_font) {
    const unsigned char *p = (const unsigned char *)s;
    int own = -1;
    while (*p) {
        const unsigned char *start = p;
        unsigned int codepoint = '?';
        if (!next_codepoint(&p, &codepoint)) p = start + 1;
        int mine = codepoint > 0xffff || TTF_GlyphIsProvided(font->font, (Uint16)codepoint) ||
                   !TTF_GlyphIsProvided(font->fallback, (Uint16)codepoint);
        if ((own >= 0 && mine != own) || (size_t)(p - (const unsigned char *)s) >= size) {
            p = start;
            break;
        }
        own = mine;
    }
    size_t length = (size_t)(p - (const unsigned char *)s);
    memcpy(out, s, length);
    out[length] = 0;
    *run_font = own ? font->font : font->fallback;
    return length;
}

static int mixed_width(const struct mainui_font *font, const char *s) {
    char run[512];
    int total = 0;
    TTF_Font *run_font;
    for (size_t n; *s; s += n) {
        int w = 0;
        n = mixed_run(font, s, run, sizeof(run), &run_font);
        if (!n) break;
        if (TTF_SizeUTF8(run_font, run, &w, NULL) == 0) total += w;
    }
    return total;
}

static int mixed_ascent(const struct mainui_font *font) {
    int a = TTF_FontAscent(font->font), b = TTF_FontAscent(font->fallback);
    return a > b ? a : b;
}

static int mixed_height(const struct mainui_font *font) {
    int a = TTF_FontDescent(font->font), b = TTF_FontDescent(font->fallback);
    return mixed_ascent(font) - (a < b ? a : b);
}

static int draw_mixed(struct mainui_renderer *r, const struct mainui_font *font, const char *s, int x, int y, int rect_h,
                      SDL_Color c, enum align a) {
    int w = mixed_width(font, s), ascent = mixed_ascent(font);
    y += rect_h > 0 ? (rect_h - mixed_height(font)) / 2 : font->shift;
    int left = a == LEFT ? x : a == CENTER ? x - (w + 1) / 2 : x - w;
    SDL_Color solid = {c.r, c.g, c.b, 255};
    char run[512];
    TTF_Font *run_font;
    for (size_t n; *s; s += n) {
        n = mixed_run(font, s, run, sizeof(run), &run_font);
        if (!n) break;
        SDL_Surface *surface = TTF_RenderUTF8_Blended(run_font, run, solid);
        if (!surface) continue;
        SDL_Texture *texture = SDL_CreateTextureFromSurface(r->sdl, surface);
        SDL_Rect to = {left, y + ascent - TTF_FontAscent(run_font), surface->w, surface->h};
        left += surface->w;
        SDL_FreeSurface(surface);
        if (!texture) continue;
        SDL_SetTextureAlphaMod(texture, c.a);
        SDL_RenderCopy(r->sdl, texture, NULL, &to);
        SDL_DestroyTexture(texture);
    }
    return w;
}

// Draws one line of text and returns its width. Its top is at y (for the Default size) plus the
// role's shift; or, with a rectangle height, the text is centered in the rectangle that starts at
// y by its own rendered height, as the official DrawString does (rect.y + (rect.h - height) / 2).
static int draw_text(struct mainui_renderer *r, const struct mainui_font *font, const char *s, int x, int y, int rect_h, SDL_Color c, enum align a) {
    if (!s || !s[0]) return 0;
    if (mixed_text(font, s)) return draw_mixed(r, font, s, x, y, rect_h, c, a);
    int shift = 0;
    TTF_Font *selected = font_for_text(font, s, &shift);
    if (!selected) return 0;
    SDL_Color solid = {c.r, c.g, c.b, 255};
    SDL_Surface *surface = TTF_RenderUTF8_Blended(selected, s, solid);
    if (!surface) return 0;
    SDL_Texture *texture = SDL_CreateTextureFromSurface(r->sdl, surface);
    int w = surface->w, h = surface->h;
    SDL_FreeSurface(surface);
    if (!texture) return 0;
    y += rect_h > 0 ? (rect_h - h) / 2 : shift;
    SDL_SetTextureAlphaMod(texture, c.a);
    SDL_Rect to = {a == LEFT ? x : a == CENTER ? x - (w + 1) / 2 : x - w, y, w, h};
    SDL_RenderCopy(r->sdl, texture, NULL, &to);
    SDL_DestroyTexture(texture);
    return w;
}

static int text(struct mainui_renderer *r, const struct mainui_font *font, const char *s, int x, int y, SDL_Color c, enum align a) {
    return draw_text(r, font, s, x, y, 0, c, a);
}

// One line centered on center_x, drawn whole and scaled down to max_width when it is wider, its
// middle where the unscaled line's would be. Returns 0, drawing nothing, for text in more than one
// font, which the caller shortens instead.
static int text_fit(struct mainui_renderer *r, const struct mainui_font *font, const char *s, int center_x, int y,
                    int max_width, SDL_Color c) {
    if (!s || !s[0] || mixed_text(font, s)) return 0;
    int shift = 0;
    TTF_Font *selected = font_for_text(font, s, &shift);
    if (!selected) return 0;
    SDL_Color solid = {c.r, c.g, c.b, 255};
    SDL_Surface *surface = TTF_RenderUTF8_Blended(selected, s, solid);
    if (!surface) return 0;
    SDL_Texture *texture = SDL_CreateTextureFromSurface(r->sdl, surface);
    int w = surface->w, h = surface->h;
    SDL_FreeSurface(surface);
    if (!texture) return 0;
    int fit_w = w > max_width ? max_width : w, fit_h = w > max_width ? (int)lroundf((float)h * max_width / w) : h;
    SDL_SetTextureScaleMode(texture, SDL_ScaleModeLinear);
    SDL_SetTextureAlphaMod(texture, c.a);
    SDL_Rect to = {center_x - (fit_w + 1) / 2, y + shift + (h - fit_h) / 2, fit_w, fit_h};
    SDL_RenderCopy(r->sdl, texture, NULL, &to);
    SDL_DestroyTexture(texture);
    return 1;
}

// Centers a line in a rectangle as the official DrawString does, at rect.x + (rect.w - width) / 2:
// a line wider than the rectangle, such as a long tile label, starts where C division puts it,
// one pixel right of an exact half for an odd overflow.
static int text_in(struct mainui_renderer *r, const struct mainui_font *font, const char *s, int left, int width, int y,
                   SDL_Color c) {
    int shift = 0, w = 0;
    if (mixed_text(font, s)) return text(r, font, s, left + (width - mixed_width(font, s)) / 2, y, c, LEFT);
    TTF_Font *selected = s && s[0] ? font_for_text(font, s, &shift) : NULL;
    if (!selected || TTF_SizeUTF8(selected, s, &w, NULL) != 0) return text(r, font, s, left + width / 2, y, c, CENTER);
    return text(r, font, s, left + (width - w) / 2, y, c, LEFT);
}

static int text_width(const struct mainui_font *font, const char *s) {
    if (mixed_text(font, s)) return mixed_width(font, s);
    int w = 0, h = 0;
    TTF_Font *selected = s && s[0] ? font_for_text(font, s, NULL) : font->font;
    if (selected) TTF_SizeUTF8(selected, s, &w, &h);
    return w;
}

// Draws text centered in a box the way the official lays it out, which puts odd leftovers on
// the right: settings values use the stock field's x=512, width=462 rectangle.
static void text_box(struct mainui_renderer *r, const struct mainui_font *font, const char *s, int box_x, int box_w, int y, SDL_Color c) {
    text(r, font, s, box_x + (box_w - text_width(font, s)) / 2, y, c, LEFT);
}

// Copies s into out, shortened with "..." at a character boundary to fit max_width (the
// official lists end long names so, e20).
static const char *shorten(const struct mainui_font *font, const char *s, int max_width, char *out, size_t size) {
    snprintf(out, size, "%s", s ? s : "");
    if (text_width(font, out) <= max_width) return out;
    size_t n = strlen(out);
    while (n > 0) {
        do --n; while (n > 0 && ((unsigned char)out[n] & 0xc0) == 0x80);
        if (n + 4 > size) continue;
        memcpy(out + n, "...", 4);
        if (text_width(font, out) <= max_width) return out;
    }
    return out;
}

// Copies s into line with a line break the language puts in a text for the stock tile (Portuguese
// "Mapa de\n teclado") made one space, with the spaces around it: BrickOS never draws it as a glyph.
static void join_line(const char *s, char *line, size_t size) {
    size_t n = 0;
    for (const char *p = s ? s : ""; *p && n + 1 < size; ++p) {
        if (*p != '\n') {
            line[n++] = *p;
            continue;
        }
        while (n > 0 && line[n - 1] == ' ') --n;
        line[n++] = ' ';
        while (p[1] == ' ') ++p;
    }
    line[n] = 0;
}

// As shorten, for the BrickOS labels drawn on one line (rows and titles).
static const char *shorten_line(const struct mainui_font *font, const char *s, int max_width, char *out, size_t size) {
    char line[512];
    join_line(s, line, sizeof(line));
    return shorten(font, line, max_width, out, size);
}

// The first n bytes of s, as much of them as out holds.
static void copy_start(char *out, size_t size, const char *s, size_t n) {
    if (!size) return;
    if (n >= size) n = size - 1;
    memcpy(out, s, n);
    out[n] = 0;
}

// A BrickOS card or tile name on up to two lines of max_width, its language line break joined as
// shorten_line does. Text that fits stays whole in first. Otherwise first ends at the last space
// of the longest start that fits (at least one character), or inside the word when that start has
// no space; second takes the rest, shortened with "..." when it is still too wide. Returns the
// number of lines.
static int two_lines(const struct mainui_font *font, const char *s, int max_width, char *first, char *second,
                     size_t size) {
    char line[512], probe[512];
    join_line(s, line, sizeof(line));
    second[0] = 0;
    copy_start(first, size, line, strlen(line));
    if (text_width(font, line) <= max_width) return 1;
    // The ends of the characters, as byte lengths: the longest start that fits by binary search.
    int ends[512], count = 0;
    size_t length = strlen(line);
    for (size_t i = 1; i <= length; ++i)
        if (i == length || ((unsigned char)line[i] & 0xc0) != 0x80) ends[count++] = (int)i;
    int low = 1, high = count - 1;
    while (low < high) {
        int middle = (low + high + 1) / 2;
        memcpy(probe, line, (size_t)ends[middle - 1]);
        probe[ends[middle - 1]] = 0;
        if (text_width(font, probe) <= max_width) low = middle;
        else high = middle - 1;
    }
    int cut = count > 1 ? ends[low - 1] : (int)length, end = cut;
    if (line[cut] != ' ') {
        int space = cut;
        while (space > 0 && line[space - 1] != ' ') --space;
        int before = space - 1;
        while (before > 0 && line[before - 1] == ' ') --before;
        if (space > 0 && before > 0) end = before;
    }
    while (end > 0 && line[end - 1] == ' ') --end;
    if (end == 0) end = cut;
    const char *rest = line + end;
    while (*rest == ' ') ++rest;
    copy_start(first, size, line, (size_t)end);
    if (!*rest) return 1;
    shorten(font, rest, max_width, second, size);
    return 2;
}

// A list name cut the way the official list drawer cuts it (static: getTruncateLen): glyph
// advances, plus 1.7 for each code point below 0x81, add up until they reach 90% of the text
// rectangle's width less 20; the code points counted by then stay and "..." follows. A name that
// never reaches it stays whole. Numbered rows have a rectangle of 984 - 120 pixels.
#define MAINUI_NUMBERED_TEXT_W (984 - 120)
static const char *truncate_like_stock(const struct mainui_font *font, const char *s, int rect_w, char *out, size_t size) {
    snprintf(out, size, "%s", s ? s : "");
    TTF_Font *selected = font_for_text(font, s, NULL);
    if (!selected) return out;
    float limit = (float)((rect_w - 20) * 9 / 10), sum = 0;
    for (size_t i = 0; out[i];) {
        if (sum >= limit) {
            if (i + 4 <= size) memcpy(out + i, "...", 4);
            return out;
        }
        // One UTF-8 code point.
        unsigned char c = (unsigned char)out[i];
        unsigned cp = c;
        size_t len = c < 0x80 ? 1 : c < 0xe0 ? 2 : c < 0xf0 ? 3 : 4;
        if (len > 1) cp = c & (0x3f >> (len - 1));
        for (size_t k = 1; k < len && out[i + k]; ++k) cp = (cp << 6) | ((unsigned char)out[i + k] & 0x3f);
        int advance = 0;
        TTF_GlyphMetrics(selected, (Uint16)(cp & 0xffff), NULL, NULL, NULL, NULL, &advance);
        sum += (float)advance + (cp < 0x81 ? 1.7f : 0.0f);
        for (size_t k = 0; k < len && out[i]; ++k) ++i;
    }
    return out;
}

// A string of the loaded language, or the English fallback.
static const char *say(const struct mainui_frontend *fe, int index, const char *fallback) {
    const char *s = mainui_lang_get(&fe->lang, index);
    return s ? s : fallback;
}

// ---- Frame parts ---------------------------------------------------------------------------
// Text tops below put the glyphs where the captures have them: a 36-pixel line's capitals
// start 14 pixels under its top, a 24-pixel line's digits 9, and a 32-pixel line's 12.

// A home tab's background picture, or the theme's when there is none or it does not load
// (static: draw_back_internal falls back to skin/bg.png).
static struct mainui_render_image *backdrop_image(struct mainui_renderer *r, const char *path,
                                                  struct mainui_render_image *theme) {
    struct mainui_render_image *img = path[0] == '/' ? image(r, path) : NULL;
    return img ? img : theme;
}

// The background, stretched over the screen; a new one fades in over the last for 500 ms.
static void background(struct mainui_renderer *r, const struct mainui_frontend *fe, uint64_t now_ms) {
    struct mainui_render_image *theme = skin(r, "bg.png");
    struct mainui_render_image *bg = backdrop_image(r, fe->backdrop.path, theme);
    int alpha = mainui_frontend_backdrop_alpha(fe, now_ms);
    struct mainui_render_image *from = alpha < 255 ? backdrop_image(r, fe->backdrop.from, theme) : NULL;
    if (from && bg && from != bg) {
        draw_stretch(r, from, 0, 0, MAINUI_SCREEN_W, MAINUI_SCREEN_H);
        SDL_BlendMode mode;
        Uint8 old_alpha;
        SDL_GetTextureBlendMode(bg->texture, &mode);
        SDL_GetTextureAlphaMod(bg->texture, &old_alpha);
        SDL_SetTextureBlendMode(bg->texture, SDL_BLENDMODE_BLEND);
        SDL_SetTextureAlphaMod(bg->texture, (Uint8)alpha);
        draw_stretch(r, bg, 0, 0, MAINUI_SCREEN_W, MAINUI_SCREEN_H);
        SDL_SetTextureAlphaMod(bg->texture, old_alpha);
        SDL_SetTextureBlendMode(bg->texture, mode);
    } else if (bg) {
        draw_stretch(r, bg, 0, 0, MAINUI_SCREEN_W, MAINUI_SCREEN_H);
    } else {
        fill(r, 0, 0, MAINUI_SCREEN_W, MAINUI_SCREEN_H, (SDL_Color){20, 70, 82, 255});
    }
    // Themes that have them draw a title bar and a hint bar on every screen (f04); Default has
    // neither.
    draw_at(r, skin(r, "title-bg.png"), 0, 0);
    draw_at(r, skin(r, "tips-bar-bg.png"), 0, 712);
}

// A status icon, centered in a 33x32 slot at y 19 (static: draw_bar_icon; the slot would take
// the size of nav_item_bg, which no theme sets).
#define STATUS_SLOT_W 33
static void status_icon(struct mainui_renderer *r, const char *name, int slot_x) {
    struct mainui_render_image *icon = skin(r, name);
    if (icon) draw_at(r, icon, slot_x + (STATUS_SLOT_W - icon->w) / 2, 19 + (32 - icon->h) / 2);
}

// Right to left from x 1004, counting off the pixels taken (static: draw_nav_bar and
// draw_battery_icon): the clock, 10 pixels, and without the 24-hour clock "AM" or "PM" 5 pixels
// left of it and 20 more; the charging bolt 16 pixels in and the battery 4 pixels past it; the
// percentage; then 5 pixels, the speaker, and the other icons, each slot 10 pixels past the one
// before. The percentage sits in a box of fixed width (mainui_status_percent_box) 16 pixels left
// of the battery's slot, and the battery takes its slot, 10, the box, and 12: so the icons do not
// move with the text's width, and in a large font "100%" runs over the battery (Syber, round 7).
// The clock and the percentage take nav_color3 (static: draw_nav_bar draws them in
// gNavColorIcon), which is white without a theme value (r9, r3) and half white in Syber Dark
// (f04), whose stat_color1 is white. Their size is nav_font2 (32 in Syber, round 7).
static void status_bar(struct mainui_renderer *r, const struct mainui_frontend *fe) {
    char clock[16], battery[16];
    const char *half = mainui_status_clock(r->status.clock, fe->settings.values[MAINUI_SET_24HOURCLOCK], clock, sizeof(clock));
    int off = text(r, &r->status_text, clock, 1004, 18, r->status_color, RIGHT);
    if (half) off += 5 + text(r, &r->status_text, half, 1004 - off - 5, 18, r->status_color, RIGHT) + 20;
    else off += 10;
    int slot = 1004 - off;
    if (r->status.charging) {
        status_icon(r, "ic-charging.png", slot - 16);
        slot -= 20;
    }
    slot -= STATUS_SLOT_W;
    status_icon(r, mainui_status_battery_icon(r->status.battery), slot);
    int box = mainui_status_percent_box(r->status.battery);
    snprintf(battery, sizeof(battery), "%d%%", r->status.battery);
    text(r, &r->status_text, battery, slot - 16 - box, 18, r->status_color, LEFT);
    off += STATUS_SLOT_W + 10 + box + 12 + (r->status.charging ? 20 : 0);
    // The mute icon shows at volume 0 (static: draw_nav_bar tests keymon's volume; it showed the
    // speaker with system.json mute 1).
    status_icon(r, fe->settings.values[MAINUI_SET_VOL] == 0 ? "ic-sound-mute.png" : "ic-sound-n.png", 1004 - off - 5 - STATUS_SLOT_W);
    off += STATUS_SLOT_W + 10;
    // Then the headphones while /dev/dsp1 can be read; the hotspot, or the Wi-Fi connection's
    // signal (g10, c1); then the Bluetooth devices (c4): the gamepad of a connected input device,
    // and for the other connected devices the headphones when one of them is an audio device,
    // else the plain Bluetooth icon.
    if (fe->dsp1) {
        status_icon(r, "icon-bluetooth-headphone.png", 1004 - off - STATUS_SLOT_W);
        off += STATUS_SLOT_W + 10;
    }
    char name[32];
    const char *network = NULL;
    if (fe->settings.values[MAINUI_SET_HOSTAP]) {
        network = "icon-hotspot.png";
    } else if (fe->settings.values[MAINUI_SET_WIFI] && strcmp(fe->wifi_status.state, "COMPLETED") == 0) {
        snprintf(name, sizeof(name), "ic-wifi-0%d.png", mainui_wifi_bar_level(fe->wifi_rssi));
        network = name;
    }
    if (network) {
        status_icon(r, network, 1004 - off - STATUS_SLOT_W);
        off += STATUS_SLOT_W + 10;
    }
    if (!fe->settings.values[MAINUI_SET_BLUETOOTH]) return;
    int input = 0, others = 0;
    for (int i = 0; i < fe->paired_count; ++i) {
        if (!fe->paired[i].connected) continue;
        if (!input && strstr(fe->paired[i].icon, "input")) input = 1;
        else ++others;
    }
    if (input) {
        status_icon(r, "icon-bluetooth-gamepad.png", 1004 - off - STATUS_SLOT_W);
        off += STATUS_SLOT_W + 10;
    }
    if (others)
        status_icon(r, mainui_frontend_audio_device(fe) ? "icon-bluetooth-headphone.png" : "icon-bluetooth.png", 1004 - off - STATUS_SLOT_W);
}

// The page title: the TriMUI mark on tab pages, the back arrow on screens opened from them.
static void header(struct mainui_renderer *r, const char *title, int back) {
    if (back) {
        draw_at(r, skin(r, "icon-back.png"), 20, 7);
        draw_text(r, &r->content, title, 110, 0, 70, white, LEFT);
    } else {
        struct mainui_render_image *icon = skin(r, "icon-trimui.png");
        if (icon) draw_at(r, icon, 20, 15);
        else fill(r, 22, 18, 36, 36, green);
        draw_text(r, &r->content, title, 80, 0, 70, white, LEFT);
    }
}

// A bottom hint: the tips-<key>.png button and its label.
struct hint {
    const char *key;
    const char *label;
};

static void hints(struct mainui_renderer *r, const struct mainui_frontend *fe,
                  const struct hint *list, int count, const char *counter) {
    int x = 30;
    for (int i = 0; i < count; ++i) {
        char name[64];
        const char *key = list[i].key;
        if (fe->settings.values[MAINUI_SET_BCONFIRM]) {
            if (!strcmp(key, "A")) key = "B";
            else if (!strcmp(key, "B")) key = "A";
        }
        snprintf(name, sizeof(name), "tips-%s.png", key);
        struct mainui_render_image *icon = skin(r, name);
        // The button at its own size, centered on the hint line (32 px in Default, 34 in Syber Dark).
        if (icon) draw_at(r, icon, x, 742 - icon->h / 2);
        else text(r, &r->small, key, x + 16, 725, green, CENTER);
        x += (icon ? icon->w : 32) + 10;
        x += text(r, &r->stat, list[i].label, x, 725, white, LEFT) + 24;
    }
    // The page counter keeps the Default top in every font, where the hint labels move with the
    // font's height (f04, f05).
    struct mainui_font top = r->stat;
    top.shift = 0;
    if (counter) text(r, &top, counter, 1000, 727, white, RIGHT);
}

static void focus_box(struct mainui_renderer *r, int x, int y, int w, int h, int focused) {
    if (focused) {
        fill(r, x, y, w, h, (SDL_Color){16, 150, 116, 235});
        outline(r, x, y, w, h, 3, green);
    } else {
        fill(r, x, y, w, h, (SDL_Color){255, 255, 255, 34});
        outline(r, x, y, w, h, 2, (SDL_Color){255, 255, 255, 60});
    }
}

// A focused or plain skin at its own size, or the plain box when the theme has neither.
static void skin_box(struct mainui_renderer *r, const char *focused_name, const char *normal_name, int x, int y, int w, int h,
                     int focused) {
    struct mainui_render_image *img = skin(r, focused ? focused_name : normal_name);
    if (img) draw_at(r, img, x, y);
    else focus_box(r, x, y, w, h, focused);
}

// A one-line list row's skin: at the row's left, centered in the 107-pixel row pitch that starts
// 3 pixels above the 100-pixel row. Energy's and Green's focused row, 992x108, sits 3 pixels up
// (round 7: its frame at 20..1011 and 70..177); a 100-pixel skin sits on the row.
static void row_skin(struct mainui_renderer *r, int x, int base, int focused) {
    struct mainui_render_image *img = skin(r, focused ? "list-item-1line-sort-bg-f.png" : "list-item-1line-sort-bg-n.png");
    if (img) draw_at(r, img, x, base - 3 + (107 - img->h) / 2);
    else focus_box(r, x, base, 984, 100, focused);
}

static const char *tab_name(const struct mainui_frontend *fe, enum mainui_tab tab) {
    switch (tab) {
    case MAINUI_TAB_FAVORITES: return say(fe, 1, "Favorites");
    case MAINUI_TAB_RECENT: return say(fe, 18, "Recent");
    case MAINUI_TAB_BESTS: return say(fe, 140, "Bests");
    case MAINUI_TAB_GAMES: return say(fe, 0, "Games");
    case MAINUI_TAB_PORTS: return say(fe, 196, "Ports");
    case MAINUI_TAB_APPS: return say(fe, 2, "Apps");
    case MAINUI_TAB_NETPLAY: return say(fe, 118, "Netplay");
    case MAINUI_TAB_SETTINGS: return say(fe, 15, "Settings");
    default: return "";
    }
}

// The tab strip shown while the tab bar has the focus (n03, f04): five names at a time, the
// window that holds the focused tab (Favorites to Netplay with Games focused, Recent to Settings
// with Settings focused), 30 apart and centered on x 492, the focused one in the main-nav-sel
// pill. As the official lays them out (static: TabMenuWindow::draw), the space between the L and R
// buttons is 984 less their widths, the names start half the space left over after the L button,
// and 40 after it when they do not fit (Japanese, f05; Russian, round 8); French centers from x 68
// (round 8). The L button is drawn first and the R button last, so French names cover L and
// Russian ones pass under R, uncut. Open: whether the window only scrolls when the focus leaves it.
static void tab_strip(struct mainui_renderer *r, const struct mainui_frontend *fe) {
    enum mainui_tab tabs[MAINUI_TAB_COUNT];
    int n = mainui_model_visible_tabs(&fe->model, tabs), ws[MAINUI_TAB_COUNT], focus = 0;
    for (int i = 0; i < n; ++i) {
        ws[i] = text_width(&r->nav, tab_name(fe, tabs[i]));
        if (tabs[i] == fe->model.tab) focus = i;
    }
    int first = focus > 4 ? focus - 4 : 0;
    if (first > n - 5) first = n > 5 ? n - 5 : 0;
    int last = first + 5 < n ? first + 5 : n, span = 0;
    for (int i = first; i < last; ++i) span += ws[i] + (i > first ? 30 : 0);
    struct mainui_render_image *l_button = skin(r, "tips-L.png"), *r_button = skin(r, "tips-R.png");
    int lw = l_button ? l_button->w : 55, rw = r_button ? r_button->w : 55;
    int margin = (984 - lw - rw - span) / 2;
    if (margin < 0) margin = 40;
    int tx = margin + lw;
    draw_at(r, l_button, 20, 17);
    for (int i = first; i < last; ++i) {
        int focused = i == focus;
        if (focused) {
            struct mainui_render_image *left = skin(r, "main-nav-sel-left.png"), *mid = skin(r, "main-nav-sel-mid.png"),
                                       *right = skin(r, "main-nav-sel-right.png");
            if (left && mid && right) {
                int x = tx - 17;
                draw_at(r, left, x, 0);
                x += left->w;
                // Stock tiles complete center pieces rather than stretching them.
                // Integer division also retains the measured odd-label endpoint.
                int pieces = mid->w > 0 ? (ws[i] + 36 - left->w - right->w + mid->w) / mid->w : 0;
                for (int piece = 0; piece < pieces; ++piece) {
                    draw_at(r, mid, x, 0);
                    x += mid->w;
                }
                draw_at(r, right, x, 0);
            } else {
                fill(r, tx - 8, 9, ws[i] + 19, 52, (SDL_Color){16, 150, 116, 255});
            }
        }
        text(r, &r->nav, tab_name(fe, tabs[i]), tx, 12, focused ? r->nav_focus : r->nav_dim, LEFT);
        tx += ws[i] + 30;
    }
    draw_at(r, r_button, 1004 - rw, 17);
}

// ---- Tab pages -----------------------------------------------------------------------------

static const char *const setting_icons[MAINUI_SETTINGS_TILES] = {
    "ic-display", "ic-volume", "ic-wifi", "ic-hotspot", "ic-bluetooth", "ic-theme", "ic-keymap", "ic-homepage", "ic-system",
};
static const int setting_labels[MAINUI_SETTINGS_TILES] = {142, 24, 111, 114, 117, 108, 22, 211, 143};
static const char *const setting_names[MAINUI_SETTINGS_TILES] = {
    "Display", "Volume", "Wi-Fi", "Hotspot", "Bluetooth", "Themes", "Keymap", "Homepage", "System",
};

// ---- Settings extensions -------------------------------------------------------------------

// A device path from a registry path, which carries the frontend's root.
static const char *ext_device_path(const struct mainui_frontend *fe, const char *path) {
    size_t root = strlen(fe->ext.root);
    return root && strncmp(path, fe->ext.root, root) == 0 ? path + root : path;
}

// An extension tile's own icon, or NULL for the generic one.
static const char *ext_tile_icon(const struct mainui_frontend *fe, int tile) {
    int e = mainui_ext_tile_extension(&fe->ext, tile);
    return e >= 0 && fe->ext.extensions[e].icon[0] ? ext_device_path(fe, fe->ext.extensions[e].icon) : NULL;
}

static const char *ext_label(struct mainui_frontend *fe, int node, char *out, size_t size) {
    if (mainui_ext_text(&fe->ext, node, MAINUI_EXT_TEXT_LABEL, out, size) < 0 && size) out[0] = 0;
    return out;
}

static int language_is_portuguese(const struct mainui_frontend *fe);
static const char *mapping_text(const struct mainui_frontend *fe, const char *english);
#define MAPPING_SAVE_FAILED_TEXT "Could not save the button mapping.\nPrevious mapping kept."

// A row's value as text: a toggle's state, a select's choice, a range's number, an info row's
// value, or "..." while its script runs.
static const char *ext_value(struct mainui_frontend *fe, int node, int brickos, char *out, size_t size) {
    const struct mainui_ext_node *n = mainui_ext_node(&fe->ext, node);
    if (size) out[0] = 0;
    if (!n) return out;
    if (n->busy) { snprintf(out, size, "..."); return out; }
    switch (n->kind) {
    case MAINUI_EXT_TOGGLE:
        if (brickos) snprintf(out, size, "%s", n->value ? (language_is_portuguese(fe) ? "Ligado" : "On") :
                                                       (language_is_portuguese(fe) ? "Desligado" : "Off"));
        else snprintf(out, size, "%s", n->value ? say(fe, 104, "On") : say(fe, 105, "Off"));
        break;
    case MAINUI_EXT_SELECT: mainui_ext_text(&fe->ext, node, MAINUI_EXT_TEXT_ITEM, out, size); break;
    case MAINUI_EXT_RANGE: snprintf(out, size, "%d", n->value); break;
    case MAINUI_EXT_INFO: mainui_ext_text(&fe->ext, node, MAINUI_EXT_TEXT_VALUE, out, size); break;
    default: break;
    }
    return out;
}

// The focused extension row's description, or NULL.
static const char *ext_description(struct mainui_frontend *fe, const struct mainui_item *items, int count, int focus,
                                   char *out, size_t size) {
    if (focus < 0 || focus >= count || items[focus].kind != MAINUI_ITEM_EXTENSION) return NULL;
    if (mainui_ext_text(&fe->ext, items[focus].action, MAINUI_EXT_TEXT_DESCRIPTION, out, size) < 0 || !out[0]) return NULL;
    return out;
}

// The title of a Settings screen; an extension screen takes its menu's label.
static const char *screen_title(const struct mainui_frontend *fe, enum mainui_screen screen);
static const char *settings_title(struct mainui_frontend *fe, const struct mainui_window *w) {
    static char title[256];
    if (w->screen == MAINUI_SCREEN_EXTENSION) return ext_label(fe, w->index, title, sizeof(title));
    return screen_title(fe, w->screen);
}

// What an extension's output view says about its script: running, done, or failed.
static const char *ext_view_state(const struct mainui_frontend *fe, char *out, size_t size) {
    int portuguese = language_is_portuguese(fe);
    if (fe->ext_view_status < 0) snprintf(out, size, "%s", portuguese ? "A correr..." : "Running...");
    else if (fe->ext_view_status == 0) snprintf(out, size, "%s", portuguese ? "Concluído" : "Done");
    else snprintf(out, size, portuguese ? "Falhou (estado %d)" : "Failed (status %d)", fe->ext_view_status);
    return out;
}

// A grid tile's picture and labels, for the Games, Apps, and Settings tabs.
static void tile_content(struct mainui_renderer *r, struct mainui_frontend *fe, int index, int focused,
                         struct mainui_render_image **picture, const char **label, char *count, size_t count_size) {
    char path[PATH_BYTES];
    *picture = NULL;
    *label = "";
    count[0] = 0;
    if ((fe->model.tab == MAINUI_TAB_GAMES && index < fe->visible_count) ||
        (fe->model.tab == MAINUI_TAB_BESTS && index < fe->shown_best_count)) {
        const struct mainui_system *s = mainui_frontend_system(fe, index +
            (fe->model.tab == MAINUI_TAB_BESTS ? MAINUI_BEST_SYSTEM_BASE : 0));
        if (s->icontop[0]) {
            snprintf(path, sizeof(path), "%s/%s", s->directory, s->icontop);
            *picture = system_icon(r, path);
        }
        *label = s->label;
        // The official grid shows a count only for systems it has scanned (with a list cache); BrickOS
        // counts every system (a stock defect fixed). An empty system shows none (k2i: Arcade after
        // its empty list was opened).
        int games = mainui_frontend_game_count(fe, index +
            (fe->model.tab == MAINUI_TAB_BESTS ? MAINUI_BEST_SYSTEM_BASE : 0));
        if (games > 0) snprintf(count, count_size, say(fe, games == 1 ? 150 : 151, "%d Games"), games);
    } else if ((fe->model.tab == MAINUI_TAB_APPS && index < fe->shown_app_count) ||
               (fe->model.tab == MAINUI_TAB_PORTS && index < fe->shown_port_count)) {
        const struct mainui_app *app = fe->model.tab == MAINUI_TAB_PORTS ? &fe->ports[fe->shown_ports[index]]
                                                                         : &fe->apps[fe->shown_apps[index]];
        if (app->icon[0]) {
            snprintf(path, sizeof(path), "%s/%s", app->directory, app->icon);
            *picture = image(r, path);
        }
        // BrickOS takes nothing from the stock theme: its card draws its own generic icon instead.
        if (!*picture && r->presentation != MAINUI_PRESENTATION_MATERIAL_YOU) *picture = skin(r, "icon-app-default.png");
        // IconMenu draws installed App/Port icons with the stock texture's
        // default nearest sampling, including downscaled large icons.
        if (*picture) SDL_SetTextureScaleMode((*picture)->texture, SDL_ScaleModeNearest);
        *label = app->label;
    } else if (fe->model.tab == MAINUI_TAB_SETTINGS && index < MAINUI_SETTINGS_TILES) {
        char name[64];
        snprintf(name, sizeof(name), "%s%s.png", setting_icons[index], focused ? "" : "-n");
        *picture = skin(r, name);
        *label = say(fe, setting_labels[index], setting_names[index]);
    } else if (fe->model.tab == MAINUI_TAB_SETTINGS && index < mainui_model_settings_tiles(&fe->model)) {
        // A settings extension: its own icon for both focus states, else the generic silhouette
        // in white like the skin's Settings icons, else the skin's System pair. The labels
        // rotate so that every tile drawn keeps its own.
        static char titles[16][256];
        static int next;
        char *title = titles[next++ % 16];
        int tile = index - MAINUI_SETTINGS_TILES;
        mainui_ext_tile_title(&fe->ext, tile, title, sizeof(titles[0]));
        *label = title;
        const char *icon = ext_tile_icon(fe, tile);
        if (icon) *picture = image(r, icon);
        if (!*picture) *picture = image_resized_style(r, "/mnt/SDCARD/BrickOS/icons/settings/Extension.png",
                                                    240, 240, RESIZED_MONOCHROME);
        if (!*picture) *picture = skin(r, focused ? "ic-system.png" : "ic-system-n.png");
    }
}

// One 300x300 grid tile: its skin, the grid-item-poster panel, the picture, and the labels.
// The Apps tab shows no panel (e04 against n05 and e05).
// A tile picture (icontop) fits a 240-pixel box at the top of the tile; a plain icon (config
// "icon" alone: ports, some apps and systems) shows at its own size, centered in the tile
// (static: IconMenu::draw; p1: a port's 300-pixel icon at the tile's corner).
static void grid_tile(struct mainui_renderer *r, int x, int y, int focused, int poster, int plain,
                      struct mainui_render_image *picture, struct mainui_render_image *overlay, const char *label, const char *count) {
    skin_box(r, "grid-item-bg-f.png", "grid-item-bg-n.png", x, y, 300, 300, focused);
    if (poster) {
        struct mainui_render_image *panel = skin(r, "grid-item-poster.png");
        if (panel) draw_at(r, panel, x, y);
        else fill(r, x + 20, y + 20, 260, 200, (SDL_Color){255, 255, 255, 26});
    }
    draw_down(r, overlay, x, y, 300, 300);
    if (plain) draw_down(r, picture, x, y, 300, 300);
    else draw_down(r, picture, x + 30, y, 240, 240);
    const struct mainui_font *font = focused ? &r->focus_label : &r->content;
    if (label && strchr(label, '\n') && font->font) {
        // A label the language splits (Portuguese "Mapa de\n teclado"): the official raises the
        // label's box by 20 and draws each line in a row as high as the font plus a spacing of -8,
        // or -6 on the focused tile, each line centered in its row (static: IconMenu::draw and
        // DrawStringLinesSpacing; round 8: 43 pixels apart, the first 14 above a one-line label).
        char lines[256];
        snprintf(lines, sizeof(lines), "%s", label);
        struct mainui_font unshifted = *font;
        unshifted.shift = 0;
        int spacing = focused ? -6 : -8, pitch = TTF_FontHeight(font->font) + spacing, top = y + 215 + spacing / 2;
        for (char *line = strtok(lines, "\n"); line; line = strtok(NULL, "\n"), top += pitch)
            text_in(r, &unshifted, line, x, 300, top, white);
    } else {
        text_in(r, font, label, x, 300, y + 225, white);
    }
    if (count && count[0]) text(r, &r->small, count, x + 150, y + 263, r->dim, CENTER);
}

static int item_picture(const struct mainui_frontend *fe, int index, char *path, size_t size);

static struct mainui_render_image *app_icon_layer(struct mainui_renderer *r, const struct mainui_app *app) {
    if (!app || !app->icon_layer[0]) return NULL;
    char path[PATH_BYTES];
    snprintf(path, sizeof(path), "%s/%s", app->directory, app->icon_layer);
    return system_icon(r, path);
}

static struct mainui_render_image *tab_app_icon_layer(struct mainui_renderer *r, struct mainui_frontend *fe, int index) {
    if (fe->model.tab == MAINUI_TAB_APPS) return app_icon_layer(r, &fe->apps[fe->shown_apps[index]]);
    if (fe->model.tab == MAINUI_TAB_PORTS) return app_icon_layer(r, &fe->ports[fe->shown_ports[index]]);
    return NULL;
}

static void grid_page(struct mainui_renderer *r, struct mainui_frontend *fe, int n, int focus, int bar) {
    int page = focus / 6;
    for (int slot = 0; slot < 6; ++slot) {
        int index = page * 6 + slot;
        if (index >= n) break;
        int focused = !bar && index == focus;
        struct mainui_render_image *picture;
        const char *label;
        char count[64];
        char path[PATH_BYTES];
        tile_content(r, fe, index, focused, &picture, &label, count, sizeof(count));
        int plain = item_picture(fe, index, path, sizeof(path));
        // Apps and Ports tiles have no poster panel (e04, p1: a port icon brings its own).
        int poster = fe->model.tab != MAINUI_TAB_APPS && fe->model.tab != MAINUI_TAB_PORTS;
        grid_tile(r, 42 + (slot % 3) * 320, 84 + (slot / 3) * 320, focused, poster, plain, picture,
                  tab_app_icon_layer(r, fe, index), label, count);
    }
}

// The compact layout: 100-pixel tiles, 8 a row, under a bar with the focused name (n06).
static void compact_page(struct mainui_renderer *r, struct mainui_frontend *fe, int n, int focus, int bar) {
    struct mainui_render_image *picture;
    const char *label;
    char count[64];
    tile_content(r, fe, focus, 1, &picture, &label, count, sizeof(count));
    draw_at(r, skin(r, "bg-grid2-title.png"), 78, 90);
    text(r, &r->content, label, 512, 85, white, CENTER);
    const int per_page = MAINUI_COMPACT_COLUMNS * MAINUI_COMPACT_ROWS;
    int first = focus / per_page * per_page, end = first + per_page < n ? first + per_page : n;
    for (int i = first; i < end; ++i) {
        int pos = i - first;
        int x = 77 + (pos % MAINUI_COMPACT_COLUMNS) * 110, y = 150 + (pos / MAINUI_COMPACT_COLUMNS) * 110;
        int focused = i == focus && !bar;
        tile_content(r, fe, i, focused, &picture, &label, count, sizeof(count));
        // The compact tiles use the flow-item skins, without the grid tile panel (n06).
        struct mainui_render_image *tile = skin(r, focused ? "flow-item-bg-f.png" : "flow-item-bg-n.png");
        if (tile) draw_stretch(r, tile, x, y, 100, 100);
        else focus_box(r, x, y, 100, 100, focused);
        draw_fit(r, tab_app_icon_layer(r, fe, i), x, y, 100, 100);
        draw_fit(r, picture, x, y, 100, 100);
    }
}

// A skin image resized the official way, or NULL.
static struct mainui_render_image *skin_resized(struct mainui_renderer *r, const char *name, int w, int h) {
    char relative[256], path[PATH_BYTES];
    snprintf(relative, sizeof(relative), "skin/%s", name);
    if (mainui_asset_find(r->fs, r->theme, relative, path, sizeof(path)) < 0) return NULL;
    return image_resized(r, path, w, h);
}

// The picture of a Games or Apps item: its path, and 1 when it is a plain icon, which the item
// has in place of a tile picture (icontop).
static int item_picture(const struct mainui_frontend *fe, int index, char *path, size_t size) {
    path[0] = 0;
    if ((fe->model.tab == MAINUI_TAB_GAMES && index < fe->visible_count) ||
        (fe->model.tab == MAINUI_TAB_BESTS && index < fe->shown_best_count)) {
        const struct mainui_system *s = mainui_frontend_system(fe, index +
            (fe->model.tab == MAINUI_TAB_BESTS ? MAINUI_BEST_SYSTEM_BASE : 0));
        if (s->icontop[0]) snprintf(path, size, "%s/%s", s->directory, s->icontop);
        return s->icon_only;
    }
    if ((fe->model.tab == MAINUI_TAB_APPS && index < fe->shown_app_count) ||
        (fe->model.tab == MAINUI_TAB_PORTS && index < fe->shown_port_count)) {
        const struct mainui_app *app = fe->model.tab == MAINUI_TAB_PORTS ? &fe->ports[fe->shown_ports[index]]
                                                                         : &fe->apps[fe->shown_apps[index]];
        if (app->icon[0]) snprintf(path, size, "%s/%s", app->directory, app->icon);
        return app->icon_only;
    }
    return 0;
}

// Draws an image stretched with its opacity scaled, then gives the shared texture back its own.
static void draw_stretch_faded(struct mainui_renderer *r, struct mainui_render_image *img, int x, int y, int w, int h,
                               float opacity) {
    if (!img || opacity <= 0) return;
    SDL_BlendMode mode;
    Uint8 alpha;
    SDL_GetTextureBlendMode(img->texture, &mode);
    SDL_GetTextureAlphaMod(img->texture, &alpha);
    SDL_SetTextureBlendMode(img->texture, SDL_BLENDMODE_BLEND);
    SDL_SetTextureAlphaMod(img->texture, (Uint8)lroundf(255 * fminf(opacity, 1.f)));
    draw_stretch(r, img, x, y, w, h);
    SDL_SetTextureAlphaMod(img->texture, alpha);
    SDL_SetTextureBlendMode(img->texture, mode);
}

static SDL_Color mix_color(SDL_Color from, SDL_Color to, float amount) {
    return (SDL_Color){(Uint8)lroundf(from.r + (to.r - from.r) * amount), (Uint8)lroundf(from.g + (to.g - from.g) * amount),
                       (Uint8)lroundf(from.b + (to.b - from.b) * amount), (Uint8)lroundf(from.a + (to.a - from.a) * amount)};
}

// A one-row card `slot` cards from the center, a fraction while the row slides: 500 pixels at
// the center and 400 from one card off, 500 pixels apart center to center, so slots -1, 0 and 1
// are the stock tiles at x -188, 262 and 812. off is how far it is from the center, 0 to 1.
struct row_card { int x, y, size; float off; };
static struct row_card row_card(float slot) {
    float off = fminf(fabsf(slot), 1.f);
    int size = (int)lroundf(500 - 100 * off), center = (int)lroundf(512 + 500 * slot);
    return (struct row_card){center - size / 2, (int)lroundf(134 + 40 * off), size, off};
}

// How many cards the row still stands off its places: 0 at rest, in the tab bar and in static
// captures. The slots k + shift under 1.5 cards from the center reach the screen.
static float row_shift(const struct mainui_renderer *r, const struct mainui_frontend *fe, int bar) {
    return r->animations_enabled && !bar ? mainui_frontend_carousel_shift(fe, r->frame_ms) : 0;
}

// The one-row layout (menustylel1 0; r9): the focused item in a 500-pixel flow tile at the
// center, and its neighbors in 400-pixel tiles 50 pixels away, wrapping at the ends (static:
// IconMenu::draw lays the tiles at y 134 and 174). A tile picture shows at 370 in the focused
// tile and at 300 in the others; a plain icon covers the whole tile (r9: EmulationStation; p2: a
// port). Games tiles have the poster panel, Apps and Ports tiles none. The names are under the
// tiles, the neighbors' at half opacity, and there is no page counter. While the tab bar has the
// focus, the three tiles are 400 pixels, 450 apart around x 312 (r10).
// Bests uses its own masks and fits art over the entire tile (IconMenu's plain flag),
// followed by the optional config icon. Keep one surviving item centered too: stock
// initializes local focus to zero in that case, clipping the selected tile at x=-188.
// The stock row jumps to a new focus; BrickOS slides it there with the background's fade
// (mainui_frontend_motion). The frames at rest are the stock's.
static void bests_row(struct mainui_renderer *r, struct mainui_frontend *fe, int n, int focus, int bar) {
    float shift = row_shift(r, fe, bar);
    for (int k = -3; k <= 3; ++k) {
        float slot = (float)k + shift;
        if (fabsf(slot) >= 1.5f || (bar && (k < -1 || k > 1))) continue;
        int index = ((focus + k) % n + n) % n;
        // While the row slides, a card leaves or takes the center by degrees: its mask fades in
        // as it moves off.
        struct row_card card = bar ? (struct row_card){312 + k * 450, 174, 400, 1} : row_card(slot);
        int centered = card.off == 0, size = card.size, y = card.y, x = card.x;
        const struct mainui_system *s = mainui_frontend_system(fe, MAINUI_BEST_SYSTEM_BASE + index);
        if (!s) continue;
        if (bar) draw_stretch(r, skin(r, k == 0 ? "best-mask-selected.png" : "best-mask-unselected.png"), x, y, size, size);
        else if (shift != 0) draw_stretch_faded(r, skin(r, "best-mask-unselected.png"), x, y, size, size, card.off);
        else if (!centered) draw_stretch(r, skin(r, "best-mask-unselected.png"), x, y, size, size);
        // The catalog folds icon into icontop for ordinary Games tiles. Bests instead
        // always keeps the stock poster underneath the art and separate config icon.
        draw_fit(r, skin(r, "flow-item-poster.png"), x, y, size, size);
        const char *paths[] = {s->icon_only ? "" : s->icontop, s->icon};
        for (int layer = 0; layer < 2; ++layer) {
            char path[PATH_BYTES];
            if (!paths[layer][0]) continue;
            if (paths[layer][0] == '/') snprintf(path, sizeof(path), "%s", paths[layer]);
            else snprintf(path, sizeof(path), "%s/%s", s->directory, paths[layer]);
            draw_fit(r, system_icon(r, path), x, y, size, size);
        }
        draw_text(r, &r->content, s->label, x + size / 2, (int)lroundf(654 - 50 * card.off), 30,
                  bar ? (k == 0 ? white : r->dim) : mix_color(white, r->dim, card.off), CENTER);
    }
}

static void row_page(struct mainui_renderer *r, struct mainui_frontend *fe, int n, int focus, int bar) {
    if (n <= 0) return;
    if (fe->model.tab == MAINUI_TAB_BESTS) { bests_row(r, fe, n, focus, bar); return; }
    // Games tiles have the poster panel; Apps and Ports tiles none (r9, p2).
    int poster = fe->model.tab == MAINUI_TAB_GAMES;
    float shift = row_shift(r, fe, bar);
    for (int k = -3; k <= 3; ++k) {
        float slot = (float)k + shift;
        if (fabsf(slot) >= 1.5f || (bar && (k < -1 || k > 1))) continue;
        int index = ((focus + k) % n + n) % n;
        struct row_card card = bar ? (struct row_card){312 + k * 450, 174, 400, 1} : row_card(slot);
        int centered = card.off == 0, size = card.size, y = card.y, x = card.x;
        struct mainui_render_image *picture;
        const char *label;
        char count[64], path[PATH_BYTES];
        tile_content(r, fe, index, centered, &picture, &label, count, sizeof(count));
        int plain = item_picture(fe, index, path, sizeof(path));
        int direct_icon = fe->model.tab == MAINUI_TAB_APPS || fe->model.tab == MAINUI_TAB_PORTS;
        if (shift != 0) {
            // While the row slides, every size is in between: the skins cross-fade, and the
            // poster and the picture scale on the GPU rather than being resized for each frame.
            int side = plain ? size : (int)lroundf(370 - 70 * card.off), inset = (size - side) / 2;
            struct mainui_render_image *focused_tile = skin(r, "flow-item-bg-f.png"), *tile = skin(r, "flow-item-bg-n.png");
            if (focused_tile || tile) {
                draw_stretch_faded(r, tile, x, y, size, size, card.off);
                draw_stretch_faded(r, focused_tile, x, y, size, size, 1 - card.off);
            } else {
                focus_box(r, x, y, size, size, card.off < 0.5f);
            }
            if (poster) draw_stretch(r, skin(r, "flow-item-poster.png"), x, y, size, size);
            draw_flow_icon(r, tab_app_icon_layer(r, fe, index), x, y, size, size);
            struct mainui_render_image *art = path[0] && !direct_icon ? image(r, path) : NULL;
            if (art) {
                SDL_SetTextureScaleMode(art->texture, SDL_ScaleModeLinear);
                draw_stretch(r, art, x + inset, y + inset, side, side);
            } else if (direct_icon) {
                draw_flow_icon(r, picture, x + inset, y + inset, side, side);
            } else {
                draw_fit(r, picture, x + inset, y + inset, side, side);
            }
            text_in(r, &r->content, label, x, size, (int)lroundf(644 - 50 * card.off), mix_color(white, r->dim, card.off));
            continue;
        }
        struct mainui_render_image *tile = skin(r, centered ? "flow-item-bg-f.png" : "flow-item-bg-n.png");
        if (tile) draw_stretch(r, tile, x, y, size, size);
        else focus_box(r, x, y, size, size, centered);
        if (poster) draw_stretch(r, centered ? skin(r, "flow-item-poster.png") : skin_resized(r, "flow-item-poster.png", 400, 400),
                                 x, y, size, size);
        int side = plain ? size : centered ? 370 : 300;
        struct mainui_render_image *art = path[0] && !direct_icon ? image_resized(r, path, side, side) : NULL;
        draw_flow_icon(r, tab_app_icon_layer(r, fe, index), x, y, size, size);
        if (art) draw_at(r, art, x + (size - side) / 2, y + (size - side) / 2);
        else if (direct_icon) draw_flow_icon(r, picture, x + (size - side) / 2, y + (size - side) / 2, side, side);
        else draw_fit(r, picture, x + (size - side) / 2, y + (size - side) / 2, side, side);
        text_in(r, &r->content, label, x, size, centered ? 644 : 594, k == 0 ? white : r->dim);
    }
}

// The Recent and Favorites tabs: box art and name; Recent adds the system and the favorite
// star (n04), Favorites shows the name alone (n03).
static void entry_list(struct mainui_renderer *r, struct mainui_frontend *fe, int recent, int focus, int first, int bar) {
    char name[512];
    int n = recent ? fe->recent_count : fe->favorites_count;
    // An empty Recent or Favorites page says None, centred on the screen in the list font
    // (static: TabMenuWindow::draw draws it into {0, 0, 1024, 768} when the page has no items).
    if (!n) {
        draw_text(r, &r->content, say(fe, 144, "None"), MAINUI_SCREEN_W / 2, 0, MAINUI_SCREEN_H, white, CENTER);
        return;
    }
    for (int row = 0; row < MAINUI_TAB_LIST_ROWS && first + row < n; ++row) {
        int index = first + row, base = 70 + row * 130, focused = !bar && index == focus;
        const struct mainui_list_entry *e = recent ? mainui_frontend_recent(fe, index) : mainui_frontend_favorite(fe, index);
        struct mainui_render_image *bg = skin(r, focused ? "list-item-2line-long-bg-f.png" : "list-item-2line-long-bg-n.png");
        if (bg) draw_at(r, bg, 20, base + (128 - bg->h) / 2);  // Default: 128 focused, 120 plain
        else focus_box(r, 20, base + 2, 984, 124, focused);
        // Box art is resized to 90 x 90 the official way (static: TextMenuDrawer loads it sized).
        // An app entry has no picture of its own; it shows its app's icon (r6t: EmulationStation
        // and File Manager in Recent).
        struct mainui_render_image *art = image_resized(r, e->imgpath, 90, 90);
        const struct mainui_app *app = art ? NULL : mainui_frontend_entry_app(fe, e);
        if (app && app->icon[0]) {
            char icon[PATH_BYTES];
            snprintf(icon, sizeof(icon), "%s/%s", app->directory, app->icon);
            art = image_resized(r, icon, 90, 90);
        }
        if (art) draw_at(r, art, 50, base + 19);
        else draw_stretch(r, skin(r, MAINUI_ENTRY_SCRIPT(e->type) ? "icon-app-default.png" : "ic-game.png"), 50, base + 19, 90, 90);
        // Under an app, Recent shows Apps (r6t), and under a port Ports (p4).
        // Favorites shows an app's or a port's sublabel, its description, under the name (p7).
        const char *system = !recent ? (MAINUI_ENTRY_SCRIPT(e->type) && e->sublabel[0] ? e->sublabel : NULL)
                           : e->type == MAINUI_ENTRY_APP  ? say(fe, 2, "Apps")
                           : e->type == MAINUI_ENTRY_PORT ? say(fe, 196, "Ports")
                                                          : mainui_frontend_entry_system(fe, e);
        shorten(&r->content, mainui_frontend_entry_label(fe, e), 760, name, sizeof(name));
        if (system && r->content.font && r->small.font) {
            // Two lines keep their bottoms: the name in a rectangle as high as its font that ends
            // 65 pixels below the row's top, the system in one ending at 95 (static:
            // TextMenuDrawer::draw), so a smaller font moves them down by the whole difference.
            int h1 = TTF_FontHeight(r->content.font), h2 = TTF_FontHeight(r->small.font);
            draw_text(r, &r->content, name, 160, base + 65 - h1, h1, white, LEFT);
            draw_text(r, &r->small, system, 160, base + 95 - h2, h2, r->dim, LEFT);
        } else {
            text(r, &r->content, name, 160, base + 36, white, LEFT);
        }
        if (recent && mainui_favorites_find(&fe->favorites, e) >= 0) draw_at(r, skin(r, "icon-star.png"), 934, base + 39);
    }
}

static void netplay_list(struct mainui_renderer *r, struct mainui_frontend *fe, int focus, int first, int bar) {
    if (!fe->host_count) {
        text(r, &r->content, say(fe, 144, "None"), 512, 365, white, CENTER);
        return;
    }
    for (int row = 0; row < MAINUI_TAB_LIST_ROWS && first + row < fe->host_count; ++row) {
        int index = first + row, base = 70 + row * 128, focused = !bar && index == focus;
        const struct mainui_netplay_host *host = &fe->hosts[index];
        struct mainui_render_image *bg = skin(r, focused ? "list-item-2line-long-bg-f.png" : "list-item-2line-long-bg-n.png");
        if (bg) draw_at(r, bg, 20, base + (128 - bg->h) / 2);
        else focus_box(r, 20, base + 2, 984, 124, focused);
        char icon[PATH_BYTES], label[512], sublabel[160];
        snprintf(icon, sizeof(icon), "%s", host->coverpath);
        struct mainui_render_image *art = host->coverpath[0] ? image_resized(r, icon, 90, 90) : NULL;
        if (art) draw_at(r, art, 50, base + 19);
        else draw_stretch(r, skin(r, "ic-game.png"), 50, base + 19, 90, 90);
        shorten(&r->content, host->label, 760, label, sizeof(label));
        snprintf(sublabel, sizeof(sublabel), "%s %s", host->gamelabel, host->hostip);
        if (r->content.font && r->small.font) {
            draw_text(r, &r->content, label, 147, base + 65 - TTF_FontHeight(r->content.font), TTF_FontHeight(r->content.font), white, LEFT);
            draw_text(r, &r->small, sublabel, 147, base + 95 - TTF_FontHeight(r->small.font), TTF_FontHeight(r->small.font), r->dim, LEFT);
        }
    }
}

static void tabs_window(struct mainui_renderer *r, struct mainui_frontend *fe) {
    const struct mainui_model *m = &fe->model;
    const struct mainui_tab_state *t = &m->tabs[m->tab];
    char counter[32] = "";
    int bar = m->bar_focused;
    if (bar) tab_strip(r, fe);
    else header(r, tab_name(fe, m->tab), 0);
    if (!bar) status_bar(r, fe);
    int n = 0;
    switch (m->tab) {
    case MAINUI_TAB_RECENT:
    case MAINUI_TAB_FAVORITES:
        n = m->tab == MAINUI_TAB_RECENT ? fe->recent_count : fe->favorites_count;
        entry_list(r, fe, m->tab == MAINUI_TAB_RECENT, t->focus, t->first, bar);
        if (n) snprintf(counter, sizeof(counter), "%d/%d", t->focus + 1, n);
        break;
    case MAINUI_TAB_NETPLAY:
        n = fe->host_count;
        netplay_list(r, fe, t->focus, t->first, bar);
        if (n) snprintf(counter, sizeof(counter), "%d/%d", t->focus + 1, n);
        break;
    default:
        n = m->tab == MAINUI_TAB_GAMES ? fe->visible_count : m->tab == MAINUI_TAB_APPS ? fe->shown_app_count
          : m->tab == MAINUI_TAB_PORTS ? fe->shown_port_count
          : m->tab == MAINUI_TAB_BESTS ? fe->shown_best_count
                                                                                       : mainui_model_settings_tiles(m);
        if (mainui_model_tab_layout(m, m->tab) == 2) {
            compact_page(r, fe, n, t->focus, bar);
            const int per_page = MAINUI_COMPACT_COLUMNS * MAINUI_COMPACT_ROWS;
            snprintf(counter, sizeof(counter), "%d/%d", t->focus / per_page + 1, n ? (n + per_page - 1) / per_page : 1);
        } else if (mainui_model_tab_layout(m, m->tab) == 0) {
            row_page(r, fe, n, t->focus, bar);
        } else {
            grid_page(r, fe, n, t->focus, bar);
            snprintf(counter, sizeof(counter), "%d/%d", t->focus / 6 + 1, (n + 5) / 6);
        }
        break;
    }
    const struct hint list[3] = {{"A", say(fe, 46, "OK")}, {"MENU", say(fe, 91, "MENU")}, {"SELECT", say(fe, 134, "Search")}};
    hints(r, fe, list, 3, counter[0] ? counter : NULL);
}

// ---- Other windows -------------------------------------------------------------------------

// A one-line list row: the row skin, the 4-digit number, and the name (l2a).
static void numbered_row(struct mainui_renderer *r, int base, int number, const char *label, int focused) {
    char digits[16], name[512];
    row_skin(r, 20, base, focused);
    snprintf(digits, sizeof(digits), "%04d", number);
    text(r, &r->small, digits, 50, base + 36, r->number, LEFT);
    text(r, &r->content, truncate_like_stock(&r->content, label, MAINUI_NUMBERED_TEXT_W, name, sizeof(name)), 115, base + 23, white, LEFT);
}

static void roms_window(struct mainui_renderer *r, struct mainui_frontend *fe, const struct mainui_window *w) {
    const struct mainui_system *s = mainui_frontend_system(fe, w->system);
    // A list rebuilt after a launch is titled TRIMUI until it is opened again (l2b, f4).
    const char *folder = w->rompath[0] ? strrchr(w->rompath, '/') : NULL;
    header(r, w->resumed ? "TRIMUI" : folder ? folder + 1 : s ? s->label : "", 1);
    status_bar(r, fe);
    int n = mainui_frontend_roms(fe, w->system);
    char counter[32] = "", rompath[1024], imgpath[1024];
    if (!n) {
        // An empty system list says None, its text top at 372, 7 pixels above the Netplay tab's (k2i).
        text(r, &r->content, say(fe, 144, "None"), 512, 358, white, CENTER);
    }
    int icon_rows = 0;
    for (int i = 0; i < n; ++i) if (fe->games[i].folder) { icon_rows = 1; break; }
    for (int row = 0; row < MAINUI_ROM_LIST_ROWS && w->first + row < n; ++row) {
        int index = w->first + row;
        int base = 73 + row * 107;
        if (icon_rows) {
            char name[512];
            row_skin(r, 20, base, index == w->focus);
            draw_at(r, skin(r, fe->games[index].folder ? "ic-folder.png" : "ic-game.png"), 50, base + 10);
            text(r, &r->content, truncate_like_stock(&r->content,
                 mainui_catalog_game_label(&fe->games[index]), MAINUI_NUMBERED_TEXT_W,
                 name, sizeof(name)), 137, base + 25, white, LEFT);
        } else {
            numbered_row(r, base, index + 1, mainui_catalog_game_label(&fe->games[index]), index == w->focus);
        }
        // A favorite shows the star, which the picture covers (k2b).
        if (mainui_frontend_is_favorite(fe, MAINUI_LIST_ROMS, w->system, index))
            draw_at(r, skin(r, "icon-star.png"), 934, 73 + row * 107 + 25);
    }
    if (n && s) {
        // The focused game's picture at the Picture Size (150% on the reference device).
        mainui_catalog_game_paths(s, &fe->games[w->focus], rompath, sizeof(rompath), imgpath, sizeof(imgpath));
        struct mainui_render_image *art = image(r, imgpath);
        int size = fe->settings.values[MAINUI_SET_PICTURESIZE];
        if (art) {
            // TextMenuDrawer first CPU-resizes into 420x580, 280x380 or
            // 160x220, then fits that texture with single-precision arithmetic.
            // Keeping both stages matters for patterned art and integer rounding.
            int limit_w = size == 150 ? 420 : size == 100 ? 280 : 160;
            int ih = size == 150 ? 580 : size == 100 ? 380 : 220;
            int iw = art->w;
            if (iw != limit_w || art->h != ih) {
                iw = (int)((int64_t)ih * art->w / art->h);
                if (iw > limit_w) {
                    iw = limit_w;
                    ih = (int)((int64_t)limit_w * art->h / art->w);
                }
            } else ih = art->h;
            if (iw < 1) iw = 1;
            if (ih < 1) ih = 1;
            float bw = size == 150 ? 400.f : size == 100 ? 260.f : 150.f;
            float bh = size == 150 ? 560.f : size == 100 ? 360.f : 210.f;
            float scale = 1.f / fmaxf((float)iw / bw, (float)ih / bh);
            int dw = (int)((float)iw * scale), dh = (int)((float)ih * scale);
            int margin = size == 50 ? 5 : 10, border = margin * 2;
            int x = MAINUI_SCREEN_W - dw - 20 - margin;
            struct mainui_render_image *cover = skin(r, "bg-cover.png");
            if (cover) draw_stretch(r, cover, x - margin, 90, dw + border, dh + border);
            else fill(r, x - margin, 90, dw + border, dh + border, (SDL_Color){255, 255, 255, 179});
            struct mainui_render_image *sized = image_resized(r, imgpath, iw, ih);
            draw_stretch(r, sized ? sized : art, x, 90 + margin, dw, dh);
        }
        snprintf(counter, sizeof(counter), "%d/%d", w->focus + 1, n);
    }
    const struct hint list[2] = {{"A", say(fe, 46, "OK")}, {"MENU", say(fe, 91, "MENU")}};
    hints(r, fe, list, 2, counter[0] ? counter : NULL);
}

static void dim_screen(struct mainui_renderer *r) {
    struct mainui_render_image *mask = skin(r, "float-win-mask.png");
    if (mask) draw_stretch(r, mask, 0, 0, MAINUI_SCREEN_W, MAINUI_SCREEN_H);
    else fill(r, 0, 0, MAINUI_SCREEN_W, MAINUI_SCREEN_H, (SDL_Color){0, 0, 0, 150});
}

static const char *popup_label(struct mainui_frontend *fe, const struct mainui_window *w, int item, char *buffer, size_t size) {
    if (item >= MAINUI_POP_LAUNCHER) {
        int system = w->system, launcher = item - MAINUI_POP_LAUNCHER;
        const struct mainui_system *s = mainui_frontend_system(fe, system);
        if (s && launcher < s->launcher_count) return s->launcher_names[launcher];
        return "";
    }
    switch ((enum mainui_pop)item) {
    case MAINUI_POP_EDIT: return say(fe, 168, "Edit");
    case MAINUI_POP_SEARCH_ALL: return say(fe, 223, "Search All");
    case MAINUI_POP_REFRESH_ROMS: return say(fe, 54, "Refresh Roms");
    case MAINUI_POP_ADD_FAVORITE: return say(fe, 55, "Add Favorite");
    case MAINUI_POP_REMOVE_FAVORITE: return say(fe, 81, "Remove Favorite");
    case MAINUI_POP_CLEAR_LIST: return say(fe, 77, "Clear List");
    case MAINUI_POP_CLEAR_FAVORITE: return say(fe, 79, "Clear Favorite");
    case MAINUI_POP_OPEN: return say(fe, 51, "Open");
    case MAINUI_POP_DELETE: return say(fe, 110, "Delete");
    case MAINUI_POP_SEARCH: return say(fe, 134, "Search");
    case MAINUI_POP_REMOVE_DEVICE: return say(fe, 52, "Remove");
    case MAINUI_POP_CONNECT: return say(fe, 113, "Connect");
    case MAINUI_POP_CHANGE_PASSWORD: return say(fe, 183, "Change Password");
    case MAINUI_POP_FORGET: return say(fe, 184, "Forget");
    case MAINUI_POP_PICTURE_SIZE:
        snprintf(buffer, size, say(fe, 216, "Picture Size %d%%"), fe->settings.values[MAINUI_SET_PICTURESIZE]);
        return buffer;
    default: return "";
    }
}

// A popup menu: the bg-menu-NN panel sized for its rows plus one, 60-pixel rows, and the
// bg-menu-item bar on the focused row (n04).
static void popup_window(struct mainui_renderer *r, struct mainui_frontend *fe, const struct mainui_window *w) {
    char buffer[64], name[32];
    dim_screen(r);
    int h = 60 * (w->item_count + 1) + 4, y = (MAINUI_SCREEN_H - h) / 2;
    snprintf(name, sizeof(name), "bg-menu-%02d.png", w->item_count + 1);
    struct mainui_render_image *panel = w->item_count + 1 <= 9 ? skin(r, name) : NULL;
    if (panel) draw_at(r, panel, (MAINUI_SCREEN_W - panel->w) / 2, y);
    else if ((panel = skin(r, "bg-menu-09.png")) != NULL) draw_stretch(r, panel, (MAINUI_SCREEN_W - panel->w) / 2, y, panel->w, h);
    else {
        fill(r, 212, y + 1, 600, h - 2, (SDL_Color){8, 12, 14, 235});
        outline(r, 212, y + 1, 600, h - 2, 2, (SDL_Color){150, 150, 150, 255});
    }
    for (int i = 0; i < w->item_count; ++i) {
        int row = y + 32 + i * 60;
        if (i == w->focus) {
            struct mainui_render_image *bar = skin(r, "bg-menu-item.png");
            if (bar) draw_at(r, bar, 230, row);
            else { fill(r, 230, row, 560, 60, (SDL_Color){16, 110, 84, 255}); outline(r, 230, row, 560, 60, 2, green); }
        }
        // Rows center their label by the font's height: row + 4 in Default.
        struct mainui_font label = r->menu;
        label.shift = 0;
        text(r, &label, popup_label(fe, w, w->items[i], buffer, sizeof(buffer)), 512, row + (60 - TTF_FontHeight(label.font)) / 2, white,
             CENTER);
    }
}

// The pop-bg panel centers on the screen, and what a window draws on it moves with it:
// Default's 904x544 sits at (60, 112), Syber's 908x548 at (58, 110) (round 7). Returns the offset
// from Default's place.
static struct mainui_render_image *pop_panel(struct mainui_renderer *r, int *dx, int *dy) {
    struct mainui_render_image *panel = skin(r, "pop-bg.png");
    *dx = panel ? (MAINUI_SCREEN_W - panel->w) / 2 - 60 : 0;
    *dy = panel ? (MAINUI_SCREEN_H - panel->h) / 2 - 112 : 0;
    if (panel) draw_at(r, panel, 60 + *dx, 112 + *dy);
    return panel;
}

// A dialog button: the skin's focused or plain button, stretched to width when it is not 300,
// with its label centred on the focused button's height (Default's plain button is 2 pixels
// taller): 506 on Default's 60-pixel button.
static void dialog_button(struct mainui_renderer *r, int x, int y, int width, int focused, const char *label) {
    struct mainui_render_image *button = skin(r, focused ? "btn-bg-f.png" : "btn-bg-n.png"), *shape = skin(r, "btn-bg-f.png");
    int h = shape ? shape->h : 60;
    if (button && width == button->w) {
        draw_at(r, button, x, y);
    } else if (button) {
        draw_stretch(r, button, x, y, width, button->h);
    } else {
        fill(r, x, y, width, 61, focused ? (SDL_Color){16, 90, 70, 255} : (SDL_Color){0, 0, 0, 120});
        outline(r, x, y, width, 61, 2, focused ? green : (SDL_Color){150, 150, 150, 255});
    }
    struct mainui_font font = r->popup_button;
    font.shift = 0;
    text(r, &font, label, x + width / 2, y + (h - TTF_FontHeight(font.font)) / 2, white, CENTER);
}

// A dialog: the pop-bg panel, a green title, the message, and Cancel/OK buttons (n04). The power
// dialog is titled Power, asks whether to power off or reboot, and has three buttons: Power off,
// Reboot and Cancel. Power off and Cancel use the language's stock texts; the rest is English, or
// Portuguese in Portuguese.
static void dialog_window(struct mainui_renderer *r, struct mainui_frontend *fe, const struct mainui_window *w) {
    static const struct { int title, message; const char *title_text, *message_text; } texts[] = {
        [MAINUI_DIALOG_SAVE_EDIT] = {93, 161, "Save", "Do you want to save modification?"},
        [MAINUI_DIALOG_CLEAR_RECENT] = {77, 78, "Clear List", "Clear recent game list,\nTF card content will\nnot be deleted"},
        [MAINUI_DIALOG_REMOVE_FAVORITE] = {81, 82, "Remove Favorite", "Remove game from this\nlist, TF card content will\nnot be deleted"},
        [MAINUI_DIALOG_CLEAR_FAVORITE] = {79, 80, "Clear Favorite", "Clear all game from this\nlist, TF card content will\nnot be deleted"},
        [MAINUI_DIALOG_DELETE_ROM] = {106, 107, "Delete ROM", "Delete ROM from TF Card?"},
        [MAINUI_DIALOG_FACTORY_RESET] = {67, 68, "Resume Default Setting", "Resume all system\nsettings.TF card content\nwill not be deleted"},
        [MAINUI_DIALOG_POWER_OFF] = {86, 86, "Power off", "Power off"},  // static: DialogWindow(86, 86)
        [MAINUI_DIALOG_FLASH_MODE] = {29, 146, "Flash Mode", "Reboot into Flash Mode"},
        [MAINUI_DIALOG_UPGRADE] = {56, 57, "Upgrade", "Update system?"},
        [MAINUI_DIALOG_REMOVE_DEVICE] = {52, 165, "Remove", "Remove device?"},  // d1
        [MAINUI_DIALOG_REFRESH_ROMS] = {54, 69, "Refresh Roms", "refresh roms on TF card"},
    };
    char message[256];
    // The Save dialog shows over black, without the editor under it (e15).
    if (w->dialog == MAINUI_DIALOG_SAVE_EDIT) fill(r, 0, 0, MAINUI_SCREEN_W, MAINUI_SCREEN_H, (SDL_Color){0, 0, 0, 255});
    else dim_screen(r);
    int dx, dy;
    if (!pop_panel(r, &dx, &dy)) {
        fill(r, 60, 113, 903, 542, (SDL_Color){8, 10, 12, 225});
        outline(r, 60, 113, 903, 542, 2, (SDL_Color){150, 150, 150, 255});
    }
    // A settings extension's confirm: prompt has the row's label and the script's line.
    int extension = w->dialog == MAINUI_DIALOG_EXTENSION_CONFIRM, mapping = w->dialog == MAINUI_DIALOG_RESET_MAPPING;
    int power = w->dialog == MAINUI_DIALOG_POWER_OFF, pt = language_is_portuguese(fe);
    char extension_title[256];
    text(r, &r->title, extension ? ext_label(fe, w->index, extension_title, sizeof(extension_title)) :
                       mapping ? mapping_text(fe, "Reset") :
                       power ? (pt ? "Energia" : "Power") :
                                 say(fe, texts[w->dialog].title, texts[w->dialog].title_text), 512, 183 + dy, r->title_color, CENTER);
    if (extension) shorten(&r->popup_text, fe->ext_confirm, 820, message, sizeof(message));
    else if (mapping) snprintf(message, sizeof(message), "%s", mapping_text(fe, "Remove all button mappings?"));
    else if (power) snprintf(message, sizeof(message), "%s", pt ? "Desligar ou reiniciar?" : "Power off or reboot?");
    else snprintf(message, sizeof(message), "%s", say(fe, texts[w->dialog].message, texts[w->dialog].message_text));
    // DrawStringLines over the 100-pixel rectangle at y 298: the lines share its height, each row
    // at least the font's height plus 5, each line centered in its row. That gives the measured one
    // line at 322 and three at 300, 56 apart in Default and 48 in Syber Dark (n04, e15, e17c, f04),
    // and the two-line messages of Chinese, Korean, and Vietnamese (round 8).
    struct mainui_font unshifted = r->popup_text;
    unshifted.shift = 0;
    int lines = 1, height = TTF_FontHeight(unshifted.font);
    for (const char *p = message; *p; ++p) lines += *p == 0x0a;
    int row = 100 / lines;
    if (height + 4 >= row) row = height + 5;
    char *line = message;
    for (int i = 0; i < lines && line; ++i) {
        char *end = strchr(line, 0x0a);
        if (end) *end = 0;
        text(r, &unshifted, line, 512, 298 + dy + i * row + (row - height) / 2, white, CENTER);
        line = end ? end + 1 : NULL;
    }
    if (power) {
        // The stock skin's buttons are 300 pixels wide, for two; three are stretched to 220, at
        // 162, 402 and 642 on the panel.
        static const int order[3] = {MAINUI_DIALOG_OK, MAINUI_DIALOG_REBOOT, MAINUI_DIALOG_CANCEL};
        const char *labels[3] = {pt ? "Desligar" : say(fe, 86, "Power off"), pt ? "Reiniciar" : "Reboot",
                                 pt ? "Cancelar" : say(fe, 45, "Cancel")};
        for (int i = 0; i < 3; ++i) dialog_button(r, 162 + 240 * i + dx, 502 + dy, 220, w->focus == order[i], labels[i]);
        return;
    }
    for (int i = 0; i < 2; ++i)
        dialog_button(r, (i ? 560 : 160) + dx, 502 + dy, 300, w->focus == (i ? MAINUI_DIALOG_OK : MAINUI_DIALOG_CANCEL),
                      i ? say(fe, 46, "OK") : say(fe, 45, "Cancel"));
}

static int language_is_portuguese(const struct mainui_frontend *fe) {
    return strcmp(fe->settings.language, "pt.lang") == 0;
}

// Button Mapping's texts: Portuguese, or English in every other language.
static const char *mapping_text(const struct mainui_frontend *fe, const char *english) {
    static const struct { const char *english, *portuguese; } texts[] = {
        {"Button Mapping", "Mapeamento de botões"}, {"Status", "Estado"}, {"Active", "Ativo"},
        {"Inactive", "Inativo"}, {"Invalid file", "Ficheiro inválido"}, {"Layout", "Esquema"},
        {"Default", "Predefinido"}, {"Disabled", "Desativado"}, {"Custom", "Personalizado"}, {"Reset", "Repor"},
        {"(remapped)", "(remapeada)"}, {"Remove all button mappings?", "Remover todos os mapeamentos de botões?"},
        {MAPPING_SAVE_FAILED_TEXT, "Não foi possível guardar o mapeamento.\nO mapeamento anterior foi mantido."},
    };
    if (!language_is_portuguese(fe)) return english;
    for (size_t i = 0; i < sizeof(texts) / sizeof(texts[0]); ++i)
        if (strcmp(texts[i].english, english) == 0) return texts[i].portuguese;
    return english;
}

// LED Policy's texts: Portuguese, or English in every other language.
static const char *led_text(const struct mainui_frontend *fe, const char *english) {
    static const struct { const char *english, *portuguese; } texts[] = {
        {"Battery level", "Nível da bateria"}, {"CPU speed", "Velocidade do CPU"}, {"Temperature", "Temperatura"},
        {"Charging", "Carregamento"}, {"Custom", "Personalizado"}, {"Effect", "Efeito"}, {"Colour", "Cor"},
        {"Time", "Tempo"}, {"Off", "Desligado"}, {"Linear", "Linear"}, {"Breath", "Respiração"}, {"Sniff", "Farejar"},
        {"Static", "Estático"}, {"Blink 1", "Piscar 1"}, {"Blink 2", "Piscar 2"}, {"Blink 3", "Piscar 3"},
        {"Random", "Aleatória"}, {"Red", "Vermelho"}, {"Green", "Verde"}, {"Blue", "Azul"}, {"Yellow", "Amarelo"},
        {"Purple", "Roxo"}, {"Cyan", "Ciano"}, {"White", "Branco"},
        {"(not running)", "(não está a correr)"}, {"(BrickTools in control)", "(controlado pelo BrickTools)"},
    };
    if (!language_is_portuguese(fe)) return english;
    for (size_t i = 0; i < sizeof(texts) / sizeof(texts[0]); ++i)
        if (strcmp(texts[i].english, english) == 0) return texts[i].portuguese;
    return english;
}

// The values a row shows: LED and Button Mapping rows live in the model, the rest in system.json.
static int row_value(const struct mainui_frontend *fe, enum mainui_setting setting) {
    return mainui_model_mapping_setting(setting) || mainui_model_led_setting(setting) ? fe->model.settings[setting]
                                                                                       : fe->settings.values[setting];
}

// A range's value: Time reads "N s", the others "N/max" (Stock++) or "N / max" (BrickOS).
static const char *range_text(const struct mainui_item *item, int v, const char *separator, char *buffer, size_t size) {
    if (item->setting == MAINUI_SET_LED_TIME) snprintf(buffer, size, "%d s", v);
    else snprintf(buffer, size, "%d%s%d", v, separator, item->max);
    return buffer;
}

static const char *mapping_status_text(const struct mainui_frontend *fe) {
    return mapping_text(fe, fe->mapping_status == MAINUI_MAPPING_ACTIVE ? "Active" :
                            fe->mapping_status == MAINUI_MAPPING_INVALID ? "Invalid file" : "Inactive");
}

// Button Mapping's Layout and key rows are read-only while the profile is invalid.
static int mapping_locked_row(const struct mainui_frontend *fe, const struct mainui_item *item) {
    return item->kind == MAINUI_ITEM_CYCLE && fe->model.mapping_locked && mainui_model_mapping_setting(item->setting);
}

// The value of a Device Info row: the stock rows hold them in order, and rows that settings
// extensions add between them do not count.
static const char *device_info_value(const struct mainui_frontend *fe, const struct mainui_item *items, int index) {
    if (items[index].kind != MAINUI_ITEM_INFO) return NULL;
    int info = 0;
    for (int i = 0; i < index; ++i) info += items[i].kind == MAINUI_ITEM_INFO;
    return info < MAINUI_INFO_COUNT ? fe->device_info[info] : NULL;
}

// The Theme row's value: the name of the catalog theme in use, or none when no theme matches.
static const char *theme_value(const struct mainui_frontend *fe) {
    for (int i = 0; i < fe->theme_count; ++i)
        if (strcmp(fe->themes[i].path, fe->settings.theme) == 0) return fe->themes[i].name;
    return "";
}

static const char *screen_title(const struct mainui_frontend *fe, enum mainui_screen screen) {
    if (screen == MAINUI_SCREEN_FRONTEND) return "Frontend";
    // The stock list behind the Theme row keeps the stock title.
    if (screen == MAINUI_SCREEN_THEME_LIST) return say(fe, 108, "Themes");
    if (screen == MAINUI_SCREEN_BUTTON_MAPPING) return mapping_text(fe, "Button Mapping");
    if (screen == MAINUI_SCREEN_BOOTLOGO)
        return language_is_portuguese(fe) ? "Logótipo de arranque" : "Bootlogo";
    if (screen == MAINUI_SCREEN_SPLASHSCREEN)
        return language_is_portuguese(fe) ? "Ecrã de arranque" : "Splashscreen";
    if (screen == MAINUI_SCREEN_LOADING_SCREEN)
        return language_is_portuguese(fe) ? "Ecrã de carregamento" : "Loading Screen";
    if (screen == MAINUI_SCREEN_BRICKOS_STARTUP)
        return language_is_portuguese(fe) ? "Destino inicial" : "Default destination";
    if (screen == MAINUI_SCREEN_APPEARANCE) return mainui_appearance_text("Appearance", language_is_portuguese(fe));
    if (screen == MAINUI_SCREEN_APPEARANCE_FONT) return mainui_appearance_text("Font", language_is_portuguese(fe));
    static const struct { int index; const char *text; } titles[MAINUI_SCREEN_COUNT] = {
        {142, "Display"}, {24, "Volume"}, {111, "Wi-Fi"}, {114, "Hotspot"}, {117, "Bluetooth"}, {108, "Themes"},
        {22, "Keymap"}, {211, "Homepage"}, {143, "System"}, {218, "Tab Visibility"}, {23, "Language"},
        {171, "Date And Time"}, {176, "Time Zone"}, {221, "Developer"}, {30, "Device Info"}, {197, "USB Mode"},
        {147, "IO Testing"}, {174, "Set Date"},
    };
    return say(fe, titles[screen].index, titles[screen].text);
}

// A settings row's label in the loaded language: the model names items in English, and the
// language file holds each one's text (English "Language" reads "Language (Language)", e13a).
static const char *item_label(const struct mainui_frontend *fe, const char *english) {
    if (strcmp(english, "Button Mapping") == 0 || strcmp(english, "Status") == 0 || strcmp(english, "Layout") == 0 ||
        strcmp(english, "Reset") == 0)
        return mapping_text(fe, english);
    if (strcmp(english, "Effect") == 0 || strcmp(english, "Colour") == 0 || strcmp(english, "Time") == 0)
        return led_text(fe, english);
    // BrickOS Appearance's rows.
    if (strcmp(english, "Appearance") == 0 || strcmp(english, "Font") == 0 || strcmp(english, "Reset appearance") == 0)
        return mainui_appearance_text(english, language_is_portuguese(fe));
    for (int setting = MAINUI_SET_APPEAR_ACCENT; setting <= MAINUI_SET_APPEAR_LAST; ++setting)
        if (strcmp(english, mainui_appearance_label(setting, 0)) == 0)
            return mainui_appearance_label(setting, language_is_portuguese(fe));
    if (language_is_portuguese(fe)) {
        if (strcmp(english, "Theme") == 0) return "Tema";
        if (strcmp(english, "Bootlogo") == 0) return "Logótipo de arranque";
        if (strcmp(english, "Splashscreen") == 0) return "Ecrã de arranque";
        if (strcmp(english, "Loading Screen") == 0) return "Ecrã de carregamento";
        if (strcmp(english, "No valid images installed") == 0) return "Sem imagens válidas instaladas";
        if (strcmp(english, "Default destination") == 0) return "Destino inicial";
        if (strcmp(english, "Desktop") == 0) return "Secretária";
        if (strcmp(english, "Games") == 0) return "Jogos";
        if (strcmp(english, "Favorites") == 0) return "Favoritos";
        if (strcmp(english, "Recent") == 0) return "Recentes";
        if (strcmp(english, "Last used") == 0) return "Último ecrã usado";
    }
    static const struct { const char *english; int index; } labels[] = {
        {"24-hour clock", 172}, {"Allow focus to the tab bar", 213}, {"Apps", 2}, {"BGM Volume", 25},
        {"Bests", 140}, {"Bluetooth", 117}, {"Brightness", 26}, {"Color Temperature", 192},
        {"Confirmation Button", 214}, {"Contrast", 193}, {"Date And Time", 171}, {"Developer", 221},
        {"Device Info", 30}, {"Dpad to Analog Key(hold)", 209}, {"Enable Keytone", 220}, {"Enable SSH", 222},
        {"Exposure", 195}, {"F1/F2 LED Scale", 206}, {"Factory Reset", 28}, {"Favorites", 1}, {"Flash Mode", 29},
        {"Games", 0}, {"Hardware Test", 31}, {"Hotspot", 114}, {"Keymap", 22}, {"LED", 139}, {"LED Policy", 207},
        {"Language", 23}, {"Linux Version", 160}, {"Lock Screen Time", 187}, {"Model Name", 59},
        {"Model Number", 149}, {"My Devices", 129}, {"Navigation page", 212}, {"Netplay", 118},
        {"Network Synchronization Time", 173}, {"OSD Shortcut", 217}, {"Other Devices", 130}, {"Password", 116}, {"Ports", 196}, {"Power off", 86}, {"Recent", 18},
        {"Refresh Roms", 27}, {"Rumble", 186}, {"SSID", 115}, {"Saturation", 194},
        {"Set Date", 174}, {"Tab Visibility", 218}, {"Taillight LED Scale", 205}, {"Time Zone", 176},
        {"Top LED Scale", 145}, {"UI Version", 169}, {"USB Bottom", 201}, {"USB Mode", 197}, {"Version", 61},
        {"Volume", 24}, {"Wi-Fi", 111}, {"Year", 177}, {"Month", 178}, {"Day", 179},
        {"Hour", 180}, {"Minute", 181}, {"Second", 182},
    };
    for (size_t i = 0; i < sizeof(labels) / sizeof(labels[0]); ++i)
        if (strcmp(labels[i].english, english) == 0) return say(fe, labels[i].index, english);
    return english;
}

// The text a cycling setting shows for its value (e05, e11); a value outside the measured ones
// shows its number.
static const char *cycle_text(const struct mainui_frontend *fe, enum mainui_setting setting, int v, char *buffer, size_t size) {
    static const char *const dpad[7] = {NULL, "F1", "F2", "R2", "L2", "SELECT", "START"};
    if (mainui_model_appearance_setting(setting)) {
        const char *value = mainui_appearance_value_label(setting, v, language_is_portuguese(fe));
        if (value) return value;
    }
    switch (setting) {
    case MAINUI_SET_LEDPOLICY:
        // The other policy reads "Default" (led1).
        return v == 1 ? say(fe, 208, "Follow Theme") : say(fe, 109, "Default");
    case MAINUI_SET_LED_OPTION: {
        static const char *const modes[MAINUI_LED_OPTIONS] = {
            NULL, NULL, "Battery level", "CPU speed", "Temperature", "Charging", "Custom",
        };
        if (v == MAINUI_LED_DEFAULT) return say(fe, 109, "Default");
        if (v == MAINUI_LED_FOLLOW_THEME) return say(fe, 208, "Follow Theme");
        if (v < 0 || v >= MAINUI_LED_OPTIONS) break;
        // An LED mode says when the LED daemon cannot show it.
        if (fe->led_status == MAINUI_LED_RUNNING) return led_text(fe, modes[v]);
        snprintf(buffer, size, "%s %s", led_text(fe, modes[v]),
                 led_text(fe, fe->led_status == MAINUI_LED_BRICKTOOLS ? "(BrickTools in control)" : "(not running)"));
        return buffer;
    }
    case MAINUI_SET_LED_EFFECT: {
        static const char *const effects[MAINUI_LED_EFFECTS] = {
            "Off", "Linear", "Breath", "Sniff", "Static", "Blink 1", "Blink 2", "Blink 3",
        };
        if (v >= 0 && v < MAINUI_LED_EFFECTS) return led_text(fe, effects[v]);
        break;
    }
    case MAINUI_SET_LED_COLOUR: {
        static const char *const colours[MAINUI_LED_COLOURS] = {
            "Random", "Red", "Green", "Blue", "Yellow", "Purple", "Cyan", "White",
        };
        if (v >= 0 && v < MAINUI_LED_COLOURS) return led_text(fe, colours[v]);
        break;
    }
    case MAINUI_SET_DIMTIME:
        if (v < 0) return say(fe, 189, "Never");
        snprintf(buffer, size, say(fe, 188, "%d Minutes"), v / 60);
        return buffer;
    case MAINUI_SET_KEYMAP:
        // default and rotate 270 (g04); "map3" is not offered.
        if (v == 0) return say(fe, 71, "default");
        if (v == 1) return say(fe, 72, "rotate 270");
        break;
    case MAINUI_SET_DPAD2AXIS:
        if (v == 0) return say(fe, 105, "Off");
        // A key Button Mapping binds loses its hold role.
        if (v > 0 && v < 7 && mainui_model_hold_remapped(&fe->model)) {
            snprintf(buffer, size, "%s %s", dpad[v], mapping_text(fe, "(remapped)"));
            return buffer;
        }
        if (v > 0 && v < 7) return dpad[v];
        break;
    case MAINUI_SET_MAP_LAYOUT:
        if (v == MAINUI_MAP_NINTENDO) return "Nintendo";
        if (v == MAINUI_MAP_XBOX) return "Xbox";
        return mapping_text(fe, "Custom");
    case MAINUI_SET_MAP_SELECT: case MAINUI_SET_MAP_START: case MAINUI_SET_MAP_F1: case MAINUI_SET_MAP_F2:
    case MAINUI_SET_MAP_VOLUP: case MAINUI_SET_MAP_VOLDOWN:
        if (v < 0 || v >= MAINUI_MAP_DESTS) return mapping_text(fe, "Custom");
        if (v == MAINUI_MAP_DEFAULT || v == MAINUI_MAP_DISABLED) return mapping_text(fe, mainui_map_dest_names[v]);
        return mainui_map_dest_names[v];
    case MAINUI_SET_BCONFIRM:
        return v ? "B" : "A";
    case MAINUI_SET_OSDSHORTCUT:
        if (v >= 0 && v < MAINUI_OSD_SHORTCUTS) return mainui_osd_shortcut_names[v];
        break;
    case MAINUI_SET_USBMODE:
        if (v == 0) return say(fe, 198, "Host Mode");
        if (v == 1) return say(fe, 199, "Dock Mode");
        if (v == 2) return say(fe, 200, "Data Mode");
        break;
    default:
        break;
    }
    snprintf(buffer, size, "%d", v);
    return buffer;
}

// A settings row: the label at the left, then a value between arrows (in a dark field while
// focused) or a switch (e05).
// An extension row in the stock list: a toggle's switch, a select's or range's arrows and value,
// an info row's value at the right, and a submenu's arrow. A disabled row is grey.
static void row_skin(struct mainui_renderer *r, int x, int base, int focused);
static void extension_row(struct mainui_renderer *r, struct mainui_frontend *fe, int node, int base, int dx, int focused) {
    const struct mainui_ext_node *n = mainui_ext_node(&fe->ext, node);
    if (!n) return;
    char label[256], value[256], visible[256];
    SDL_Color ink = n->disabled ? (SDL_Color){128, 128, 128, 255} : white;
    row_skin(r, 20 + dx, base, focused);
    ext_label(fe, node, label, sizeof(label));
    ext_value(fe, node, 0, value, sizeof(value));
    int label_width = 860;
    if (n->kind == MAINUI_EXT_SELECT || n->kind == MAINUI_EXT_RANGE) label_width = 430;
    else if (n->kind == MAINUI_EXT_INFO || n->kind == MAINUI_EXT_TOGGLE || n->kind == MAINUI_EXT_MENU) label_width = 480;
    text(r, &r->content, shorten(&r->content, label, label_width, visible, sizeof(visible)), 60 + dx, base + 25, ink, LEFT);
    if (n->busy) {
        text(r, &r->content, value, 964 + dx, base + 25, ink, RIGHT);
        return;
    }
    switch (n->kind) {
    case MAINUI_EXT_TOGGLE: {
        struct mainui_render_image *sw = skin(r, n->value ? "sw-on.png" : "sw-off.png");
        if (sw) draw_at(r, sw, 884 + dx, base + 30);
        else text(r, &r->content, value, 928 + dx, base + 25, ink, CENTER);
        break;
    }
    case MAINUI_EXT_SELECT:
    case MAINUI_EXT_RANGE:
        if (focused) draw_at(r, skin(r, "bg-setting-textfield.png"), 512, base + 20);
        draw_at(r, skin(r, focused ? "ic-left-arrow-a.png" : "ic-left-arrow-n.png"), 512, base + 20);
        draw_at(r, skin(r, focused ? "ic-right-arrow-a.png" : "ic-right-arrow-n.png"), 914, base + 20);
        text_box(r, &r->content, shorten(&r->content, value, 380, visible, sizeof(visible)), 512, 462, base + 25, ink);
        break;
    case MAINUI_EXT_INFO:
        text(r, &r->content, shorten(&r->content, value, 420, visible, sizeof(visible)), 979 + dx, base + 25, ink, RIGHT);
        break;
    case MAINUI_EXT_MENU:
        draw_at(r, skin(r, focused ? "ic-right-arrow-a.png" : "ic-right-arrow-n.png"), 914 + dx, base + 20);
        break;
    case MAINUI_EXT_CMD:
        break;
    }
}

static void settings_row(struct mainui_renderer *r, struct mainui_frontend *fe, const struct mainui_item *item, const char *label,
                         const char *picture, const char *info, int base, int network, int dx, int focused, int date_value) {
    char value[64], buffer[64];
    // Button Mapping's rows are read-only while the profile is invalid: grey, without arrows.
    int locked = mapping_locked_row(fe, item);
    SDL_Color ink = locked ? (SDL_Color){128, 128, 128, 255} : white;
    row_skin(r, 20 + dx, base, focused);
    struct mainui_render_image *flag = picture && picture[0] ? skin(r, picture) : NULL;
    if (flag) draw_at(r, flag, 30, base + 5);
    else text(r, &r->content, label, (network ? 50 : 60) + dx, base + 25, ink, LEFT);
    // Device Info and hotspot values sit at the right end (e13e, e05).
    if (info) text(r, &r->content, info, (network ? 973 : 979) + dx, base + 25, white, RIGHT);
    int v = row_value(fe, item->setting);
    value[0] = 0;
    if (locked) {
        text(r, &r->content, cycle_text(fe, item->setting, v, buffer, sizeof(buffer)), 979 + dx, base + 25, ink, RIGHT);
        return;
    }
    if (item->kind == MAINUI_ITEM_RANGE) range_text(item, v, "/", value, sizeof(value));
    else if (item->kind == MAINUI_ITEM_DATE) snprintf(value, sizeof(value), "%d", date_value);
    else if (item->kind == MAINUI_ITEM_CYCLE) snprintf(value, sizeof(value), "%s", cycle_text(fe, item->setting, v, buffer, sizeof(buffer)));
    if (item->kind == MAINUI_ITEM_SWITCH) {
        struct mainui_render_image *sw = skin(r, v ? "sw-on.png" : "sw-off.png");
        if (sw) draw_at(r, sw, 884 + dx, base + (network ? 32 : 30));
        else text(r, &r->content, v ? say(fe, 104, "On") : say(fe, 105, "Off"), 928 + dx, base + 25, white, CENTER);
    } else if (value[0]) {
        if (focused) draw_at(r, skin(r, "bg-setting-textfield.png"), 512, base + 20);
        draw_at(r, skin(r, focused ? "ic-left-arrow-a.png" : "ic-left-arrow-n.png"), 512, base + 20);
        draw_at(r, skin(r, focused ? "ic-right-arrow-a.png" : "ic-right-arrow-n.png"), 914, base + 20);
        text_box(r, &r->content, value, 512, 462, base + 25, white);
    }
}

static void boot_visual_row(struct mainui_renderer *r, const char *label, int base, int focused, int ticked) {
    char visible_label[MAINUI_BOOT_VISUAL_NAME + 8];
    struct mainui_render_image *background = skin_resized(r, focused ? "list-item-1line-sort-bg-f.png" :
                                                           "list-item-1line-sort-bg-n.png", 470, 96);
    if (background) draw_at(r, background, 20, base + 2);
    else focus_box(r, 20, base + 2, 470, 96, focused);
    text(r, &r->content, shorten(&r->content, label, 360, visible_label, sizeof(visible_label)),
         54, base + 35, white, LEFT);
    if (ticked) draw_at(r, skin(r, "tick-focused.png"), 438, base + 32);
}

static void boot_visual_preview(struct mainui_renderer *r, struct mainui_frontend *fe,
                                enum mainui_screen screen, int focus, int brickos);
static void loading_screen_preview(struct mainui_renderer *r, struct mainui_frontend *fe, int focus, int brickos);

// The installed loading screen: a preset's name, TRIMUI, Custom, or Unavailable when the skin
// folder cannot be read.
static const char *loading_screen_value(const struct mainui_frontend *fe) {
    int active = fe->loading_screen_active;
    if (active == MAINUI_LOADING_SCREEN_TRIMUI) return "TRIMUI";
    for (int i = 0; active >= 0 && i < fe->loading_screen_count; ++i)
        if (fe->loading_screens[i].index == active) return fe->loading_screens[i].name;
    if (active == -2) return language_is_portuguese(fe) ? "Indisponível" : "Unavailable";
    return language_is_portuguese(fe) ? "Personalizado" : "Custom";
}

// Selector row 0 is TRIMUI and row n preset n - 1.
static const char *loading_screen_label(const struct mainui_frontend *fe, int entry) {
    return entry > 0 && entry <= fe->loading_screen_count ? fe->loading_screens[entry - 1].name : "TRIMUI";
}

static int loading_screen_installed(const struct mainui_frontend *fe, int entry) {
    if (entry == 0) return fe->loading_screen_active == MAINUI_LOADING_SCREEN_TRIMUI;
    return entry <= fe->loading_screen_count && fe->loading_screen_active >= 0 &&
           fe->loading_screens[entry - 1].index == fe->loading_screen_active;
}

// The Bootlogo and Splashscreen lists (bootlogo 1 or 0): entry 0 is Random and entry n the
// catalog's image n - 1.
static const char *boot_visual_random_label(const struct mainui_frontend *fe) {
    return language_is_portuguese(fe) ? "Aleatório" : "Random";
}

static const struct mainui_boot_visual *boot_visual_entry(const struct mainui_frontend *fe, int bootlogo, int entry) {
    int count = bootlogo ? fe->bootlogo_count : fe->splashscreen_count;
    return entry > 0 && entry <= count ? &(bootlogo ? fe->bootlogos : fe->splashscreens)[entry - 1] : NULL;
}

// The ticked entry: Random in random mode, else the applied image.
static int boot_visual_entry_ticked(const struct mainui_frontend *fe, int bootlogo, int entry) {
    if (fe->boot_visuals.random[bootlogo ? MAINUI_BOOTLOGO : MAINUI_SPLASHSCREEN]) return entry == 0;
    const struct mainui_boot_visual *visual = boot_visual_entry(fe, bootlogo, entry);
    return visual && visual->index == (bootlogo ? fe->bootlogo_active : fe->splashscreen_active);
}

// The applied image, found by content, or NULL when it is not one of the list.
static const struct mainui_boot_visual *boot_visual_applied(const struct mainui_frontend *fe, int bootlogo) {
    int active = bootlogo ? fe->bootlogo_active : fe->splashscreen_active;
    int count = bootlogo ? fe->bootlogo_count : fe->splashscreen_count;
    const struct mainui_boot_visual *visuals = bootlogo ? fe->bootlogos : fe->splashscreens;
    for (int i = 0; active >= 0 && i < count; ++i)
        if (visuals[i].index == active) return &visuals[i];
    return NULL;
}

static const char *boot_visual_applied_name(const struct mainui_frontend *fe, int bootlogo) {
    const struct mainui_boot_visual *applied = boot_visual_applied(fe, bootlogo);
    if (applied) return applied->name;
    int active = bootlogo ? fe->bootlogo_active : fe->splashscreen_active;
    return active == -2 ? (language_is_portuguese(fe) ? "Indisponível" : "Unavailable") :
                          (language_is_portuguese(fe) ? "Personalizado" : "Custom");
}

// A selector's row in Settings: Random in random mode, else the applied image.
static const char *boot_visual_value(const struct mainui_frontend *fe, int bootlogo) {
    return fe->boot_visuals.random[bootlogo ? MAINUI_BOOTLOGO : MAINUI_SPLASHSCREEN] ? boot_visual_random_label(fe)
                                                                                    : boot_visual_applied_name(fe, bootlogo);
}

// A network row: the name, then the tick of the network in use or the lock of a locked one, and
// its signal (w1, c1).
static void wifi_row(struct mainui_renderer *r, const struct mainui_wifi_network *net, int base, int focused) {
    char signal[32], name[MAINUI_SSID_MAX + 8];
    row_skin(r, 20, base, focused);
    text(r, &r->content, shorten(&r->content, net->ssid, 780, name, sizeof(name)), 50, base + 25, white, LEFT);
    if (net->connected) draw_at(r, skin(r, "wifi-connected.png"), 899, base + 30);
    else if (net->locked) draw_at(r, skin(r, "wifi-locked.png"), 899, base + 30);
    snprintf(signal, sizeof(signal), "ic-setting-wifi-0%d.png", mainui_wifi_level(net->rssi, net->connected));
    draw_at(r, skin(r, signal), 954, base + 30);
}

// A Bluetooth device row: its kind's icon and its name, on the wider Bluetooth rows (w1); a
// paired device also shows Connected or Disconnected at the right (static: BTDevice::draw).
static void device_row(struct mainui_renderer *r, struct mainui_frontend *fe, const struct mainui_bt_device *device, int paired,
                       int base, int focused) {
    char name[MAINUI_BT_TEXT + 8];
    row_skin(r, 10, base, focused);
    draw_at(r, skin(r, mainui_bt_icon(device)), 40, base + 34);
    text(r, &r->content, shorten(&r->content, device->name[0] ? device->name : device->addr, 640, name, sizeof(name)), 87,
         base + 25, white, LEFT);
    if (paired)
        text(r, &r->content, device->connected ? say(fe, 128, "Connected") : say(fe, 42, "Disconnected"), 984, base + 25, white,
             RIGHT);
}

// A Time Zone entry: the folder or file icon and the name (w1).
static void zone_row(struct mainui_renderer *r, const struct mainui_zone *zone, int base, int focused) {
    row_skin(r, 20, base, focused);
    draw_at(r, skin(r, zone->folder ? "ic-folder.png" : "ic-file.png"), 50, base + 10);
    text(r, &r->content, zone->name, 135, base + 25, white, LEFT);
}

static void files_window(struct mainui_renderer *r, struct mainui_frontend *fe, const struct mainui_window *w) {
    header(r, say(fe, w->index, "TRIMUI"), 1);
    status_bar(r, fe);
    const struct mainui_file_folder *folder = mainui_frontend_file_folder(fe, w->system);
    if (folder) {
        int zone = strstr(folder->path, "/usr/share/zoneinfo") != NULL;
        for (int row = 0; row < MAINUI_ROM_LIST_ROWS && w->first + row < folder->count; ++row) {
            const struct mainui_file *file = &folder->files[w->first + row];
            int base = 73 + row * 107;
            row_skin(r, 20, base, w->first + row == w->focus);
            struct mainui_render_image *icon = skin(r, file->folder ? "ic-folder.png" : zone ? "ic-file.png" : "ic-game.png");
            if (icon) draw_at(r, icon, 50, base - 3 + (107 - icon->h) / 2);
            text(r, &r->content, file->name, 135, base + 25, white, LEFT);
        }
    }
    const struct hint list[] = {{"A", say(fe, 46, "OK")}};
    hints(r, fe, list, 1, NULL);
}

static void settings_rows(struct mainui_renderer *r, struct mainui_frontend *fe, const struct mainui_window *w);

static void settings_window(struct mainui_renderer *r, struct mainui_frontend *fe, const struct mainui_window *w) {
    // Under "Turning on/off bluetooth" the screen stays as it was before the switch (w1: the
    // switch off and no device list), so it is drawn with the previous value.
    int level = (int)(w - fe->model.stack);
    const struct mainui_window *above = level + 1 < fe->model.depth ? &fe->model.stack[level + 1] : NULL;
    int switching = above && above->kind == MAINUI_WIN_PROGRESS &&
                    (above->progress == MAINUI_PROGRESS_BLUETOOTH_ON || above->progress == MAINUI_PROGRESS_BLUETOOTH_OFF);
    if (switching) {
        fe->settings.values[MAINUI_SET_BLUETOOTH] ^= 1;
        fe->model.settings[MAINUI_SET_BLUETOOTH] ^= 1;
    }
    settings_rows(r, fe, w);
    if (switching) {
        fe->settings.values[MAINUI_SET_BLUETOOTH] ^= 1;
        fe->model.settings[MAINUI_SET_BLUETOOTH] ^= 1;
    }
}

static void settings_rows(struct mainui_renderer *r, struct mainui_frontend *fe, const struct mainui_window *w) {
    if (w->screen == MAINUI_SCREEN_IO_TEST) {
        // Actual KeyTestWindow2::getRectByCode/getMaskByCode and draw: ordered,
        // unscaled masks over the Brick picture. Both Fn codes use fn2 in stock.
        static const struct { const char *mask; int x, y; } controls[MAINUI_TEST_CONTROLS] = {
            {"A",608,495}, {"B",577,527}, {"X",577,464}, {"Y",545,495},
            {"up",415,475}, {"down",415,521}, {"left",389,501}, {"right",435,501},
            {"start",540,622}, {"select",498,622}, {"menu",456,622},
            {"L",391,110}, {"R",576,110}, {"L2",455,110}, {"R2",540,110},
            {"power",740,359}, {"add",258,235}, {"minus",258,282},
            {"fn2",469,468}, {"fn2",514,468},
        };
        draw_at(r, skin(r, "bg-iotesting.png"), 0, 0);
        header(r, screen_title(fe, w->screen), 1);
        status_bar(r, fe);
        for (int i = 0; i < MAINUI_TEST_CONTROLS; ++i) if (fe->io_held & (UINT32_C(1) << i)) {
            char mask[64]; snprintf(mask, sizeof(mask), "iotesting-key-%s.png", controls[i].mask);
            draw_at(r, skin(r, mask), controls[i].x, controls[i].y);
        }
        if (fe->io_motor) draw_at(r, skin(r, "ic-vibration.png"), 714, 109);
        if (fe->io_fn1) draw_at(r, skin(r, "iotesting-key-fn1.png"), 743, 286);
        draw_down(r, skin(r, "tips-MENU.png"), 24, 726, 32, 32);
        text(r, &r->stat, "+", 66, 725, white, LEFT);
        draw_down(r, skin(r, "tips-START.png"), 89, 726, 32, 32);
        text(r, &r->stat, say(fe, 148, "Exit"), 131, 725, white, LEFT);
        return;
    }
    struct mainui_item fixed_items[128];
    int boot_visual_selector = w->screen == MAINUI_SCREEN_BOOTLOGO || w->screen == MAINUI_SCREEN_SPLASHSCREEN ||
                               w->screen == MAINUI_SCREEN_LOADING_SCREEN;
    // Rows beyond 128 (long image lists, extension screens and sections) get their own array.
    int capacity = mainui_model_screen_item_capacity(&fe->model, w->screen);
    int allocated = capacity > 128;
    if (!allocated) capacity = 128;
    struct mainui_item *items = allocated ? calloc((size_t)capacity, sizeof(*items)) : fixed_items;
    if (!items) return;
    header(r, settings_title(fe, w), 1);
    status_bar(r, fe);
    if (w->screen == MAINUI_SCREEN_USB_MODE) {
        // The picture of the chosen USB role under the list (e13f).
        static const char *const pictures[3] = {"usb-host-mode.png", "usb-dock-mode.png", "usb-data-mode.png"};
        int mode = fe->settings.values[MAINUI_SET_USBMODE];
        if (mode >= 0 && mode < 3) draw_at(r, skin(r, pictures[mode]), 0, 0);
    }
    int n = mainui_model_screen_items(&fe->model, w->screen, items, capacity);
    // A list that shrank (a device moved to My Devices) can leave the focus past the end or on a
    // section title; the row the next key starts from is the one shown in focus, as the model
    // takes the row above.
    int focus = w->focus < n ? w->focus : n - 1;
    while (focus > 0 && items[focus].kind == MAINUI_ITEM_HEADER) --focus;
    int first = w->first;
    if (first > n - 6) first = n > 6 ? n - 6 : 0;
    if (first < 0 || focus < first) first = focus > 0 ? focus : 0;
    if (focus >= first + 6) first = focus - 5;
    // The network screens start their list 10 pixels lower, and Bluetooth 10 pixels further
    // left (e05, e08).
    int network = w->screen == MAINUI_SCREEN_WIFI || w->screen == MAINUI_SCREEN_HOTSPOT || w->screen == MAINUI_SCREEN_BLUETOOTH;
    int top = network ? 83 : 73, dx = w->screen == MAINUI_SCREEN_BLUETOOTH ? -10 : 0;
    for (int row = 0; row < 6 && first + row < n; ++row) {
        int index = first + row, base = top + row * 107, focused = index == focus;
        const struct mainui_item *item = &items[index];
        const char *label = item_label(fe, item->label), *picture = NULL, *info = NULL;
        int ticked = 0;
        if (item->kind == MAINUI_ITEM_HEADER) {
            // A section title: green text without a row (w1).
            text(r, &r->content, label, 50 + dx, base + 25, r->highlight, LEFT);
            continue;
        }
        if (item->kind == MAINUI_ITEM_EXTENSION) {
            extension_row(r, fe, item->action, base, dx, focused);
            continue;
        }
        if (item->kind == MAINUI_ITEM_LIST) {
            int first_list = index;
            while (first_list > 0 && items[first_list - 1].kind == MAINUI_ITEM_LIST && items[first_list - 1].action == item->action)
                --first_list;
            int entry = index - first_list;
            if (item->action == MAINUI_LIST_WIFI && entry < fe->wifi_count) {
                wifi_row(r, &fe->wifi[entry], base, focused);
                continue;
            }
            if (item->action == MAINUI_LIST_PAIRED && entry < fe->paired_count) {
                device_row(r, fe, &fe->paired[entry], 1, base, focused);
                continue;
            }
            if (item->action == MAINUI_LIST_BLUETOOTH && entry < fe->found_count) {
                device_row(r, fe, &fe->found[entry], 0, base, focused);
                continue;
            }
            if (item->action == MAINUI_LIST_TIMEZONES) {
                const struct mainui_zone_folder *folder = &fe->zones[w->system >= 0 && w->system < MAINUI_ZONE_LEVELS ? w->system : 0];
                if (entry < folder->count) zone_row(r, &folder->zones[entry], base, focused);
                continue;
            }
            if (item->action == MAINUI_LIST_THEMES && entry < fe->theme_count) {
                label = fe->themes[entry].name;
                ticked = strcmp(fe->themes[entry].path, fe->settings.theme) == 0;
            } else if (item->action == MAINUI_LIST_LANGUAGES && entry < fe->language_count) {
                label = fe->languages[entry];
                picture = fe->language_icons[entry];
            } else if (item->action == MAINUI_LIST_BOOTLOGOS || item->action == MAINUI_LIST_SPLASHSCREENS) {
                int bootlogo = item->action == MAINUI_LIST_BOOTLOGOS;
                const struct mainui_boot_visual *visual = boot_visual_entry(fe, bootlogo, entry);
                if (entry == 0) label = boot_visual_random_label(fe);
                else if (visual) label = visual->name;
                ticked = boot_visual_entry_ticked(fe, bootlogo, entry);
            } else if (item->action == MAINUI_LIST_LOADING_SCREENS) {
                label = loading_screen_label(fe, entry);
                ticked = loading_screen_installed(fe, entry);
            }
        }
        if (w->screen == MAINUI_SCREEN_DEVICE_INFO && device_info_value(fe, items, index)) info = device_info_value(fe, items, index);
        if (w->screen == MAINUI_SCREEN_BUTTON_MAPPING && item->kind == MAINUI_ITEM_INFO) info = mapping_status_text(fe);
        if (item->kind == MAINUI_ITEM_ACTION && item->action == MAINUI_ACTION_SSID) info = fe->hotspot_ssid;
        if (item->kind == MAINUI_ITEM_ACTION && item->action == MAINUI_ACTION_PASSWORD) info = fe->hotspot_password;
        if (item->kind == MAINUI_ITEM_PRESENTATION_CHOICE && item->action == (int)fe->model.presentation)
            info = language_is_portuguese(fe) ? "Ativo" : "Active";
        if (item->kind == MAINUI_ITEM_SUBMENU && item->screen == MAINUI_SCREEN_FRONTEND)
            info = fe->model.presentation == MAINUI_PRESENTATION_STOCK ? "Stock++" : "BrickOS";
        if (boot_visual_selector) {
            boot_visual_row(r, label, base, focused, ticked);
            continue;
        }
        settings_row(r, fe, item, label, picture, info, base, network, dx, focused,
                     item->kind == MAINUI_ITEM_DATE ? w->date[item->action] : 0);
        // The theme in use has the tick at the right (t1; static: ThemeMenu loads tick-focused.png).
        if (ticked) draw_at(r, skin(r, "tick-focused.png"), 939, base + 30);
        // Date And Time shows the stored zone, or Default (w1), and the Set Date row the local date
        // and time (g07c: 09/26/2026 15:48:39).
        if (item->kind == MAINUI_ITEM_SUBMENU && item->screen == MAINUI_SCREEN_TIMEZONE)
            text(r, &r->content, fe->settings.timezone[0] ? fe->settings.timezone : say(fe, 109, "Default"), 964, base + 25, white,
                 RIGHT);
        if (item->kind == MAINUI_ITEM_SUBMENU &&
            (item->screen == MAINUI_SCREEN_BOOTLOGO || item->screen == MAINUI_SCREEN_SPLASHSCREEN)) {
            char visible_value[MAINUI_BOOT_VISUAL_NAME + 8];
            text(r, &r->content, shorten(&r->content, boot_visual_value(fe, item->screen == MAINUI_SCREEN_BOOTLOGO), 360,
                                         visible_value, sizeof(visible_value)),
                 964, base + 25, white, RIGHT);
        }
        if (item->kind == MAINUI_ITEM_SUBMENU && item->screen == MAINUI_SCREEN_LOADING_SCREEN) {
            char visible_value[MAINUI_LOADING_SCREEN_NAME + 8];
            text(r, &r->content, shorten(&r->content, loading_screen_value(fe), 360, visible_value, sizeof(visible_value)),
                 964, base + 25, white, RIGHT);
        }
        if (item->kind == MAINUI_ITEM_SUBMENU && item->screen == MAINUI_SCREEN_THEME_LIST) {
            char visible_value[sizeof(fe->themes[0].name) + 8];
            text(r, &r->content, shorten(&r->content, theme_value(fe), 360, visible_value, sizeof(visible_value)),
                 964, base + 25, white, RIGHT);
        }
        if (item->kind == MAINUI_ITEM_ACTION && item->action == MAINUI_ACTION_SET_DATE) {
            char now_text[80];
            time_t now = time(NULL);
            struct tm local;
            localtime_r(&now, &local);
            snprintf(now_text, sizeof(now_text), "%02d/%02d/%04d %02d:%02d:%02d", local.tm_mon + 1, local.tm_mday, local.tm_year + 1900,
                     local.tm_hour, local.tm_min, local.tm_sec);
            text(r, &r->content, now_text, 964, base + 25, white, RIGHT);
        }
    }
    if (w->screen == MAINUI_SCREEN_LOADING_SCREEN) loading_screen_preview(r, fe, focus, 0);
    else if (boot_visual_selector) boot_visual_preview(r, fe, w->screen, focus, 0);
    // The focused extension row's description, on the hint line's right.
    char description[512], visible_description[512];
    if (ext_description(fe, items, n, focus, description, sizeof(description)))
        text(r, &r->stat, shorten(&r->stat, description, 560, visible_description, sizeof(visible_description)),
             1000, 725, white, RIGHT);
    if (allocated) free(items);
    // Until its first scan is read, the Wi-Fi list says Scanning (r3: text top at 377).
    if (w->screen == MAINUI_SCREEN_WIFI && fe->wifi_first && fe->settings.values[MAINUI_SET_WIFI])
        text(r, &r->content, say(fe, 120, "Scanning"), 512, 365, white, CENTER);
    if (w->screen == MAINUI_SCREEN_WIFI || w->screen == MAINUI_SCREEN_BLUETOOTH) {
        const struct hint list[1] = {{"A", say(fe, 46, "OK")}};
        hints(r, fe, list, 1, NULL);
        return;
    }
    if (w->screen == MAINUI_SCREEN_USB_MODE) {
        const struct hint list[1] = {{"B", say(fe, 89, "BACK")}};
        hints(r, fe, list, 1, NULL);
        return;
    }
    const struct hint list[2] = {{"A", say(fe, 46, "OK")}, {"MENU", say(fe, 91, "MENU")}};
    hints(r, fe, list, 2, NULL);
}

// The search keyboard: the panel, the entry line, 90x80 keys on a 94x84 pitch, and the bottom
// row of five 184-pixel keys with the buttons that work them (e20).
static void keyboard_window(struct mainui_renderer *r, struct mainui_frontend *fe, const struct mainui_window *w) {
    static const char *const bottom_icons[5][2] = {
        {"ic-shift.png", "button-tips-L.png"}, {"ic-abc.png", "button-tips-R.png"}, {"ic-space.png", "button-tips-X.png"},
        {"ic-delete.png", "button-tips-Y.png"}, {"ic-OK.png", "button-tips-START.png"},
    };
    static const char *const bottom_names[5][2] = {{"shift", "L"}, {"abc", "R"}, {"space", "X"}, {"del", "Y"}, {"OK", "START"}};
    // The keyboard keeps the title of what it types for (w1: Wi-Fi). Open: the hotspot's.
    if (w->purpose == MAINUI_TYPE_WIFI_PASSWORD) header(r, say(fe, 111, "Wi-Fi"), 1);
    else if (w->purpose == MAINUI_TYPE_SEARCH) header(r, say(fe, 134, "Search"), 1);
    else header(r, say(fe, 114, "Hotspot"), 1);
    status_bar(r, fe);
    struct mainui_render_image *panel = skin(r, "bg-wifi-keyboard.png");
    if (panel) draw_at(r, panel, 20, 74);
    // The keyboard's entry field (static: ImeWindow loads input-wifi-assword.png, with the stock
    // spelling; f04: nearly clear in Syber Dark, whose bg-search-input.png is light).
    struct mainui_render_image *field = skin(r, "input-wifi-assword.png");
    if (field) draw_at(r, field, 44, 104);
    else fill(r, 44, 104, 936, 100, (SDL_Color){255, 255, 255, 40});
    text(r, &r->content, w->text, 75, 126, white, LEFT);
    for (int key = 0; key < MAINUI_KEYBOARD_KEYS; ++key) {
        int focused = key == w->focus;
        if (key < 40) {
            int x = 44 + (key % 10) * 94, y = 234 + (key / 10) * 84;
            char label[8];
            skin_box(r, "bg-wifi-button-01-selected.png", "bg-wifi-button-01-unselect.png", x, y, 90, 80, focused);
            snprintf(label, sizeof(label), "%c", mainui_keyboard_char(key, w->shift, w->symbols));
            text(r, &r->content, label, x + 45, y + 14, white, CENTER);
        } else {
            int b = key - 40, x = 44 + b * 188, y = 570;
            skin_box(r, "bg-wifi-button-02-selected.png", "bg-wifi-button-02-unselect.png", x, y, 184, 80, focused);
            const char *icon_name = b == 0 && w->shift ? "ic-shift-active.png" : b == 1 && w->symbols ? "ic-symbol.png" : bottom_icons[b][0];
            struct mainui_render_image *icon = skin(r, icon_name), *button = skin(r, bottom_icons[b][1]);
            if (icon) draw_down(r, icon, x + 10, y + 16, 72, 48);
            else text(r, &r->small, bottom_names[b][0], x + 46, y + 25, white, CENTER);
            if (button) draw_down(r, button, x + 102, y + 16, 72, 48);
            else text(r, &r->small, bottom_names[b][1], x + 138, y + 25, white, CENTER);
        }
    }
    const struct hint list[1] = {{"A", say(fe, 46, "OK")}};
    hints(r, fe, list, 1, NULL);
}

// Draws a name with the part that matched the search in green (e20).
static void matched_text(struct mainui_renderer *r, const char *name, const char *typed, int x, int bottom) {
    if (!r->content.font) return;
    int height = TTF_FontHeight(r->content.font), y = bottom - height;
    size_t n = strlen(name), t = strlen(typed), at = n;
    for (size_t i = 0; t && i + t <= n && at == n; ++i) {
        size_t k = 0;
        while (k < t && tolower((unsigned char)name[i + k]) == tolower((unsigned char)typed[k])) ++k;
        if (k == t) at = i;
    }
    if (at == n) { draw_text(r, &r->content, name, x, y, height, white, LEFT); return; }
    char part[512];
    snprintf(part, sizeof(part), "%.*s", (int)at, name);
    x += draw_text(r, &r->content, part, x, y, height, white, LEFT);
    snprintf(part, sizeof(part), "%.*s", (int)t, name + at);
    x += draw_text(r, &r->content, part, x, y, height, r->highlight, LEFT);
    draw_text(r, &r->content, name + at + t, x, y, height, white, LEFT);
}

// Search results: two-line rows with the number, the name, and the system (e20).
static void search_window(struct mainui_renderer *r, struct mainui_frontend *fe, const struct mainui_window *w) {
    char title[MAINUI_TEXT_MAX + 128], counter[32] = "", number[16], name[512];
    snprintf(title, sizeof(title), "%s:%s", say(fe, 134, "Search"), w->text);
    header(r, title, 1);
    status_bar(r, fe);
    int scoped = w->system >= 0;
    if (!fe->hit_count) text(r, &r->content, say(fe, 144, "None"), 512, 365, white, CENTER);
    for (int row = 0; row < mainui_search_rows(w->system) && w->first + row < fe->hit_count; ++row) {
        int index = w->first + row, base = scoped ? 73 + row * 107 : 70 + row * 128, focused = index == w->focus;
        const struct mainui_search_hit *hit = &fe->hits[index];
        struct mainui_render_image *bg = skin(r, focused ? "list-item-2line-long-bg-f.png" : "list-item-2line-long-bg-n.png");
        if (scoped) row_skin(r, 20, base, focused);
        else if (bg) draw_at(r, bg, 20, base + (128 - bg->h) / 2);  // Default: 128 focused, 120 plain
        else focus_box(r, 20, base + 2, 984, 124, focused);
        snprintf(number, sizeof(number), "%04d", index + 1);
        text(r, &r->small, number, 50, base + (scoped ? 36 : 49), r->number, LEFT);
        const struct mainui_game *game = mainui_frontend_search_game(fe, index);
        if (game) {
            // Names end in "..." as in the numbered lists (k2g: "(Track 0..." at the 42nd code point).
            matched_text(r, truncate_like_stock(&r->content, mainui_catalog_game_label(game), MAINUI_NUMBERED_TEXT_W, name, sizeof(name)), w->text, 115, base + (scoped ? 62 : 65));
        }
        const struct mainui_system *s = mainui_frontend_system(fe, hit->system);
        if (r->small.font) {
            int height = TTF_FontHeight(r->small.font);
            draw_text(r, &r->small, s ? s->label : "", 115, base + (scoped ? 92 : 95) - height, height, r->dim, LEFT);
        }
    }
    if (fe->hit_count) snprintf(counter, sizeof(counter), "%d/%d", w->focus + 1, fe->hit_count);
    const struct hint list[2] = {{"A", say(fe, 46, "OK")}, {scoped ? "MENU" : "B", scoped ? say(fe, 91, "MENU") : say(fe, 89, "BACK")}};
    hints(r, fe, list, 2, counter[0] ? counter : NULL);
}

// Edit: the grid of every system or app with a checkbox for its visibility (e15).
static void edit_window(struct mainui_renderer *r, struct mainui_frontend *fe, const struct mainui_window *w) {
    header(r, tab_name(fe, (enum mainui_tab)w->system), 0);
    status_bar(r, fe);
    int apps = w->system == MAINUI_TAB_APPS;
    int n = apps ? fe->app_count : fe->system_count, page = w->focus / 6;
    char counter[32], path[PATH_BYTES];
    for (int slot = 0; slot < 6; ++slot) {
        int index = page * 6 + slot;
        if (index >= n) break;
        int x = 42 + (slot % 3) * 320, y = 84 + (slot / 3) * 320;
        const char *label = apps ? fe->apps[index].label : fe->systems[index].label;
        int shown = apps ? fe->apps[index].show : fe->systems[index].show;
        const char *directory = apps ? fe->apps[index].directory : fe->systems[index].directory;
        const char *icon = apps ? fe->apps[index].icon : fe->systems[index].icontop;
        struct mainui_render_image *picture = NULL;
        if (icon[0]) {
            snprintf(path, sizeof(path), "%s/%s", directory, icon);
            picture = apps ? image(r, path) : system_icon(r, path);
        }
        if (picture && apps) SDL_SetTextureScaleMode(picture->texture, SDL_ScaleModeNearest);
        int plain = apps ? fe->apps[index].icon_only : fe->systems[index].icon_only;
        struct mainui_render_image *icon_layer = apps ? app_icon_layer(r, &fe->apps[index]) : NULL;
        grid_tile(r, x, y, index == w->focus, !apps || (!picture && !icon_layer), plain, picture,
                  icon_layer, label, NULL);
        struct mainui_render_image *box = skin(r, shown ? "checkbox-checked.png" : "checkbox-uncheck.png");
        if (box) draw_at(r, box, x + 17, y + 17);
        else outline(r, x + 17, y + 17, 60, 60, 2, white);
    }
    snprintf(counter, sizeof(counter), "%d/%d", page + 1, (n + 5) / 6);
    const struct hint list[3] = {{"A", say(fe, 46, "OK")}, {"MENU", say(fe, 91, "MENU")}, {"SELECT", say(fe, 134, "Search")}};
    hints(r, fe, list, 3, counter);
}

// The home page: the TRIMUI logo, the search box, and four 220-pixel shortcuts (n08).
static void home_window(struct mainui_renderer *r, struct mainui_frontend *fe) {
    static const char *const icons[MAINUI_TAB_COUNT] = {
        "favorites", "recent", "best", "games", "ports", "app", "netplay", "settings",
    };
    struct mainui_render_image *logo = skin(r, "nav-logo.png");
    if (logo) draw_at(r, logo, 20, 15);
    else text(r, &r->title, "TRIMUI", 20, 12, green, LEFT);
    status_bar(r, fe);
    // The search box skin carries its "Trimui Search" text and magnifier.
    skin_box(r, "nav-search-f.png", "nav-search-n.png", 30, 125, 964, 160, fe->model.home_search);
    enum mainui_tab tabs[MAINUI_TAB_COUNT];
    int n = mainui_model_visible_tabs(&fe->model, tabs), focus = 0;
    for (int i = 0; i < n; ++i)
        if ((int)tabs[i] == fe->model.home_focus) focus = i;
    int start = fe->model.home_first < n - 4 ? fe->model.home_first : n - 4;
    if (start < 0) start = 0;
    for (int slot = 0; slot < 4 && start + slot < n; ++slot) {
        int i = start + slot, x = 30 + slot * 248;
        int focused = i == focus && !fe->model.home_search;
        char name[64];
        skin_box(r, "nav-item-bg-f.png", "nav-item-bg-n.png", x, 393, 220, 220, focused);
        snprintf(name, sizeof(name), "nav-icon-%s-%s.png", icons[tabs[i]], focused ? "f" : "n");
        draw_at(r, skin(r, name), x, 393);
        text(r, &r->content, tab_name(fe, tabs[i]), x + 110, 623, focused ? white : r->home_dim, CENTER);
    }
    const struct hint list[1] = {{"A", say(fe, 46, "OK")}};
    hints(r, fe, list, 1, NULL);
}

static void progress_window(struct mainui_renderer *r, struct mainui_frontend *fe, const struct mainui_window *w) {
    // Bluetooth and Wi-Fi (the same ProgressDialog): the popup panel over the screen, the dots,
    // and the message (w1). The dots run through five 100 ms frames: none, then dot 1 to dot 4
    // (static: ProgressDialog::draw, frame % 5 with SDL_Delay(100)).
    int dx, dy;
    if (!pop_panel(r, &dx, &dy)) fill(r, 60, 112, 904, 544, (SDL_Color){0, 0, 0, 230});
    char dot[32];
    unsigned frame = (unsigned)(SDL_GetTicks() / 100 % 5);
    snprintf(dot, sizeof(dot), "progress-dot%u.png", frame);
    if (frame) draw_at(r, skin(r, dot), 481 + dx, 379 + dy);
    if (w->progress == MAINUI_PROGRESS_EXTENSION) {
        // A settings extension's command: its label, its newest line, and B to cancel.
        char label[256], visible[256];
        text(r, &r->content, shorten(&r->content, ext_label(fe, w->index, label, sizeof(label)), 820, visible, sizeof(visible)),
             512, 300 + dy, white, CENTER);
        text(r, &r->small, shorten(&r->small, fe->ext_line, 820, visible, sizeof(visible)), 512, 450 + dy, white, CENTER);
        const struct hint list[1] = {{"B", language_is_portuguese(fe) ? "Cancelar" : "Cancel"}};
        hints(r, fe, list, 1, NULL);
        return;
    }
    if (w->progress == MAINUI_PROGRESS_REBOOT) {
        // The stock firmware has no reboot progress: English, or Portuguese in Portuguese.
        text(r, &r->content, language_is_portuguese(fe) ? "A reiniciar..." : "Rebooting...", 512, 390 + dy, white, CENTER);
        return;
    }
    static const struct { int index; const char *text; } messages[] = {
        [MAINUI_PROGRESS_REFRESH] = {53, "Refresh"},
        [MAINUI_PROGRESS_BLUETOOTH_ON] = {191, "Turning on bluetooth"},
        [MAINUI_PROGRESS_BLUETOOTH_OFF] = {190, "Turning off bluetooth"},
        [MAINUI_PROGRESS_WIFI] = {111, "WiFi"},
        [MAINUI_PROGRESS_WIFI_CONNECT] = {113, "Connect"},
        [MAINUI_PROGRESS_WIFI_FORGET] = {184, "Forget"},
        [MAINUI_PROGRESS_POWER_OFF] = {86, "Power off"},
        [MAINUI_PROGRESS_FLASH_MODE] = {29, "Flash Mode"},
    };
    text(r, &r->content, say(fe, messages[w->progress].index, messages[w->progress].text), 512, 390 + dy, white, CENTER);
}

// A notice: a message with OK over the screen (d1: "Device Removed" for about 5 s; w5: the Wi-Fi
// failure). The official one was not captured, so its look follows the static layout below.
static void notice_window(struct mainui_renderer *r, struct mainui_frontend *fe, const struct mainui_window *w) {
    static const struct { int index; const char *text; } messages[] = {
        [MAINUI_NOTICE_DEVICE_REMOVED] = {164, "Device Removed"},
        [MAINUI_NOTICE_CONNECTION_FAILED] = {163, "Connection Failed"},
        [MAINUI_NOTICE_WIFI_FAILED] = {135, "Fail to connect\nCheck your password\nOr reboot router"},
        [MAINUI_NOTICE_UPGRADE_SPACE] = {58, "SD space low\n%dM left"},
        [MAINUI_NOTICE_BOOT_VISUAL_APPLIED] = {0, "Bitmap saved.\nReboot to apply."},
        [MAINUI_NOTICE_BOOT_VISUAL_FAILED] = {0, "Could not write bitmap.\nCurrent image kept."},
        [MAINUI_NOTICE_PREFERENCE_SAVE_FAILED] = {0, "Could not save preference.\nPrevious destination kept."},
        [MAINUI_NOTICE_LOADING_SCREEN_APPLIED] = {0, "Loading screen saved.\nIt shows the next time a game starts."},
        [MAINUI_NOTICE_LOADING_SCREEN_FAILED] = {0, "Could not write loading screen.\nPrevious images kept."},
        [MAINUI_NOTICE_MAPPING_SAVE_FAILED] = {0, MAPPING_SAVE_FAILED_TEXT},
        [MAINUI_NOTICE_APPEARANCE_SAVE_FAILED] = {0, "Appearance not saved"},
    };
    // Static (NotifyWindow::draw): over the dimmed screen, the popup panel, the message in the
    // dialog's font and rectangle but 120 pixels high, and one OK button in focus, centered 90
    // pixels above the panel's bottom. Not captured on the device.
    dim_screen(r);
    int dx, dy;
    if (!pop_panel(r, &dx, &dy)) fill(r, 60, 112, 904, 544, (SDL_Color){0, 0, 0, 230});
    // DrawStringLines: the rectangle's height is shared by the lines, each row at least the
    // font's height plus 5, and each line centered in its row. For the dialog's 100 pixels this
    // gives the measured one line at 322 and three at 300.
    char message[256], *line = message;
    const char *notice_text;
    char extension_line[256];
    if (w->notice == MAINUI_NOTICE_EXTENSION_LINE)
        notice_text = shorten(&r->popup_text, fe->ext_notice, 820, extension_line, sizeof(extension_line));
    else if (w->notice == MAINUI_NOTICE_BOOT_VISUAL_APPLIED)
        notice_text = language_is_portuguese(fe) ? "Imagem guardada.\nReinicie para aplicar." : messages[w->notice].text;
    else if (w->notice == MAINUI_NOTICE_BOOT_VISUAL_FAILED)
        notice_text = language_is_portuguese(fe) ? "Não foi possível gravar.\nA imagem atual foi mantida." : messages[w->notice].text;
    else if (w->notice == MAINUI_NOTICE_PREFERENCE_SAVE_FAILED)
        notice_text = language_is_portuguese(fe) ? "Não foi possível guardar.\nO destino anterior foi mantido." : messages[w->notice].text;
    else if (w->notice == MAINUI_NOTICE_LOADING_SCREEN_APPLIED)
        notice_text = language_is_portuguese(fe) ? "Ecrã de carregamento guardado.\nAparece no próximo jogo iniciado." :
                      messages[w->notice].text;
    else if (w->notice == MAINUI_NOTICE_LOADING_SCREEN_FAILED)
        notice_text = language_is_portuguese(fe) ? "Não foi possível gravar.\nAs imagens anteriores foram mantidas." :
                      messages[w->notice].text;
    else if (w->notice == MAINUI_NOTICE_MAPPING_SAVE_FAILED)
        notice_text = mapping_text(fe, messages[w->notice].text);
    else if (w->notice == MAINUI_NOTICE_APPEARANCE_SAVE_FAILED)
        notice_text = mainui_appearance_text(messages[w->notice].text, language_is_portuguese(fe));
    else
        notice_text = say(fe, messages[w->notice].index, messages[w->notice].text);
    snprintf(message, sizeof(message), "%s", notice_text);
    if (w->notice == MAINUI_NOTICE_UPGRADE_SPACE) {
        // Installed stock translations supply the integer placeholder in string 58.
        snprintf(message, sizeof(message), say(fe, 58, "SD space low\n%dM left"), w->index);
    }
    struct mainui_font font = r->popup_text;
    font.shift = 0;
    int lines = 1, height = TTF_FontHeight(font.font);
    for (const char *p = message; *p; ++p) lines += *p == '\n';
    int row = 120 / lines;
    if (height + 4 >= row) row = height + 5;
    for (int i = 0; i < lines && line; ++i) {
        char *end = strchr(line, '\n');
        if (end) *end = 0;
        text(r, &font, line, 512, 298 + dy + i * row + (row - height) / 2, white, CENTER);
        line = end ? end + 1 : NULL;
    }
    struct mainui_render_image *button = skin(r, "btn-bg-f.png");
    int bw = button ? button->w : 300, bh = button ? button->h : 60, bx = (MAINUI_SCREEN_W - bw) / 2, by = 656 + dy - bh - 90;
    if (button) draw_at(r, button, bx, by);
    else fill(r, bx, by, bw, bh, (SDL_Color){16, 90, 70, 255});
    struct mainui_font label = r->popup_button;
    label.shift = 0;
    text(r, &label, say(fe, 46, "OK"), bx + bw / 2, by + (bh - TTF_FontHeight(label.font)) / 2, white, CENTER);
}

static void brickos_bevel(struct mainui_renderer *r, int x, int y, int w, int h, int raised);

// A settings extension's output: its lines from the top line the view scrolled to, and its
// state. Stock draws it as a settings screen, BrickOS as a panel under its app bar.
static void extension_text_window(struct mainui_renderer *r, struct mainui_frontend *fe, const struct mainui_window *w,
                                  int brickos) {
    char state[64], line[512], visible[512];
    const char *text_lines = w->index == fe->ext_view_node && fe->ext_view ? fe->ext_view : "";
    int x = brickos ? 59 : 40, y = brickos ? 147 : 90, pitch = brickos ? 42 : 50, width = brickos ? 900 : 944;
    if (brickos) {
        fill(r, 35, 99, 954, 575, r->material.surface_container);
        brickos_bevel(r, 35, 99, 954, 575, 0);
        text(r, &r->small, ext_view_state(fe, state, sizeof(state)), 965, 111, fe->ext_view_status > 0 ?
             (SDL_Color){242, 128, 128, 255} : r->material.on_surface_variant, RIGHT);
    } else {
        char label[256];
        header(r, ext_label(fe, w->index, label, sizeof(label)), 1);
        status_bar(r, fe);
    }
    const char *p = text_lines;
    for (int skipped = 0; *p && skipped < w->first; ++skipped) {
        const char *end = strchr(p, '\n');
        p = end ? end + 1 : p + strlen(p);
    }
    for (int row = 0; *p && row < MAINUI_TEXT_ROWS; ++row) {
        const char *end = strchr(p, '\n');
        size_t n = end ? (size_t)(end - p) : strlen(p);
        if (n >= sizeof(line)) n = sizeof(line) - 1;
        memcpy(line, p, n);
        line[n] = 0;
        text(r, &r->small, shorten(&r->small, line, width, visible, sizeof(visible)), x, y + row * pitch,
             brickos ? r->material.on_surface : white, LEFT);
        p = end ? end + 1 : p + strlen(p);
    }
    if (!brickos) {
        const struct hint list[1] = {{"B", say(fe, 89, "BACK")}};
        hints(r, fe, list, 1, ext_view_state(fe, state, sizeof(state)));
    }
}

// ---- Frame ---------------------------------------------------------------------------------

// Draws a window and the windows under it that stay visible (a popup or dialog shows over the
// screen it opened from).
static void draw_window(struct mainui_renderer *r, struct mainui_frontend *fe, int level) {
    const struct mainui_window *w = &fe->model.stack[level];
    if ((w->kind == MAINUI_WIN_POPUP || w->kind == MAINUI_WIN_DIALOG || w->kind == MAINUI_WIN_PROGRESS || w->kind == MAINUI_WIN_NOTICE) &&
        level > 0)
        draw_window(r, fe, level - 1);
    switch (w->kind) {
    case MAINUI_WIN_HOME: home_window(r, fe); break;
    case MAINUI_WIN_TABS: tabs_window(r, fe); break;
    case MAINUI_WIN_ROMS: roms_window(r, fe, w); break;
    case MAINUI_WIN_FILES: files_window(r, fe, w); break;
    case MAINUI_WIN_POPUP: popup_window(r, fe, w); break;
    case MAINUI_WIN_DIALOG: dialog_window(r, fe, w); break;
    case MAINUI_WIN_SETTINGS: settings_window(r, fe, w); break;
    case MAINUI_WIN_KEYBOARD: keyboard_window(r, fe, w); break;
    case MAINUI_WIN_SEARCH: search_window(r, fe, w); break;
    case MAINUI_WIN_EDIT: edit_window(r, fe, w); break;
    case MAINUI_WIN_PROGRESS: progress_window(r, fe, w); break;
    case MAINUI_WIN_NOTICE: notice_window(r, fe, w); break;
    case MAINUI_WIN_TEXT: extension_text_window(r, fe, w, 0); break;
    }
}


static void brickos_bevel(struct mainui_renderer *r, int x, int y, int w, int h, int raised) {
    (void)raised;
    outline(r, x, y, w, h, 1, r->material.outline);
}

// Eight small texture slices preserve the corner shape at every widget size.
// Only the exterior glows: the focus border and opaque content remain steady.
enum { FOCUS_AURA_MARGIN = 24 };

static void material_focus_aura(struct mainui_renderer *r, int x, int y, int w, int h,
                                SDL_Color color, float emphasis) {
    enum mainui_halo halo = mainui_appearance_halo(look(r));
    if (!r->animations_enabled || r->focus_pulse_suppressed || emphasis <= 0 || halo == MAINUI_HALO_OUTLINE ||
        r->presentation != MAINUI_PRESENTATION_MATERIAL_YOU) return;
    // The glow follows the Card shape; the textures are kept by corner radius.
    int radius = mainui_appearance_corner(look(r), w, h);
    if (radius < 2 && mainui_appearance_get(look(r), MAINUI_SET_APPEAR_SHAPE) != MAINUI_SHAPE_SQUARE) radius = 2;
    const int margin = FOCUS_AURA_MARGIN, corner = radius + margin, size = 2 * corner + 1;
    if (!r->focus_aura[radius]) {
        SDL_Surface *surface = SDL_CreateRGBSurfaceWithFormat(0, size, size, 32, SDL_PIXELFORMAT_ARGB8888);
        if (!surface) return;
        for (int row = 0; row < size; ++row) {
            Uint32 *pixels = (Uint32 *)((Uint8 *)surface->pixels + row * surface->pitch);
            for (int col = 0; col < size; ++col) {
                float qx = (float)abs(col - corner) - 0.5f, qy = (float)abs(row - corner) - 0.5f;
                float distance = hypotf(fmaxf(qx, 0), fmaxf(qy, 0)) + fminf(fmaxf(qx, qy), 0) - radius;
                Uint8 alpha = distance > 0 ? (Uint8)lroundf(220 * expf(-distance * distance / 128)) : 0;
                pixels[col] = SDL_MapRGBA(surface->format, 255, 255, 255, alpha);
            }
        }
        r->focus_aura[radius] = SDL_CreateTextureFromSurface(r->sdl, surface);
        SDL_FreeSurface(surface);
        if (!r->focus_aura[radius]) return;
        SDL_SetTextureBlendMode(r->focus_aura[radius], SDL_BLENDMODE_BLEND);
    }
    // Pulse breathes every 1800 ms (900 ms when Fast) and asks for redraws; Steady, and Pulse
    // with the animations Off, hold its mean.
    float pulse = 0.5f;
    double motion = mainui_appearance_motion(look(r));
    if (halo == MAINUI_HALO_PULSE && motion > 0) {
        int period = (int)lround(1800 * motion);
        r->focus_pulse_visible = 1;
        pulse = 0.5f - 0.5f * cosf((float)(r->frame_ms % period) * (6.28318530718f / period));
    }
    SDL_Texture *texture = r->focus_aura[radius];
    SDL_SetTextureColorMod(texture, color.r, color.g, color.b);
    SDL_SetTextureAlphaMod(texture, (Uint8)lroundf(255 * emphasis * (0.35f + 0.65f * pulse)));
    const int source_pos[3] = {0, corner, corner + 1}, source_size[3] = {corner, 1, corner};
    const int dest_x[3] = {x - margin, x + radius, x + w - radius};
    const int dest_y[3] = {y - margin, y + radius, y + h - radius};
    const int dest_w[3] = {corner, w - 2 * radius, corner};
    const int dest_h[3] = {corner, h - 2 * radius, corner};
    for (int row = 0; row < 3; ++row) for (int col = 0; col < 3; ++col) {
        if (row == 1 && col == 1) continue;
        SDL_Rect source = {source_pos[col], source_pos[row], source_size[col], source_size[row]};
        SDL_Rect dest = {dest_x[col], dest_y[row], dest_w[col], dest_h[row]};
        SDL_RenderCopy(r->sdl, texture, &source, &dest);
    }
}

// The focus halo's Outline mode draws the outline alone, without the aura or the glow rings.
static int focus_glows(const struct mainui_renderer *r) {
    return mainui_appearance_halo(look(r)) != MAINUI_HALO_OUTLINE;
}

static void brickos_focus(struct mainui_renderer *r, int x, int y, int w, int h) {
    material_focus_aura(r, x, y, w, h, r->material.primary, 1);
    SDL_Color glow = r->material.primary;
    if (focus_glows(r)) {
        glow.a = 34;
        outline(r, x - 5, y - 5, w + 10, h + 10, 5, glow);
        glow.a = 72;
        outline(r, x - 2, y - 2, w + 4, h + 4, 4, glow);
    }
    glow.a = 255;
    outline(r, x, y, w, h, 3, glow);
}

static void material_icon(struct mainui_renderer *r, enum mainui_tab tab, int x, int y);
static void material_icon_color(struct mainui_renderer *r, SDL_Color color);
static void material_icon_line(struct mainui_renderer *r, int x1, int y1, int x2, int y2,
                               SDL_Color color, int thickness);
static void material_icon_circle(struct mainui_renderer *r, int cx, int cy, int radius,
                                 SDL_Color color, int filled);

static void brickos_button(struct mainui_renderer *r, int x, int y, int w, int h,
                          const char *label, int focused, int enabled) {
    SDL_Color background = focused ? r->material.primary_container : r->material.surface_container;
    SDL_Color foreground = !enabled ? r->material.disabled :
                           focused ? r->material.on_primary_container : r->material.on_surface;
    fill(r, x, y, w, h, background);
    if (focused) brickos_focus(r, x + 2, y + 2, w - 4, h - 4);
    char visible[256];
    shorten(&r->small, label, w - 8, visible, sizeof(visible));
    text_box(r, &r->small, visible, x + 4, w - 8, y + (h - r->small.size) / 2, foreground);
}

static void brickos_field(struct mainui_renderer *r, int x, int y, int w, int h,
                         const char *label, const char *value) {
    fill(r, x, y, w, h, r->material.surface_container);
    brickos_bevel(r, x, y, w, h, 0);
    text(r, &r->small, label, x + 8, y + 2, r->material.on_surface, LEFT);
    text(r, &r->small, value, x + 8, y + h - r->small.size - 2, r->material.on_surface, LEFT);
}

enum { MATERIAL_LIST_GAP = 12, MATERIAL_LIST_BOTTOM = 674 };

static int material_line_height(const struct mainui_font *font, const char *value) {
    if (mixed_text(font, value)) return mixed_height(font);
    TTF_Font *selected = font_for_text(font, value, NULL);
    return selected ? TTF_FontHeight(selected) : font->size > 0 ? font->size : 36;
}

static int material_max_line_height(const struct mainui_font *font) {
    int height = material_line_height(font, "");
    if (font->fallback && TTF_FontHeight(font->fallback) > height) height = TTF_FontHeight(font->fallback);
    if (font->mixed && mixed_height(font) > height) height = mixed_height(font);
    return height;
}

static int material_list_row_height(struct mainui_renderer *r) {
    int title_height = material_max_line_height(&r->content);
    int focused_height = material_max_line_height(&r->focus_label);
    if (focused_height > title_height) title_height = focused_height;
    int height = title_height + material_max_line_height(&r->small) + 38;
    return height > 82 ? height : 82;
}

// Pixel-based scrolling supplements the shared model's stock row-count policy.
static int material_visible_first(int focus, int first, int count, int row_height, int available) {
    focus = focus < 0 ? 0 : focus >= count ? count - 1 : focus;
    if (first < 0) first = 0;
    if (first > focus) first = focus;
    int used = 0;
    for (int i = first; i <= focus; ++i) used += row_height + MATERIAL_LIST_GAP;
    while (first < focus && used - MATERIAL_LIST_GAP > available) {
        used -= row_height + MATERIAL_LIST_GAP;
        ++first;
    }
    return first;
}

static int material_list_first(struct mainui_frontend *fe, const struct mainui_window *w,
                               int count, int row_height, int available) {
    int first = material_visible_first(w->focus, w->first, count, row_height, available);
    for (int i = 0; i < fe->model.depth; ++i)
        if (&fe->model.stack[i] == w) fe->model.stack[i].first = first;
    return first;
}

static void brickos_list_row_font(struct mainui_renderer *r, const struct mainui_font *label_font, int x, int y, int w, int h,
                                  const char *label, const char *detail, int focused, int enabled, int leading_icon);

static void brickos_list_row(struct mainui_renderer *r, int x, int y, int w, int h,
                            const char *label, const char *detail, int focused, int enabled, int leading_icon) {
    brickos_list_row_font(r, &r->content, x, y, w, h, label, detail, focused, enabled, leading_icon);
}

// A list row whose label is drawn in label_font (the font list's samples).
static void brickos_list_row_font(struct mainui_renderer *r, const struct mainui_font *label_font, int x, int y, int w, int h,
                                  const char *label, const char *detail, int focused, int enabled, int leading_icon) {
    SDL_Color background = focused ? r->material.primary_container : r->material.surface;
    SDL_Color foreground = !enabled ? r->material.disabled :
                           focused ? r->material.on_primary_container : r->material.on_surface;
    fill(r, x, y, w, h, background);
    brickos_bevel(r, x, y, w, h, !focused);
    if (focused) brickos_focus(r, x + 2, y + 2, w - 4, h - 4);
    int title_height = material_line_height(label_font, label);
    int detail_height = detail && detail[0] ? material_line_height(&r->small, detail) : 0;
    int text_y = y + (h - title_height - (detail_height ? 6 + detail_height : 0)) / 2;
    int text_x = x + (leading_icon ? 70 : 24);
    // Center the actual rendered font box instead of applying stock baseline shifts.
    draw_text(r, label_font, label, text_x, text_y, title_height, foreground, LEFT);
    if (detail_height)
        draw_text(r, &r->small, detail, text_x, text_y + title_height + 6, detail_height,
                  r->material.on_surface_variant, LEFT);
}

// The caption under a BrickOS preview (Loading Screen, Bootlogo and Splashscreen): the label in
// the small font at left, then the value 8 px after the label's width, shortened with "..." to the
// rest of width, so neither draws over the other in any language, text size or font.
static void brickos_preview_caption(struct mainui_renderer *r, const char *label, const char *value, int left, int y,
                                    int width) {
    char visible[512];
    text(r, &r->small, label, left, y, r->material.on_surface, LEFT);
    int value_x = left + text_width(&r->small, label) + 8, room = left + width - value_x;
    shorten(&r->small, value, room > 0 ? room : 0, visible, sizeof(visible));
    text(r, &r->small, visible, value_x, y, r->material.on_surface, LEFT);
}

static void boot_visual_preview(struct mainui_renderer *r, struct mainui_frontend *fe,
                                enum mainui_screen screen, int focus, int brickos) {
    // Random shows the applied image, which the next boot shows, under the random caption.
    int bootlogo = screen == MAINUI_SCREEN_BOOTLOGO;
    const struct mainui_boot_visual *selected = focus == 0 ? boot_visual_applied(fe, bootlogo) : boot_visual_entry(fe, bootlogo, focus);
    const char *current = boot_visual_applied_name(fe, bootlogo);
    const char *caption = focus != 0 ? (selected ? selected->name : NULL) :
                          language_is_portuguese(fe) ? "Muda em cada arranque" : "Changes at every boot";

    if (brickos) {
        int x = 530, y = 99, w = 459, h = 548;
        fill(r, x, y, w, h, r->material.surface_container);
        brickos_bevel(r, x, y, w, h, 0);
        text(r, &r->content, language_is_portuguese(fe) ? "Pré-visualização" : "Preview", x + 18, y + 16, r->material.on_surface, LEFT);
        int px = x + 18, py = y + 54, pw = w - 36, ph = 390;
        fill(r, px, py, pw, ph, r->material.on_surface);
        brickos_bevel(r, px, py, pw, ph, 0);
        struct mainui_render_image *img = selected ? image(r, selected->path) : NULL;
        if (img) draw_fit(r, img, px + 8, py + 8, pw - 16, ph - 16);
        else text(r, &r->small, language_is_portuguese(fe) ? "Pré-visualização indisponível" : "Preview unavailable",
                  x + w / 2, py + ph / 2, r->material.surface, CENTER);
        char selected_name[MAINUI_BOOT_VISUAL_NAME + 8];
        if (caption) text(r, &r->small, shorten(&r->small, caption, w - 36, selected_name, sizeof(selected_name)),
                          x + 18, y + 462, r->material.on_surface, LEFT);
        brickos_preview_caption(r, language_is_portuguese(fe) ? "Atual:" : "Current:", current, x + 18, y + 497, w - 36);
        return;
    }

    int x = 520, y = 73, w = 484, h = 600;
    fill(r, x, y, w, h, (SDL_Color){0, 0, 0, 150});
    outline(r, x, y, w, h, 2, (SDL_Color){255, 255, 255, 80});
    text(r, &r->content, language_is_portuguese(fe) ? "Pré-visualização" : "Preview", x + 24, y + 18, white, LEFT);
    int px = x + 22, py = y + 64, pw = w - 44, ph = 382;
    fill(r, px, py, pw, ph, (SDL_Color){0, 0, 0, 255});
    outline(r, px, py, pw, ph, 2, (SDL_Color){255, 255, 255, 120});
    struct mainui_render_image *img = selected ? image(r, selected->path) : NULL;
    if (img) draw_fit(r, img, px + 8, py + 8, pw - 16, ph - 16);
    else text(r, &r->small, language_is_portuguese(fe) ? "Pré-visualização indisponível" : "Preview unavailable",
              x + w / 2, py + ph / 2, white, CENTER);
    char selected_name[MAINUI_BOOT_VISUAL_NAME + 8], current_name[MAINUI_BOOT_VISUAL_NAME + 8];
    if (caption) text(r, &r->content, shorten(&r->content, caption, w - 48,
                                               selected_name, sizeof(selected_name)), x + 24, y + 470, white, LEFT);
    text(r, &r->small, language_is_portuguese(fe) ? "Imagem atual:" : "Current image:", x + 24, y + 520, white, LEFT);
    text(r, &r->small, shorten(&r->small, current, w - 48, current_name, sizeof(current_name)),
         x + 24, y + 552, white, LEFT);
}

static SDL_Texture *loading_preview_image(struct mainui_renderer *r, const struct mainui_loading_screen *preset,
                                          int frame, int *w, int *h) {
    struct mainui_loading_preview *p = &r->loading_preview;
    if (!p->images[frame] && !p->failed[frame]) {
        char device[MAINUI_LOADING_SCREEN_PATH], full[PATH_BYTES];
        int n = mainui_loading_screen_file(preset, frame, device, sizeof(device)) == 0 ?
                snprintf(full, sizeof(full), "%s%s", r->fs->root, device) : -1;
        SDL_Surface *surface = n >= 0 && n < (int)sizeof(full) ? IMG_Load(full) : NULL;
        if (surface) {
            p->images[frame] = SDL_CreateTextureFromSurface(r->sdl, surface);
            if (p->images[frame]) SDL_SetTextureScaleMode(p->images[frame], SDL_ScaleModeLinear);
            p->w[frame] = surface->w;
            p->h[frame] = surface->h;
            SDL_FreeSurface(surface);
        }
        // A file removed or damaged after the list was built stays unavailable until the focus moves.
        if (!p->images[frame]) p->failed[frame] = 1;
    }
    *w = p->w[frame];
    *h = p->h[frame];
    return p->images[frame];
}

// The background fitted in the box, and the frame due now over it, scaled by the same factor and
// centered as RetroArch shows them full screen. Frames advance every 100 ms from the moment the
// preset got the focus. Returns 0 when an image cannot be decoded.
static int loading_preview_draw(struct mainui_renderer *r, const struct mainui_loading_screen *preset,
                                int x, int y, int w, int h) {
    struct mainui_loading_preview *p = &r->loading_preview;
    if (strcmp(p->folder, preset->folder) != 0) {
        loading_preview_clear(r);
        snprintf(p->folder, sizeof(p->folder), "%s", preset->folder);
        p->started_ms = r->frame_ms;
    }
    uint64_t elapsed = r->frame_ms >= p->started_ms ? r->frame_ms - p->started_ms : 0;
    int frame = preset->frames > 0 ? (int)((elapsed / 100) % (uint64_t)preset->frames) + 1 : 1;
    int bw, bh, fw, fh;
    SDL_Texture *background = loading_preview_image(r, preset, 0, &bw, &bh);
    SDL_Texture *image = loading_preview_image(r, preset, frame, &fw, &fh);
    if (!background || !image || bw <= 0 || bh <= 0) return 0;
    double scale = (double)w / bw;
    if ((double)h / bh < scale) scale = (double)h / bh;
    int dw = (int)(bw * scale), dh = (int)(bh * scale), sw = (int)(fw * scale), sh = (int)(fh * scale);
    SDL_Rect back = {x + (w - dw) / 2, y + (h - dh) / 2, dw, dh};
    SDL_Rect front = {back.x + (dw - sw) / 2, back.y + (dh - sh) / 2, sw, sh};
    SDL_RenderCopy(r->sdl, background, NULL, &back);
    SDL_RenderCopy(r->sdl, image, NULL, &front);
    return 1;
}

static void loading_screen_preview(struct mainui_renderer *r, struct mainui_frontend *fe, int focus, int brickos) {
    int portuguese = language_is_portuguese(fe);
    const struct mainui_loading_screen *preset = focus > 0 && focus <= fe->loading_screen_count ?
                                                 &fe->loading_screens[focus - 1] : NULL;
    if (!preset) loading_preview_clear(r);
    // RetroArch draws the TRIMUI logo itself, so there is no image to show for it.
    const char *message = preset ? (portuguese ? "Pré-visualização indisponível" : "Preview unavailable") :
                                   (portuguese ? "Usa o logótipo TRIMUI incorporado" : "Uses the built-in TRIMUI logo");
    char selected_name[MAINUI_LOADING_SCREEN_NAME + 8], current_name[MAINUI_LOADING_SCREEN_NAME + 8];
    char visible_message[128];
    if (brickos) {
        int x = 530, y = 99, w = 459, h = 548;
        fill(r, x, y, w, h, r->material.surface_container);
        brickos_bevel(r, x, y, w, h, 0);
        text(r, &r->content, portuguese ? "Pré-visualização" : "Preview", x + 18, y + 16, r->material.on_surface, LEFT);
        int px = x + 18, py = y + 54, pw = w - 36, ph = 390;
        fill(r, px, py, pw, ph, r->material.on_surface);
        brickos_bevel(r, px, py, pw, ph, 0);
        if (!preset || !loading_preview_draw(r, preset, px + 8, py + 8, pw - 16, ph - 16))
            text(r, &r->small, shorten(&r->small, message, pw - 24, visible_message, sizeof(visible_message)),
                 x + w / 2, py + ph / 2, r->material.surface, CENTER);
        text(r, &r->small, shorten(&r->small, loading_screen_label(fe, focus), w - 36, selected_name, sizeof(selected_name)),
             x + 18, y + 462, r->material.on_surface, LEFT);
        brickos_preview_caption(r, portuguese ? "Atual:" : "Current:", loading_screen_value(fe), x + 18, y + 497, w - 36);
        return;
    }
    int x = 520, y = 73, w = 484, h = 600;
    fill(r, x, y, w, h, (SDL_Color){0, 0, 0, 150});
    outline(r, x, y, w, h, 2, (SDL_Color){255, 255, 255, 80});
    text(r, &r->content, portuguese ? "Pré-visualização" : "Preview", x + 24, y + 18, white, LEFT);
    int px = x + 22, py = y + 64, pw = w - 44, ph = 382;
    fill(r, px, py, pw, ph, (SDL_Color){0, 0, 0, 255});
    outline(r, px, py, pw, ph, 2, (SDL_Color){255, 255, 255, 120});
    if (!preset || !loading_preview_draw(r, preset, px + 8, py + 8, pw - 16, ph - 16))
        text(r, &r->small, shorten(&r->small, message, pw - 24, visible_message, sizeof(visible_message)),
             x + w / 2, py + ph / 2, white, CENTER);
    text(r, &r->content, shorten(&r->content, loading_screen_label(fe, focus), w - 48, selected_name, sizeof(selected_name)),
         x + 24, y + 470, white, LEFT);
    text(r, &r->small, portuguese ? "Atual:" : "Current:", x + 24, y + 520, white, LEFT);
    text(r, &r->small, shorten(&r->small, loading_screen_value(fe), w - 48, current_name, sizeof(current_name)),
         x + 24, y + 552, white, LEFT);
}

static void brickos_empty_state(struct mainui_renderer *r, const struct mainui_frontend *fe);

static const char *brickos_initial_view_label(const struct mainui_frontend *fe,
                                              enum mainui_brickos_initial_view view) {
    int portuguese = language_is_portuguese(fe);
    switch (view) {
    case MAINUI_BRICKOS_VIEW_DESKTOP: return portuguese ? "Início" : "Home";
    case MAINUI_BRICKOS_VIEW_GAMES: return portuguese ? "Jogos" : "Games";
    case MAINUI_BRICKOS_VIEW_FAVORITES: return portuguese ? "Favoritos" : "Favorites";
    case MAINUI_BRICKOS_VIEW_RECENT: return portuguese ? "Recentes" : "Recent";
    case MAINUI_BRICKOS_VIEW_GAMESWITCHER: return "GameSwitcher";
    case MAINUI_BRICKOS_VIEW_APPS: return "Apps";
    case MAINUI_BRICKOS_VIEW_LAST_USED: return portuguese ? "Último ecrã usado" : "Last used";
    default: return "Desktop";
    }
}

// A font's name in the Appearance screens: the fonts folder's name without the extension, or
// Default ("") in the language.
static const char *appearance_font_name(const struct mainui_frontend *fe, const char *file) {
    for (int i = 0; file[0] && i < fe->font_count; ++i)
        if (strcmp(fe->fonts[i].file, file) == 0) return fe->fonts[i].name;
    return mainui_appearance_text("Default", language_is_portuguese(fe));
}

// A Language row's name: the one its file holds under "lang", else the file name.
static const char *language_name(const struct mainui_frontend *fe, int entry) {
    return fe->language_names[entry][0] ? fe->language_names[entry] : fe->languages[entry];
}

// Whether a Language row is the language in use: the file system.json names, or English when no
// listed file is that one, as MainUI then loads English.
static int language_applied(const struct mainui_frontend *fe, int entry) {
    for (int i = 0; i < fe->language_count; ++i)
        if (strcmp(fe->languages[i], fe->settings.language) == 0) return i == entry;
    return strcmp(fe->languages[entry], "en.lang") == 0;
}

static void brickos_settings_window(struct mainui_renderer *r, struct mainui_frontend *fe,
                                   const struct mainui_window *w) {
    struct mainui_item fixed_items[128];
    int boot_visual_selector = w->screen == MAINUI_SCREEN_BOOTLOGO || w->screen == MAINUI_SCREEN_SPLASHSCREEN ||
                               w->screen == MAINUI_SCREEN_LOADING_SCREEN;
    // Rows beyond 128 (long image lists, extension screens and sections) get their own array.
    int capacity = mainui_model_screen_item_capacity(&fe->model, w->screen);
    int allocated = capacity > 128;
    if (!allocated) capacity = 128;
    struct mainui_item *items = allocated ? calloc((size_t)capacity, sizeof(*items)) : fixed_items;
    if (!items) return;
    int count = mainui_model_screen_items(&fe->model, w->screen, items, capacity);
    if (count <= 0) {
        if (allocated) free(items);
        brickos_empty_state(r, fe);
        return;
    }
    int focus = w->focus >= 0 && w->focus < count ? w->focus : count - 1;
    // A row drawn in a font of its own (the Font list, and a Language name the content font
    // cannot draw) has its font's file and its whole name.
    struct material_setting_row { char label[512], detail[256], font_file[128]; const char *font, *name; };
    struct material_setting_row *rows = calloc((size_t)count, sizeof(*rows));
    if (!rows) {
        if (allocated) free(items);
        return;
    }
    for (int index = 0; index < count; ++index) {
        const struct mainui_item *item = &items[index];
        char detail[256] = "", value[96];
        const char *label = item_label(fe, item->label), *shown_label = label;
        if (item->kind == MAINUI_ITEM_HEADER) {
            snprintf(rows[index].label, sizeof(rows[index].label), "%s", label);
            continue;
        }
        char extension_label[256];
        if (item->kind == MAINUI_ITEM_EXTENSION) {
            shown_label = ext_label(fe, item->action, extension_label, sizeof(extension_label));
            ext_value(fe, item->action, 1, detail, sizeof(detail));
        } else if (item->kind == MAINUI_ITEM_RANGE) {
            int v = fe->model.settings[item->setting];
            range_text(item, v, " / ", detail, sizeof(detail));
        } else if (item->kind == MAINUI_ITEM_CYCLE) {
            snprintf(detail, sizeof(detail), "%s", cycle_text(fe, item->setting,
                     fe->model.settings[item->setting], value, sizeof(value)));
        } else if (item->kind == MAINUI_ITEM_SWITCH) {
            int v = fe->model.settings[item->setting];
            snprintf(detail, sizeof(detail), "%s", v ? (language_is_portuguese(fe) ? "Ligado" : "On") :
                     (language_is_portuguese(fe) ? "Desligado" : "Off"));
        } else if (item->kind == MAINUI_ITEM_DATE && item->action >= 0 && item->action < 6) {
            snprintf(detail, sizeof(detail), "%d", w->date[item->action]);
        } else if (item->kind == MAINUI_ITEM_PRESENTATION_CHOICE) {
            if (item->action == (int)fe->model.presentation)
                snprintf(detail, sizeof(detail), "%s", language_is_portuguese(fe) ? "Ativo" : "Active");
        } else if (item->kind == MAINUI_ITEM_SUBMENU && item->screen == MAINUI_SCREEN_FRONTEND) {
            snprintf(detail, sizeof(detail), "%s", fe->model.presentation == MAINUI_PRESENTATION_STOCK ? "Stock++" : "BrickOS");
        } else if (item->kind == MAINUI_ITEM_INITIAL_VIEW_CHOICE) {
            shown_label = brickos_initial_view_label(fe, (enum mainui_brickos_initial_view)item->action);
            if (item->action == (int)fe->model.brickos_initial_view)
                snprintf(detail, sizeof(detail), "%s", language_is_portuguese(fe) ? "Destino atual" : "Current destination");
        } else if (item->kind == MAINUI_ITEM_INFO && w->screen == MAINUI_SCREEN_DEVICE_INFO && device_info_value(fe, items, index)) {
            snprintf(detail, sizeof(detail), "%s", device_info_value(fe, items, index));
        } else if (item->kind == MAINUI_ITEM_INFO && w->screen == MAINUI_SCREEN_BUTTON_MAPPING) {
            snprintf(detail, sizeof(detail), "%s", mapping_status_text(fe));
        } else if (item->kind == MAINUI_ITEM_ACTION && item->action == MAINUI_ACTION_SSID) {
            snprintf(detail, sizeof(detail), "%s", fe->hotspot_ssid);
        } else if (item->kind == MAINUI_ITEM_ACTION && item->action == MAINUI_ACTION_PASSWORD) {
            snprintf(detail, sizeof(detail), "%s", fe->hotspot_password[0] ? "********" :
                     (language_is_portuguese(fe) ? "Não definido" : "Not set"));
        } else if (item->kind == MAINUI_ITEM_SUBMENU && item->screen == MAINUI_SCREEN_TIMEZONE) {
            snprintf(detail, sizeof(detail), "%s", fe->settings.timezone[0] ? fe->settings.timezone :
                     (language_is_portuguese(fe) ? "Predefinido" : "Default"));
        } else if (item->kind == MAINUI_ITEM_SUBMENU && item->screen == MAINUI_SCREEN_LOADING_SCREEN) {
            snprintf(detail, sizeof(detail), "%s", loading_screen_value(fe));
        } else if (item->kind == MAINUI_ITEM_SUBMENU && item->screen == MAINUI_SCREEN_APPEARANCE_FONT) {
            snprintf(detail, sizeof(detail), "%s", appearance_font_name(fe, fe->appearance.font));
        } else if (item->kind == MAINUI_ITEM_SUBMENU &&
                   (item->screen == MAINUI_SCREEN_BOOTLOGO || item->screen == MAINUI_SCREEN_SPLASHSCREEN)) {
            snprintf(detail, sizeof(detail), "%s", boot_visual_value(fe, item->screen == MAINUI_SCREEN_BOOTLOGO));
        } else if (item->kind == MAINUI_ITEM_LIST) {
            int start = index;
            while (start > 0 && items[start - 1].kind == MAINUI_ITEM_LIST && items[start - 1].action == item->action) --start;
            int entry = index - start;
            if (item->action == MAINUI_LIST_LANGUAGES && entry < fe->language_count) {
                shown_label = language_name(fe, entry);
                if (language_applied(fe, entry))
                    snprintf(detail, sizeof(detail), "%s", language_is_portuguese(fe) ? "Atual" : "Current");
                // A name the content font cannot draw, such as 한국어 in regular.ttf, is drawn in
                // its own language's Default font.
                if (!font_draws_text(&r->content, shown_label) &&
                    mainui_asset_font(r->fs, MAINUI_DEFAULT_THEME_DIR, "", fe->languages[entry], rows[index].font_file,
                                      sizeof(rows[index].font_file)) >= 0) {
                    rows[index].font = rows[index].font_file;
                    rows[index].name = shown_label;
                }
            } else if (item->action == MAINUI_LIST_TIMEZONES && w->system >= 0 && w->system < MAINUI_ZONE_LEVELS &&
                       entry < fe->zones[w->system].count) {
                shown_label = fe->zones[w->system].zones[entry].name;
                if (fe->zones[w->system].zones[entry].folder)
                    snprintf(detail, sizeof(detail), "%s", language_is_portuguese(fe) ? "Pasta" : "Folder");
            } else if (item->action == MAINUI_LIST_BOOTLOGOS || item->action == MAINUI_LIST_SPLASHSCREENS) {
                int bootlogo = item->action == MAINUI_LIST_BOOTLOGOS;
                const struct mainui_boot_visual *visual = boot_visual_entry(fe, bootlogo, entry);
                if (entry == 0) shown_label = boot_visual_random_label(fe);
                else if (visual) shown_label = visual->name;
                if (boot_visual_entry_ticked(fe, bootlogo, entry))
                    snprintf(detail, sizeof(detail), "%s", language_is_portuguese(fe) ? "Atual" : "Current");
            } else if (item->action == MAINUI_LIST_FONTS && entry <= fe->font_count) {
                // Default, then the fonts folder's fonts, each name drawn in its own font.
                rows[index].font = entry ? fe->fonts[entry - 1].file : "";
                shown_label = appearance_font_name(fe, rows[index].font);
                if (strcmp(rows[index].font, fe->appearance.font) == 0)
                    snprintf(detail, sizeof(detail), "%s", language_is_portuguese(fe) ? "Atual" : "Current");
            } else if (item->action == MAINUI_LIST_LOADING_SCREENS) {
                shown_label = loading_screen_label(fe, entry);
                if (loading_screen_installed(fe, entry))
                    snprintf(detail, sizeof(detail), "%s", language_is_portuguese(fe) ? "Atual" : "Current");
            } else if (item->action == MAINUI_LIST_WIFI && entry < fe->wifi_count) {
                shown_label = fe->wifi[entry].ssid;
                if (fe->wifi[entry].connected) snprintf(detail, sizeof(detail), "%s", language_is_portuguese(fe) ? "Ligado" : "Connected");
                else if (fe->wifi[entry].locked) snprintf(detail, sizeof(detail), "%s", language_is_portuguese(fe) ? "Protegida" : "Secured");
            } else if (item->action == MAINUI_LIST_PAIRED && entry < fe->paired_count) {
                shown_label = fe->paired[entry].name[0] ? fe->paired[entry].name : fe->paired[entry].addr;
                snprintf(detail, sizeof(detail), "%s", fe->paired[entry].connected ? "Connected" : "Disconnected");
            } else if (item->action == MAINUI_LIST_BLUETOOTH && entry < fe->found_count) {
                shown_label = fe->found[entry].name[0] ? fe->found[entry].name : fe->found[entry].addr;
            }
        }
        int text_width = (boot_visual_selector ? 470 : 942) - 48;
        shorten_line(&r->content, shown_label, text_width, rows[index].label, sizeof(rows[index].label));
        shorten(&r->small, detail, text_width, rows[index].detail, sizeof(rows[index].detail));
    }
    int height = material_list_row_height(r);
    int first = material_list_first(fe, w, count, height, MATERIAL_LIST_BOTTOM - 99);
    // The focused extension row's description takes the line above the footer.
    char description[512], visible_description[512];
    const char *focused_description = ext_description(fe, items, count, focus, description, sizeof(description));
    int bottom = focused_description ? MATERIAL_LIST_BOTTOM - 40 : MATERIAL_LIST_BOTTOM;
    if (focused_description) first = material_list_first(fe, w, count, height, bottom - 99);
    for (int index = first, y = 99; index < count; ++index) {
        if (y + height > bottom) break;
        if (items[index].kind == MAINUI_ITEM_HEADER) {
            draw_text(r, &r->small, rows[index].label, 59, y + 12, height - 24, r->material.primary, LEFT);
        } else if (items[index].kind == MAINUI_ITEM_EXTENSION) {
            const struct mainui_ext_node *n = mainui_ext_node(&fe->ext, items[index].action);
            int width = boot_visual_selector ? 470 : 942;
            const char *icon = NULL;
            char icon_path[PATH_BYTES];
            if (n && n->icon) {
                snprintf(icon_path, sizeof(icon_path), "%s/%s", ext_device_path(fe, mainui_ext_node_dir(&fe->ext, items[index].action)), n->icon);
                icon = icon_path;
            }
            struct mainui_render_image *picture = icon ? image_resized_style(r, icon, 40, 40, 1) : NULL;
            brickos_list_row(r, 35, y, width, height, rows[index].label, rows[index].detail, index == focus,
                             n && !n->disabled, picture != NULL);
            if (picture) {
                SDL_Color ink = index == focus ? r->material.on_primary_container : r->material.primary;
                SDL_SetTextureColorMod(picture->texture, ink.r, ink.g, ink.b);
                draw_at(r, picture, 35 + 18, y + (height - 40) / 2);
                SDL_SetTextureColorMod(picture->texture, 255, 255, 255);
            }
            if (n && n->kind == MAINUI_EXT_MENU)
                text(r, &r->content, ">", 35 + width - 30, y + height / 2 - 18,
                     index == focus ? r->material.on_primary_container : r->material.on_surface_variant, CENTER);
        } else if (rows[index].font) {
            // A font list row, or a Language row: its name in its own font (the content font when
            // it does not open).
            struct mainui_font sample = r->content;
            TTF_Font *own = font_sample(r, rows[index].font);
            char visible[512];
            if (own) {
                sample.font = own;
                sample.fallback = NULL;
                sample.mixed = 0;
                sample.shift = 0;
            }
            shorten_line(&sample, rows[index].name ? rows[index].name : appearance_font_name(fe, rows[index].font),
                         942 - 48, visible, sizeof(visible));
            brickos_list_row_font(r, &sample, 35, y, 942, height, visible, rows[index].detail, index == focus, 1, 0);
        } else {
            // Button Mapping's rows are read-only, greyed, while the profile is invalid.
            brickos_list_row(r, 35, y, boot_visual_selector ? 470 : 942, height,
                            rows[index].label, rows[index].detail, index == focus, !mapping_locked_row(fe, &items[index]), 0);
        }
        y += height + MATERIAL_LIST_GAP;
    }
    if (focused_description)
        text(r, &r->small, shorten(&r->small, focused_description, 940, visible_description, sizeof(visible_description)),
             43, bottom + 8, r->material.on_surface_variant, LEFT);
    if (w->screen == MAINUI_SCREEN_BOOTLOGO || w->screen == MAINUI_SCREEN_SPLASHSCREEN)
        boot_visual_preview(r, fe, w->screen, focus, 1);
    else if (w->screen == MAINUI_SCREEN_LOADING_SCREEN)
        loading_screen_preview(r, fe, focus, 1);
    free(rows);
    if (allocated) free(items);
}

static int brickos_catalog_tab(enum mainui_tab tab) {
    return tab == MAINUI_TAB_GAMES || tab == MAINUI_TAB_BESTS || tab == MAINUI_TAB_APPS || tab == MAINUI_TAB_PORTS;
}

static void brickos_status(struct mainui_renderer *r, struct mainui_frontend *fe) {
    char clock[16], percent[12];
    const char *half = mainui_status_clock(r->status.clock, fe->settings.values[MAINUI_SET_24HOURCLOCK],
                                           clock, sizeof(clock));
    snprintf(percent, sizeof(percent), "%d%%", r->status.battery);
    SDL_Color ink = r->material.on_surface;
    if (fe->settings.values[MAINUI_SET_WIFI]) {
        SDL_Point wifi[] = {{755, 42}, {765, 34}, {775, 42}};
        material_icon_color(r, ink);
        SDL_RenderDrawLines(r->sdl, wifi, 3);
        material_icon_circle(r, 765, 45, 2, ink, 1);
    }
    text(r, &r->status_text, percent, 844, 28, r->material.on_surface_variant, RIGHT);
    // Align the icon to visible digits, including the theme's baseline shift.
    int shift = 0, min_y = 0, max_y = 0, center_y = 44;
    TTF_Font *status_font = font_for_text(&r->status_text, clock, &shift);
    if (status_font && TTF_GlyphMetrics(status_font, '0', NULL, NULL, &min_y, &max_y, NULL) == 0)
        center_y = 28 + shift + TTF_FontAscent(status_font) - (min_y + max_y) / 2;
    material_icon_color(r, r->material.on_surface_variant);
    SDL_Rect battery = {863, center_y - 13, 36, 26};
    SDL_RenderDrawRect(r->sdl, &battery);
    SDL_Rect terminal = {900, center_y - 5, 3, 10};
    SDL_RenderFillRect(r->sdl, &terminal);
    int charge = r->status.battery < 0 ? 0 : r->status.battery > 100 ? 100 : r->status.battery;
    SDL_Rect level = {866, center_y - 10, 30 * charge / 100, 20};
    material_icon_color(r, r->status.charging ? r->material.primary : r->material.on_surface);
    SDL_RenderFillRect(r->sdl, &level);
    text(r, &r->status_text, clock, 986, 28, r->material.on_surface, RIGHT);
    if (half) text(r, &r->hint, half, 986, 50, r->material.on_surface_variant, RIGHT);
}

// The footer hints keep their size at every Text size.
static int material_hint_width(struct mainui_renderer *r, const char *key, const char *label) {
    int key_width = text_width(&r->hint, key);
    if (key_width < 20) key_width = 20;
    return key_width + 18 + 8 + text_width(&r->hint, label);
}

static void material_footer_hints(struct mainui_renderer *r, const char *const *keys,
                                  const char *const *labels, int count) {
    if (count <= 0 || count > MAINUI_HINTS_MAX) return;
    int widths[MAINUI_HINTS_MAX], total = 0, gap = count >= 6 ? 12 : 24;
    for (int i = 0; i < count; ++i) {
        widths[i] = material_hint_width(r, keys[i], labels[i]);
        total += widths[i] + (i ? gap : 0);
    }
    int split = count;
    if (total > 960) {
        // Balance two lines using actual localized glyph widths, with 32px side margins.
        int best = total;
        for (int i = 1, left = 0; i < count; ++i) {
            left += widths[i - 1] + (i > 1 ? gap : 0);
            int right = total - left - gap;
            int widest = left > right ? left : right;
            if (widest < best) { best = widest; split = i; }
        }
    }
    for (int first = 0; first < count;) {
        int end = first == 0 ? split : count, line_width = 0;
        for (int i = first; i < end; ++i) line_width += widths[i] + (i > first ? gap : 0);
        int x = (MAINUI_SCREEN_W - line_width) / 2;
        int y = split == count || first > 0 ? 713 : 677;
        for (int i = first; i < end; ++i) {
            int key_width = widths[i] - 8 - text_width(&r->hint, labels[i]);
            fill(r, x, y + 2, key_width, 28, r->material.surface_variant);
            text(r, &r->hint, keys[i], x + key_width / 2, y + 4, r->material.on_surface, CENTER);
            text(r, &r->hint, labels[i], x + key_width + 8, y + 4, r->material.on_surface_variant, LEFT);
            x += widths[i] + gap;
        }
        first = end;
    }
}

static void brickos_footer(struct mainui_renderer *r, const struct mainui_frontend *fe) {
    static const char *const labels[MAINUI_HINT_COUNT][2] = {
        [MAINUI_HINT_MOVE] = {"Move", "Mover"}, [MAINUI_HINT_OPEN] = {"Open", "Abrir"},
        [MAINUI_HINT_LAUNCH] = {"Launch", "Lançar"}, [MAINUI_HINT_SEARCH] = {"Search", "Pesquisar"},
        [MAINUI_HINT_BACK] = {"Back", "Voltar"}, [MAINUI_HINT_ACTIONS] = {"Actions", "Ações"},
        [MAINUI_HINT_LAYOUT] = {"Layout", "Layout"}, [MAINUI_HINT_PAGE] = {"Page", "Página"},
        [MAINUI_HINT_EMULATOR] = {"Emulator", "Emulador"}, [MAINUI_HINT_GROUP] = {"Group", "Grupo"},
        [MAINUI_HINT_CHANGE] = {"Change", "Alterar"}, [MAINUI_HINT_TOGGLE] = {"Toggle", "Alternar"},
        [MAINUI_HINT_APPLY] = {"Apply", "Aplicar"}, [MAINUI_HINT_CONNECT] = {"Connect", "Ligar"},
        [MAINUI_HINT_CHOOSE] = {"Choose", "Escolher"}, [MAINUI_HINT_SELECT] = {"Select", "Selecionar"},
        [MAINUI_HINT_CANCEL] = {"Cancel", "Cancelar"}, [MAINUI_HINT_CONFIRM] = {"Confirm", "Confirmar"},
        [MAINUI_HINT_UPDATE] = {"Update", "Atualizar"}, [MAINUI_HINT_CLOSE] = {"Close", "Fechar"},
        [MAINUI_HINT_SAVE] = {"Save", "Guardar"}, [MAINUI_HINT_TYPE] = {"Type", "Escrever"},
        [MAINUI_HINT_SHIFT] = {"Shift", "Shift"}, [MAINUI_HINT_SYMBOLS] = {"Symbols", "Símbolos"},
        [MAINUI_HINT_SPACE] = {"Space", "Espaço"}, [MAINUI_HINT_DELETE] = {"Delete", "Apagar"},
        [MAINUI_HINT_DONE] = {"Done", "Concluir"}, [MAINUI_HINT_KEYBOARD] = {"Keyboard", "Teclado"},
        [MAINUI_HINT_EXIT] = {"Exit", "Sair"}, [MAINUI_HINT_POWER_OFF] = {"Power off", "Desligar"},
        [MAINUI_HINT_REBOOT] = {"Reboot", "Reiniciar"},
    };
    struct mainui_hint available[MAINUI_HINTS_MAX];
    const char *keys[MAINUI_HINTS_MAX], *text_labels[MAINUI_HINTS_MAX];
    int count = mainui_model_hints(&fe->model, available);
    int portuguese = language_is_portuguese(fe);
    for (int i = 0; i < count; ++i) {
        keys[i] = available[i].key;
        text_labels[i] = labels[available[i].action][portuguese];
    }
    material_footer_hints(r, keys, text_labels, count);
}
static void brickos_empty_state(struct mainui_renderer *r, const struct mainui_frontend *fe) {
    const char *message = say(fe, 144, "No items available");
    fill(r, 160, 288, 704, 112, r->material.surface_container);
    brickos_bevel(r, 160, 288, 704, 112, 0);
    struct mainui_font label = r->content;
    label.shift = 0;
    text(r, &label, message, 512, 329, r->material.on_surface, CENTER);
}

static void material_thumbnail_row(struct mainui_renderer *r, const char *label, const char *detail,
                                   const char *picture_path, enum mainui_tab icon, int index,
                                   int x, int y, int w, int h, int focused) {
    fill(r, x, y, w, h, focused ? r->material.primary_container : r->material.surface_container);
    brickos_bevel(r, x, y, w, h, 0);
    if (focused) brickos_focus(r, x + 2, y + 2, w - 4, h - 4);
    int thumb_x = x + 12, thumb_y = y + (h - 72) / 2;
    fill(r, thumb_x, thumb_y, 112, 72, r->material.surface_variant);
    struct mainui_render_image *cover = picture_path && picture_path[0] ?
        image_resized(r, picture_path, 112, 72) : NULL;
    if (cover) draw_fit(r, cover, thumb_x, thumb_y, 112, 72);
    else material_icon(r, icon, thumb_x + 23, thumb_y + 3);
    struct mainui_font label_font = focused ? r->focus_label : r->content;
    char visible_label[512], visible_detail[256];
    shorten(&label_font, label, w - 166, visible_label, sizeof(visible_label));
    shorten(&r->small, detail ? detail : "", w - 166, visible_detail, sizeof(visible_detail));
    SDL_Color foreground = focused ? r->material.on_primary_container : r->material.on_surface;
    int title_height = material_line_height(&label_font, visible_label);
    int detail_height = visible_detail[0] ? material_line_height(&r->small, visible_detail) : 0;
    int text_y = y + (h - title_height - (detail_height ? 6 + detail_height : 0)) / 2;
    draw_text(r, &label_font, visible_label, x + 144, text_y, title_height, foreground, LEFT);
    if (detail_height)
        draw_text(r, &r->small, visible_detail, x + 144, text_y + title_height + 6, detail_height,
                  r->material.on_surface_variant, LEFT);
    if (index >= 0) {
        char number[16];
        snprintf(number, sizeof(number), "%02d", index + 1);
        text(r, &r->small, number, x + w - 22, y + 10,
             focused ? r->material.on_primary_container : r->material.on_surface_variant, RIGHT);
    }
}

static void brickos_tab_list(struct mainui_renderer *r, struct mainui_frontend *fe) {
    const struct mainui_model *m = &fe->model;
    const struct mainui_tab_state *state = &m->tabs[m->tab];
    int count = m->tab == MAINUI_TAB_RECENT ? fe->recent_count :
                m->tab == MAINUI_TAB_FAVORITES ? fe->favorites_count : fe->host_count;
    if (count <= 0) {
        brickos_empty_state(r, fe);
        return;
    }
    int row_height = material_list_row_height(r);
    int first = material_visible_first(state->focus, state->first, count, row_height, MATERIAL_LIST_BOTTOM - 99);
    fe->model.tabs[m->tab].first = first;
    for (int index = first, y = 99; index < count && y + row_height <= MATERIAL_LIST_BOTTOM;
         ++index, y += row_height + MATERIAL_LIST_GAP) {
        const char *label = "", *detail = "";
        const char *picture = NULL;
        char subtitle[192] = "";
        if (m->tab == MAINUI_TAB_RECENT || m->tab == MAINUI_TAB_FAVORITES) {
            const struct mainui_list_entry *entry = m->tab == MAINUI_TAB_RECENT ?
                mainui_frontend_recent(fe, index) : mainui_frontend_favorite(fe, index);
            if (entry) {
                label = mainui_frontend_entry_label(fe, entry);
                detail = mainui_frontend_entry_system(fe, entry);
                if (!detail && entry->sublabel[0]) detail = entry->sublabel;
                picture = entry->imgpath;
            }
        } else {
            const struct mainui_netplay_host *host = &fe->hosts[index];
            label = host->label;
            snprintf(subtitle, sizeof(subtitle), "%s  %s", host->gamelabel, host->hostip);
            detail = subtitle;
            picture = host->coverpath;
        }
        material_thumbnail_row(r, label, detail, picture, m->tab, index,
                               43, y, 938, row_height, index == state->focus);
    }
}

static int brickos_catalog_count(const struct mainui_frontend *fe, enum mainui_tab tab) {
    switch (tab) {
    case MAINUI_TAB_GAMES: return fe->visible_count;
    case MAINUI_TAB_BESTS: return fe->shown_best_count;
    case MAINUI_TAB_APPS: return fe->shown_app_count;
    case MAINUI_TAB_PORTS: return fe->shown_port_count;
    default: return 0;
    }
}

static void material_icon(struct mainui_renderer *r, enum mainui_tab tab, int x, int y);

// emphasis is 1 for the focused card and 0 for another; a sliding one-row card has a fraction,
// which blends its colours and, past one half, gives it the focus.
static void brickos_catalog_card_at(struct mainui_renderer *r, struct mainui_frontend *fe, int index,
                                    int x, int y, int w, int h, float emphasis, int compact) {
    int focused = emphasis >= 0.5f;
    struct mainui_render_image *picture = NULL;
    const char *label = "";
    char count[64], visible_label[256];
    tile_content(r, fe, index, focused, &picture, &label, count, sizeof(count));
    struct mainui_render_image *overlay = tab_app_icon_layer(r, fe, index);

    fill(r, x, y, w, h, mix_color(r->material.surface_container, r->material.primary_container, emphasis));
    brickos_bevel(r, x, y, w, h, !focused);
    if (focused) brickos_focus(r, x + 2, y + 2, w - 4, h - 4);

    // The name and the count make a block anchored to the bottom; the compact layout keeps the
    // name on one line. A second line takes its room from the picture above.
    const struct mainui_font *label_font = compact ? &r->small : focused ? &r->focus_label : &r->content;
    char rest[256];
    rest[0] = 0;
    if (compact) shorten(label_font, label, w - 16, visible_label, sizeof(visible_label));
    else two_lines(label_font, label, w - 16, visible_label, rest, sizeof(visible_label));
    TTF_Font *label_ttf = visible_label[0] ? font_for_text(label_font, visible_label, NULL) : label_font->font;
    TTF_Font *rest_ttf = rest[0] ? font_for_text(label_font, rest, NULL) : NULL;
    TTF_Font *count_ttf = count[0] ? font_for_text(&r->small, count, NULL) : NULL;
    int label_height = label_ttf ? TTF_FontHeight(label_ttf) : label_font->size;
    int rest_height = !rest[0] ? 0 : rest_ttf ? TTF_FontHeight(rest_ttf) : label_font->size;
    int count_height = count_ttf ? TTF_FontHeight(count_ttf) : r->small.size;
    int text_block = label_height + rest_height + (count[0] && !compact ? count_height + 4 : 0);
    int label_y = y + h - (compact ? 4 : 8) - text_block;

    int icon_height = compact ? h - 34 : h - (count[0] ? 92 : 76);
    int icon_width = compact ? w - 14 : w - 42;
    int icon_x = x + (w - icon_width) / 2;
    int icon_y = y + (compact ? 3 : 12);
    if (rest[0] && label_y - 4 - icon_y < icon_height) icon_height = label_y - 4 - icon_y;
    int symbol_y = y + (compact ? 1 : 18);
    if (rest[0] && icon_y + icon_height - 66 < symbol_y) symbol_y = icon_y + icon_height - 66;
    if (overlay) draw_fit(r, overlay, icon_x, icon_y, icon_width, icon_height);
    if (picture) draw_fit(r, picture, icon_x, icon_y, icon_width, icon_height);
    else if (!overlay) material_icon(r, fe->model.tab, x + (w - 66) / 2, symbol_y);

    if (focused && !compact) fill(r, x + 4, label_y - 1, w - 8, h - (label_y - y) - 4, r->material.primary_container);
    SDL_Color label_color = mix_color(r->material.on_surface, r->material.on_primary_container, emphasis);
    if (compact) {
        text_box(r, &r->small, visible_label, x + 4, w - 8, label_y, label_color);
    } else {
        text_box(r, label_font, visible_label, x + 8, w - 16, label_y, label_color);
        if (rest[0]) text_box(r, label_font, rest, x + 8, w - 16, label_y + label_height, label_color);
        if (count[0]) text_box(r, &r->small, count, x + 8, w - 16, label_y + label_height + rest_height + 4, label_color);
    }
}

static void brickos_catalog_card(struct mainui_renderer *r, struct mainui_frontend *fe, int index,
                                int x, int y, int w, int h, int focused, int compact) {
    brickos_catalog_card_at(r, fe, index, x, y, w, h, focused ? 1.f : 0.f, compact);
}

static void brickos_catalog_grid(struct mainui_renderer *r, struct mainui_frontend *fe) {
    const struct mainui_model *m = &fe->model;
    const struct mainui_tab_state *state = &m->tabs[m->tab];
    int n = brickos_catalog_count(fe, m->tab);
    if (n <= 0) {
        brickos_empty_state(r, fe);
        return;
    }
    int style = mainui_model_tab_layout(m, m->tab);
    char page[48];
    if (style == 2) {
        int pages = (n + MAINUI_COMPACT_COLUMNS * MAINUI_COMPACT_ROWS - 1) /
                    (MAINUI_COMPACT_COLUMNS * MAINUI_COMPACT_ROWS);
        snprintf(page, sizeof(page), "%s %d/%d", strcmp(fe->settings.language, "pt.lang") == 0 ? "PÁGINA" : "PAGE",
                 state->focus / (MAINUI_COMPACT_COLUMNS * MAINUI_COMPACT_ROWS) + 1, pages);
        text(r, &r->small, page, 984, 88, r->material.on_surface, RIGHT);
        int first = state->focus / (MAINUI_COMPACT_COLUMNS * MAINUI_COMPACT_ROWS) *
                    (MAINUI_COMPACT_COLUMNS * MAINUI_COMPACT_ROWS);
        for (int index = first; index < n && index < first + MAINUI_COMPACT_COLUMNS * MAINUI_COMPACT_ROWS; ++index) {
            int position = index - first;
            int x = 31 + position % MAINUI_COMPACT_COLUMNS * 120;
            int y = 119 + position / MAINUI_COMPACT_COLUMNS * 105;
            brickos_catalog_card(r, fe, index, x, y, 112, 96, index == state->focus, 1);
        }
    } else if (style == 0) {
        // The centre card is 350x382 at x 337, y 166, and its neighbours 245x322 at x 50 and 730,
        // y 196: 340 pixels apart centre to centre. While the row slides (the frontend's carousel
        // shift), each card's place, size and colours follow its distance from the centre, and the
        // card coming in enters from the edge.
        float shift = r->animations_enabled && fe->carousel.valid && fe->carousel.kind == MAINUI_WIN_TABS ?
                      mainui_frontend_carousel_shift(fe, r->frame_ms) : 0;
        int reach = shift != 0 ? 2 : 1;
        for (int pass = reach; pass >= 0; --pass) {
            for (int side = -1; side <= 1; side += 2) {
                if (!pass && side > 0) break;
                int offset = pass * side;
                float d = offset + shift;
                if (fabsf(d) > 2.2f) continue;
                float off = fminf(fabsf(d), 1.f);
                int w = (int)lroundf(350 - 105 * off), h = (int)lroundf(382 - 60 * off);
                float reach_px = fabsf(d) <= 1 ? 340 * d : (d > 0 ? 1.f : -1.f) * (340 + 340 * (fabsf(d) - 1));
                int x = (int)lroundf(512 + reach_px) - w / 2;
                int index = ((state->focus + offset) % n + n) % n;
                brickos_catalog_card_at(r, fe, index, x, (int)lroundf(166 + 30 * off), w, h, 1 - off, 0);
            }
        }
    } else {
        int pages = (n + MAINUI_GRID_COLUMNS * MAINUI_GRID_ROWS - 1) /
                    (MAINUI_GRID_COLUMNS * MAINUI_GRID_ROWS);
        snprintf(page, sizeof(page), "%s %d/%d", strcmp(fe->settings.language, "pt.lang") == 0 ? "PÁGINA" : "PAGE",
                 state->focus / (MAINUI_GRID_COLUMNS * MAINUI_GRID_ROWS) + 1, pages);
        text(r, &r->small, page, 984, 88, r->material.on_surface, RIGHT);
        int first = state->focus / (MAINUI_GRID_COLUMNS * MAINUI_GRID_ROWS) *
                    (MAINUI_GRID_COLUMNS * MAINUI_GRID_ROWS);
        for (int index = first; index < n && index < first + MAINUI_GRID_COLUMNS * MAINUI_GRID_ROWS; ++index) {
            int position = index - first;
            int x = 48 + position % MAINUI_GRID_COLUMNS * 312;
            int y = 132 + position / MAINUI_GRID_COLUMNS * 270;
            brickos_catalog_card(r, fe, index, x, y, 292, 244, index == state->focus, 0);
        }
    }
}

static void brickos_entry_icon(struct mainui_renderer *r, int x, int y, int folder) {
    fill(r, x, y, 42, 42, r->material.surface_variant);
    if (folder) {
        material_icon_color(r, r->material.primary);
        rounded_fill(r->sdl, x + 7, y + 10, 16, 8, 3);
        rounded_fill(r->sdl, x + 5, y + 16, 32, 19, 5);
        material_icon_color(r, r->material.on_primary);
        SDL_RenderDrawLine(r->sdl, x + 10, y + 22, x + 32, y + 22);
    } else {
        fill(r, x + 10, y + 6, 22, 30, r->material.surface);
        outline(r, x + 10, y + 6, 22, 30, 2, r->material.primary);
        material_icon_color(r, r->material.on_surface_variant);
        SDL_RenderDrawLine(r->sdl, x + 15, y + 16, x + 27, y + 16);
        SDL_RenderDrawLine(r->sdl, x + 15, y + 21, x + 27, y + 21);
        SDL_RenderDrawLine(r->sdl, x + 15, y + 26, x + 24, y + 26);
    }
}

static void brickos_roms(struct mainui_renderer *r, struct mainui_frontend *fe,
                        const struct mainui_window *w) {
    const struct mainui_system *system = mainui_frontend_system(fe, w->system);
    int count = mainui_frontend_roms(fe, w->system);
    if (count <= 0 || !fe->games) {
        brickos_empty_state(r, fe);
        return;
    }
    int row_height = material_list_row_height(r), top = 129;
    int rows = (MATERIAL_LIST_BOTTOM - top + MATERIAL_LIST_GAP) / (row_height + MATERIAL_LIST_GAP);
    int first = material_list_first(fe, w, count, row_height, MATERIAL_LIST_BOTTOM - top);
    int last = first + rows;
    if (last > count) last = count;
    char path_line[1100], visible_path[1100], page[48];
    const char *directory = w->rompath[0] ? w->rompath : system ? system->rompath : "";
    snprintf(path_line, sizeof(path_line), "%s %s", strcmp(fe->settings.language, "pt.lang") == 0 ? "PASTA:" : "FOLDER:", directory);
    shorten(&r->small, path_line, 520, visible_path, sizeof(visible_path));
    text(r, &r->small, visible_path, 36, 87, r->material.on_surface, LEFT);
    snprintf(page, sizeof(page), "%d-%d / %d", first + 1, last, count);
    text(r, &r->small, page, 674, 87, r->material.on_surface, RIGHT);
    for (int row = 0; row < rows && first + row < count; ++row) {
        int index = first + row, y = top + row * (row_height + MATERIAL_LIST_GAP);
        const struct mainui_game *game = &fe->games[index];
        char detail[512], visible_label[512], visible_detail[512];
        snprintf(detail, sizeof(detail), "%s%s", game->folder ? (strcmp(fe->settings.language, "pt.lang") == 0 ? "Pasta  ·  " : "Folder  ·  ") : "",
                 game->name[0] ? game->name : mainui_catalog_game_label(game));
        shorten(&r->content, mainui_catalog_game_label(game), 515, visible_label, sizeof(visible_label));
        shorten(&r->small, detail, 515, visible_detail, sizeof(visible_detail));
        brickos_list_row(r, 35, y, 642, row_height, visible_label, visible_detail, index == w->focus, 1, 1);
        brickos_entry_icon(r, 48, y + (row_height - 42) / 2, game->folder);
        if (mainui_frontend_is_favorite(fe, MAINUI_LIST_ROMS, w->system, index))
            text(r, &r->small, "*", 659, y + 25, index == w->focus ? r->material.surface : (SDL_Color){128, 0, 0, 255}, CENTER);
    }
    fill(r, 695, 111, 282, 535, r->material.surface);
    brickos_bevel(r, 695, 111, 282, 535, 0);
    text(r, &r->small, strcmp(fe->settings.language, "pt.lang") == 0 ? "CAPA" : "COVER", 836, 119, r->material.on_surface, CENTER);
    const struct mainui_game *selected = w->focus >= 0 && w->focus < count ? &fe->games[w->focus] : NULL;
    char rompath[1024], imagepath[1024];
    rompath[0] = imagepath[0] = 0;
    if (selected && system && !selected->folder)
        mainui_catalog_game_paths(system, selected, rompath, sizeof(rompath), imagepath, sizeof(imagepath));
    struct mainui_render_image *art = selected && !selected->folder ? image(r, imagepath) : NULL;
    fill(r, 716, 147, 240, 365, r->material.surface_variant);
    brickos_bevel(r, 716, 147, 240, 365, 0);
    if (art) draw_fit(r, art, 726, 157, 220, 345);
    else {
        material_icon(r, MAINUI_TAB_GAMES, 803, 283);
        text(r, &r->small, strcmp(fe->settings.language, "pt.lang") == 0 ? "Sem capa" : "No cover",
             836, 376, r->material.on_surface, CENTER);
    }
    if (selected) {
        char item_info[320], visible_info[320];
        snprintf(item_info, sizeof(item_info), "%s  %d/%d", selected->folder ?
                 (strcmp(fe->settings.language, "pt.lang") == 0 ? "PASTA" : "FOLDER") : "ROM", w->focus + 1, count);
        shorten(&r->small, item_info, 246, visible_info, sizeof(visible_info));
        text_box(r, &r->small, visible_info, 712, 248, 529, r->material.on_surface);
        if (selected->name[0]) {
            char filename[320];
            shorten(&r->small, selected->name, 246, filename, sizeof(filename));
            text_box(r, &r->small, filename, 712, 248, 552, r->material.on_surface);
        }
    }
}

static void brickos_files(struct mainui_renderer *r, struct mainui_frontend *fe,
                         const struct mainui_window *w) {
    const struct mainui_file_folder *folder = mainui_frontend_file_folder(fe, w->system);
    if (!folder || folder->count <= 0) {
        brickos_empty_state(r, fe);
        return;
    }
    int row_height = material_list_row_height(r), top = 129;
    int rows = (MATERIAL_LIST_BOTTOM - top + MATERIAL_LIST_GAP) / (row_height + MATERIAL_LIST_GAP);
    int first = material_list_first(fe, w, folder->count, row_height, MATERIAL_LIST_BOTTOM - top);
    int last = first + rows;
    if (last > folder->count) last = folder->count;
    char path[1100], visible_path[1100], page[48];
    snprintf(path, sizeof(path), "%s %s", strcmp(fe->settings.language, "pt.lang") == 0 ? "LOCAL:" : "LOCATION:", folder->path);
    shorten(&r->small, path, 735, visible_path, sizeof(visible_path));
    text(r, &r->small, visible_path, 36, 87, r->material.on_surface, LEFT);
    snprintf(page, sizeof(page), "%d-%d / %d", first + 1, last, folder->count);
    text(r, &r->small, page, 984, 87, r->material.on_surface, RIGHT);
    for (int row = 0; row < rows && first + row < folder->count; ++row) {
        int index = first + row, y = top + row * (row_height + MATERIAL_LIST_GAP);
        const struct mainui_file *file = &folder->files[index];
        char visible_name[512], detail[96];
        shorten(&r->content, file->name, 830, visible_name, sizeof(visible_name));
        snprintf(detail, sizeof(detail), "%s", file->folder ?
                 (strcmp(fe->settings.language, "pt.lang") == 0 ? "Pasta" : "Folder") :
                 (strcmp(fe->settings.language, "pt.lang") == 0 ? "Ficheiro" : "File"));
        brickos_list_row(r, 35, y, 942, row_height, visible_name, detail, index == w->focus, 1, 1);
        brickos_entry_icon(r, 48, y + (row_height - 42) / 2, file->folder);
    }
}

// How much taller than at the Normal Text size a role's line is (0 at Normal and Small).
static int text_growth(const struct mainui_font *font, int normal_size) {
    int height = material_max_line_height(font);
    return font->size > normal_size ? height - height * normal_size / font->size : 0;
}

static void brickos_keyboard(struct mainui_renderer *r, struct mainui_frontend *fe,
                            const struct mainui_window *w) {
    int portuguese = strcmp(fe->settings.language, "pt.lang") == 0;
    // Larger Text sizes make the field taller, and the keys and buttons move down with it.
    int label_growth = text_growth(&r->small, 24), growth = label_growth + text_growth(&r->content, 36);
    const char *field = w->purpose == MAINUI_TYPE_SEARCH ? (portuguese ? "PESQUISA" : "SEARCH") :
                        w->purpose == MAINUI_TYPE_WIFI_PASSWORD || w->purpose == MAINUI_TYPE_HOTSPOT_PASSWORD ?
                        (portuguese ? "PALAVRA-PASSE" : "PASSWORD") : (portuguese ? "NOME" : "NAME");
    fill(r, 35, 94, 954, 68 + growth, r->material.surface);
    brickos_bevel(r, 35, 94, 954, 68 + growth, 0);
    text(r, &r->small, field, 52, 97, r->material.on_surface, LEFT);
    if (w->text[0]) {
        char visible[MAINUI_TEXT_MAX];
        shorten(&r->content, w->text, 914, visible, sizeof(visible));
        text(r, &r->content, visible, 52, 116 + label_growth, r->material.on_surface, LEFT);
    } else {
        const char *placeholder = w->purpose == MAINUI_TYPE_SEARCH ?
            (portuguese ? "Escreva para pesquisar" : "Type to search") : "";
        text(r, &r->small, placeholder, 52, 127 + label_growth, r->material.on_surface_variant, LEFT);
    }

    for (int key = 0; key < 40; ++key) {
        int x = 36 + (key % MAINUI_KEYBOARD_COLUMNS) * 96;
        int y = 175 + growth + (key / MAINUI_KEYBOARD_COLUMNS) * 80;
        int focused = key == w->focus;
        SDL_Color background = focused ? r->material.primary : r->material.surface_container;
        SDL_Color foreground = focused ? r->material.surface : r->material.on_surface;
        char label[2] = {mainui_keyboard_char(key, w->shift, w->symbols), 0};
        fill(r, x, y, 88, 72, background);
        brickos_bevel(r, x, y, 88, 72, !focused);
        if (focused) brickos_focus(r, x + 2, y + 2, 84, 68);
        text_box(r, &r->content, label, x + 4, 80, y + 17, foreground);
    }

    const char *bottom[5] = {
        w->shift ? "SHIFT ON / L1" : "SHIFT / L1",
        w->symbols ? "ABC / R1" : "123 / R1",
        portuguese ? "Espaço / X" : "Space / X",
        portuguese ? "Apagar / Y" : "Del / Y",
        w->purpose == MAINUI_TYPE_SEARCH ? (portuguese ? "Busca / START" : "Find / START") : "OK / START",
    };
    for (int button = 0; button < 5; ++button)
        brickos_button(r, 36 + button * 192, 505 + growth, 184, 72, bottom[button], w->focus == 40 + button, 1);
}

static void brickos_search(struct mainui_renderer *r, struct mainui_frontend *fe,
                          const struct mainui_window *w) {
    int row_height = material_list_row_height(r), top = 129;
    int rows = (MATERIAL_LIST_BOTTOM - top + MATERIAL_LIST_GAP) / (row_height + MATERIAL_LIST_GAP);
    int first = fe->hit_count ? material_list_first(fe, w, fe->hit_count, row_height, MATERIAL_LIST_BOTTOM - top) : 0;
    int last = first + rows;
    if (last > fe->hit_count) last = fe->hit_count;
    char query[MAINUI_TEXT_MAX + 256], visible_query[MAINUI_TEXT_MAX + 256], page[48];
    const struct mainui_system *scope = w->system >= 0 ? mainui_frontend_system(fe, w->system) : NULL;
    if (scope) snprintf(query, sizeof(query), "%s: %s   %s: %s",
                        strcmp(fe->settings.language, "pt.lang") == 0 ? "SISTEMA" : "SYSTEM",
                        scope->label, strcmp(fe->settings.language, "pt.lang") == 0 ? "PESQUISA" : "QUERY", w->text);
    else snprintf(query, sizeof(query), "%s: %s",
                  strcmp(fe->settings.language, "pt.lang") == 0 ? "PESQUISA" : "QUERY", w->text);
    shorten(&r->small, query, 735, visible_query, sizeof(visible_query));
    text(r, &r->small, visible_query, 36, 87, r->material.on_surface, LEFT);
    if (fe->hit_count) snprintf(page, sizeof(page), "%d-%d / %d", first + 1, last, fe->hit_count);
    else snprintf(page, sizeof(page), "0 / 0");
    text(r, &r->small, page, 984, 87, r->material.on_surface, RIGHT);
    if (!fe->hit_count) {
        brickos_empty_state(r, fe);
        return;
    }
    for (int row = 0; row < rows && first + row < fe->hit_count; ++row) {
        int index = first + row, y = top + row * (row_height + MATERIAL_LIST_GAP);
        const struct mainui_search_hit *hit = &fe->hits[index];
        const struct mainui_game *game = mainui_frontend_search_game(fe, index);
        if (!game) continue;
        const struct mainui_system *system = mainui_frontend_system(fe, hit->system);
        char detail[512], visible_label[512], visible_detail[512], ordinal[16];
        const char *filename = game->path[0] ? game->path : game->name;
        size_t detail_length = 0;
        if (system) {
            detail_length = strlen(system->label);
            if (detail_length >= sizeof(detail)) detail_length = sizeof(detail) - 1;
            memcpy(detail, system->label, detail_length);
        }
        if (filename[0] && detail_length + 1 < sizeof(detail)) {
            const char *separator = detail_length ? "  ·  " : "";
            size_t separator_length = strlen(separator);
            if (separator_length > sizeof(detail) - detail_length - 1)
                separator_length = sizeof(detail) - detail_length - 1;
            memcpy(detail + detail_length, separator, separator_length);
            detail_length += separator_length;
            size_t filename_length = strlen(filename);
            size_t room = sizeof(detail) - detail_length - 1;
            if (filename_length > room) {
                filename_length = room;
                while (filename_length &&
                       ((unsigned char)filename[filename_length] & 0xc0) == 0x80) --filename_length;
            }
            memcpy(detail + detail_length, filename, filename_length);
            detail_length += filename_length;
        }
        detail[detail_length] = 0;
        shorten(&r->content, mainui_catalog_game_label(game), 820, visible_label, sizeof(visible_label));
        shorten(&r->small, detail, 700, visible_detail, sizeof(visible_detail));
        snprintf(ordinal, sizeof(ordinal), "%04d", index + 1);
        brickos_list_row(r, 35, y, 942, row_height, visible_label, visible_detail,
                        index == w->focus, 1, 1);
        text(r, &r->small, ordinal, 920, y + (row_height - r->small.size) / 2,
             index == w->focus ? r->material.on_primary_container : r->material.on_surface_variant, RIGHT);
        brickos_entry_icon(r, 48, y + (row_height - 42) / 2, 0);
    }
}

static void brickos_upgrade_dialog(struct mainui_renderer *r, struct mainui_frontend *fe,
                                  const struct mainui_window *w) {
    int portuguese = strcmp(fe->settings.language, "pt.lang") == 0;
    fill(r, 19, 80, 986, 614, (SDL_Color){0, 0, 0, 96});
    fill(r, 222, 220, 574, 322, r->material.surface_container);
    brickos_bevel(r, 222, 220, 574, 322, 0);
    fill(r, 232, 230, 554, 46, r->material.primary);
    text(r, &r->content, portuguese ? "Atualização do sistema" : "System update", 250, 239, r->material.surface, LEFT);
    text(r, &r->content, portuguese ? "Atualizar o sistema?" : "Update system?", 509, 305, r->material.on_surface, CENTER);
    const char *base = strrchr(fe->upgrade_path, '/');
    base = base ? base + 1 : fe->upgrade_path;
    char visible[256];
    shorten(&r->small, base, 490, visible, sizeof(visible));
    brickos_field(r, 264, 348, 490, 54, portuguese ? "PACOTE" : "PACKAGE", visible);
    brickos_button(r, 325, 448, 164, 48, portuguese ? "Cancelar" : "Cancel",
                  w->focus == MAINUI_DIALOG_CANCEL, 1);
    brickos_button(r, 529, 448, 164, 48, portuguese ? "Atualizar" : "Update",
                  w->focus == MAINUI_DIALOG_OK, 1);
}

static const char *brickos_tab_label(const struct mainui_frontend *fe, enum mainui_tab tab) {
    return tab >= 0 && tab < MAINUI_TAB_COUNT ? tab_name(fe, tab) : "BrickOS";
}

// A desktop card's label: its tab's, or GameSwitcher's own name, the same in every language.
static const char *brickos_home_label(const struct mainui_frontend *fe, int item) {
    return item == MAINUI_HOME_GAMESWITCHER ? "GameSwitcher" : brickos_tab_label(fe, (enum mainui_tab)item);
}

static int brickos_tab_focus(const struct mainui_model *m, const int *tabs, int n) {
    for (int i = 0; i < n; ++i) if (tabs[i] == m->home_focus) return i;
    return n ? 0 : -1;
}

static void material_icon_color(struct mainui_renderer *r, SDL_Color color) {
    SDL_SetRenderDrawBlendMode(r->sdl, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(r->sdl, color.r, color.g, color.b, color.a);
}

static void material_icon_line(struct mainui_renderer *r, int x1, int y1, int x2, int y2,
                               SDL_Color color, int thickness) {
    material_icon_color(r, color);
    for (int i = 0; i < thickness; ++i)
        SDL_RenderDrawLine(r->sdl, x1, y1 + i, x2, y2 + i);
}

static void material_icon_circle(struct mainui_renderer *r, int cx, int cy, int radius,
                                 SDL_Color color, int filled) {
    material_icon_color(r, color);
    if (filled) {
        for (int dy = -radius; dy <= radius; ++dy) {
            int dx = (int)floor(sqrt((double)(radius * radius - dy * dy)));
            SDL_RenderDrawLine(r->sdl, cx - dx, cy + dy, cx + dx, cy + dy);
        }
        return;
    }
    int previous_x = cx + radius, previous_y = cy;
    for (int degree = 4; degree <= 360; degree += 4) {
        double radians = degree * 3.14159265358979323846 / 180.0;
        int next_x = cx + (int)lround(cos(radians) * radius);
        int next_y = cy + (int)lround(sin(radians) * radius);
        SDL_RenderDrawLine(r->sdl, previous_x, previous_y, next_x, next_y);
        previous_x = next_x;
        previous_y = next_y;
    }
}

static void material_icon(struct mainui_renderer *r, enum mainui_tab tab, int x, int y) {
    SDL_Color accent = r->material.primary;
    fill(r, x, y, 66, 66, r->material.surface_variant);
    switch (tab) {
    case MAINUI_TAB_FAVORITES: {
        SDL_Point heart[] = {{x + 33, y + 48}, {x + 17, y + 33}, {x + 15, y + 26}, {x + 18, y + 20},
                             {x + 24, y + 18}, {x + 30, y + 21}, {x + 33, y + 26}, {x + 36, y + 21},
                             {x + 42, y + 18}, {x + 48, y + 20}, {x + 51, y + 26}, {x + 49, y + 33},
                             {x + 33, y + 48}};
        material_icon_color(r, accent);
        SDL_RenderDrawLines(r->sdl, heart, (int)(sizeof(heart) / sizeof(heart[0])));
        break;
    }
    case MAINUI_TAB_RECENT:
        material_icon_circle(r, x + 33, y + 33, 20, accent, 0);
        material_icon_line(r, x + 33, y + 21, x + 33, y + 34, accent, 3);
        material_icon_line(r, x + 33, y + 33, x + 44, y + 39, accent, 3);
        break;
    case MAINUI_TAB_BESTS: {
        SDL_Point star[11];
        for (int i = 0; i < 10; ++i) {
            double angle = (-90.0 + i * 36.0) * 3.14159265358979323846 / 180.0;
            int radius = i % 2 == 0 ? 21 : 9;
            star[i] = (SDL_Point){x + 33 + (int)lround(cos(angle) * radius),
                                  y + 33 + (int)lround(sin(angle) * radius)};
        }
        star[10] = star[0];
        material_icon_color(r, accent);
        SDL_RenderDrawLines(r->sdl, star, 11);
        break;
    }
    case MAINUI_TAB_GAMES:
        material_icon_color(r, accent);
        rounded_fill(r->sdl, x + 12, y + 22, 42, 23, 11);
        material_icon_line(r, x + 21, y + 29, x + 21, y + 39, r->material.surface_variant, 3);
        material_icon_line(r, x + 16, y + 34, x + 26, y + 34, r->material.surface_variant, 3);
        material_icon_circle(r, x + 42, y + 31, 3, r->material.surface_variant, 1);
        material_icon_circle(r, x + 48, y + 37, 3, r->material.surface_variant, 1);
        break;
    case MAINUI_TAB_PORTS:
        outline(r, x + 13, y + 16, 40, 31, 3, accent);
        material_icon_line(r, x + 33, y + 47, x + 33, y + 53, accent, 3);
        material_icon_line(r, x + 23, y + 54, x + 43, y + 54, accent, 3);
        break;
    case MAINUI_TAB_APPS:
        for (int row = 0; row < 2; ++row) for (int column = 0; column < 2; ++column)
            fill(r, x + 17 + column * 18, y + 17 + row * 18, 14, 14, accent);
        break;
    case MAINUI_TAB_NETPLAY:
        material_icon_circle(r, x + 33, y + 33, 21, accent, 0);
        material_icon_line(r, x + 13, y + 33, x + 53, y + 33, accent, 2);
        material_icon_line(r, x + 16, y + 25, x + 50, y + 25, accent, 2);
        material_icon_line(r, x + 16, y + 41, x + 50, y + 41, accent, 2);
        material_icon_line(r, x + 33, y + 12, x + 33, y + 54, accent, 2);
        break;
    case MAINUI_TAB_SETTINGS:
        for (int row = 0; row < 3; ++row) {
            int cy = y + 20 + row * 13;
            material_icon_line(r, x + 16, cy, x + 50, cy, accent, 2);
            material_icon_circle(r, x + (row == 1 ? 39 : 26), cy + 1, 4, accent, 1);
        }
        break;
    default:
        material_icon_circle(r, x + 33, y + 33, 18, accent, 0);
        break;
    }
}

static void home_icon_fill(struct mainui_renderer *r, int x, int y, int w, int h,
                           int radius, SDL_Color color) {
    SDL_SetRenderDrawBlendMode(r->sdl, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(r->sdl, color.r, color.g, color.b, color.a);
    rounded_fill(r->sdl, x, y, w, h, radius);
}

static void home_icon_polygon(struct mainui_renderer *r, const SDL_Point *points, int count,
                              SDL_Color color) {
    if (!points || count < 3 || count > 32) return;
    int min_y = points[0].y, max_y = points[0].y;
    for (int i = 1; i < count; ++i) {
        if (points[i].y < min_y) min_y = points[i].y;
        if (points[i].y > max_y) max_y = points[i].y;
    }
    SDL_SetRenderDrawBlendMode(r->sdl, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(r->sdl, color.r, color.g, color.b, color.a);
    for (int y = min_y; y <= max_y; ++y) {
        double intersections[32];
        int n = 0;
        for (int i = 0; i < count; ++i) {
            SDL_Point a = points[i], b = points[(i + 1) % count];
            if ((a.y <= y && b.y > y) || (b.y <= y && a.y > y))
                intersections[n++] = a.x + (double)(y - a.y) * (b.x - a.x) / (b.y - a.y);
        }
        for (int i = 1; i < n; ++i) {
            double value = intersections[i];
            int j = i;
            while (j > 0 && intersections[j - 1] > value) {
                intersections[j] = intersections[j - 1];
                --j;
            }
            intersections[j] = value;
        }
        for (int i = 0; i + 1 < n; i += 2) {
            int left = (int)ceil(intersections[i]), right = (int)floor(intersections[i + 1]);
            if (right >= left) SDL_RenderDrawLine(r->sdl, left, y, right, y);
        }
    }
}

static SDL_Color home_card_surface(int tab, int focused) {
    SDL_Color color;
    switch (tab) {
    case MAINUI_HOME_GAMESWITCHER: color = focused ? (SDL_Color){150, 78, 30, 255} : (SDL_Color){92, 52, 32, 255}; break;
    case MAINUI_TAB_GAMES: color = focused ? (SDL_Color){38, 35, 188, 255} : (SDL_Color){24, 43, 91, 255}; break;
    case MAINUI_TAB_BESTS: color = focused ? (SDL_Color){44, 91, 127, 255} : (SDL_Color){33, 63, 83, 255}; break;
    case MAINUI_TAB_FAVORITES: color = focused ? (SDL_Color){130, 47, 64, 255} : (SDL_Color){82, 37, 52, 255}; break;
    case MAINUI_TAB_RECENT: color = focused ? (SDL_Color){23, 105, 68, 255} : (SDL_Color){24, 70, 54, 255}; break;
    case MAINUI_TAB_APPS: color = focused ? (SDL_Color){32, 66, 150, 255} : (SDL_Color){28, 53, 104, 255}; break;
    case MAINUI_TAB_PORTS: color = focused ? (SDL_Color){35, 79, 130, 255} : (SDL_Color){29, 57, 91, 255}; break;
    case MAINUI_TAB_NETPLAY: color = focused ? (SDL_Color){19, 104, 125, 255} : (SDL_Color){22, 66, 80, 255}; break;
    default: color = focused ? (SDL_Color){82, 59, 139, 255} : (SDL_Color){49, 43, 85, 255}; break;
    }
    return color;
}

static SDL_Color home_icon_color(int tab) {
    switch (tab) {
    case MAINUI_HOME_GAMESWITCHER: return (SDL_Color){255, 196, 132, 255};
    case MAINUI_TAB_GAMES: return (SDL_Color){205, 200, 255, 255};
    case MAINUI_TAB_BESTS: return (SDL_Color){180, 215, 240, 255};
    case MAINUI_TAB_FAVORITES: return (SDL_Color){255, 112, 124, 255};
    case MAINUI_TAB_RECENT: return (SDL_Color){127, 225, 167, 255};
    case MAINUI_TAB_APPS: return (SDL_Color){112, 201, 255, 255};
    case MAINUI_TAB_PORTS: return (SDL_Color){166, 202, 255, 255};
    case MAINUI_TAB_NETPLAY: return (SDL_Color){102, 225, 232, 255};
    default: return (SDL_Color){203, 188, 255, 255};
    }
}

// The home cards' focus: BrickOS's violet under the Violet accent, the accent's primary otherwise.
// The cards' own colours stay by destination.
static SDL_Color home_focus_color(const struct mainui_renderer *r) {
    if (mainui_appearance_get(look(r), MAINUI_SET_APPEAR_ACCENT) == MAINUI_ACCENT_VIOLET)
        return (SDL_Color){173, 131, 255, 255};
    return r->material.primary;
}

static void home_focus_outline(struct mainui_renderer *r, int x, int y, int w, int h, float emphasis) {
    material_focus_aura(r, x, y, w, h, home_focus_color(r), emphasis);
    SDL_Color glow = home_focus_color(r);
    if (focus_glows(r)) {
        glow.a = (Uint8)lroundf(34 * emphasis);
        outline(r, x - 5, y - 5, w + 10, h + 10, 5, glow);
        glow.a = (Uint8)lroundf(72 * emphasis);
        outline(r, x - 2, y - 2, w + 4, h + 4, 4, glow);
    }
    glow.a = (Uint8)lroundf(255 * emphasis);
    outline(r, x, y, w, h, 3, glow);
}

static void material_home_icon(struct mainui_renderer *r, int tab,
                               SDL_Color cutout, int x, int y, int w, int h) {
    static const char *const names[MAINUI_HOME_COUNT] = {
        [MAINUI_TAB_GAMES] = "Games", [MAINUI_TAB_BESTS] = "Bests", [MAINUI_TAB_FAVORITES] = "Favorites",
        [MAINUI_TAB_RECENT] = "Recent", [MAINUI_TAB_APPS] = "Apps", [MAINUI_TAB_PORTS] = "Ports",
        [MAINUI_TAB_NETPLAY] = "Netplay", [MAINUI_TAB_SETTINGS] = "Settings",
        [MAINUI_HOME_GAMESWITCHER] = "GameSwitcher"
    };
    struct mainui_home_card_cache *cache = &r->home_cards[tab];
    if (!cache->icon_attempted) {
        char path[PATH_BYTES];
        int length = snprintf(path, sizeof(path), "%s/mnt/SDCARD/BrickOS/icons/home/%s.png", r->fs->root, names[tab]);
        if (length >= 0 && length < (int)sizeof(path)) cache->icon = IMG_Load(path);
        cache->icon_attempted = 1;
    }
    if (cache->icon) {
        // Endpoint sprites are baked by a separate software renderer. Keep decoded pixels,
        // then create the texture on the renderer that owns this particular draw.
        SDL_Texture *texture = SDL_CreateTextureFromSurface(r->sdl, cache->icon);
        if (texture) {
            SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_BLEND);
            SDL_SetTextureScaleMode(texture, SDL_ScaleModeLinear);
            float scale = fminf((float)w / cache->icon->w, (float)h / cache->icon->h);
            int iw = (int)lroundf(cache->icon->w * scale), ih = (int)lroundf(cache->icon->h * scale);
            SDL_Rect dest = {x + (w - iw) / 2, y + (h - ih) / 2, iw, ih};
            SDL_RenderCopy(r->sdl, texture, NULL, &dest);
            SDL_DestroyTexture(texture);
            return;
        }
    }
    SDL_Color ink = home_icon_color(tab);
    int cx = x + w / 2, cy = y + h / 2;
    switch (tab) {
    case MAINUI_TAB_GAMES: {
        int radius = h / 4;
        home_icon_fill(r, x + w * 14 / 100, y + h * 29 / 100, w * 72 / 100, h * 48 / 100,
                       radius, ink);
        material_icon_circle(r, x + w * 24 / 100, y + h * 66 / 100, h * 13 / 100, ink, 1);
        material_icon_circle(r, x + w * 76 / 100, y + h * 66 / 100, h * 13 / 100, ink, 1);
        home_icon_fill(r, x + w * 20 / 100, y + h * 46 / 100, w * 5 / 100, h * 22 / 100, 2, cutout);
        home_icon_fill(r, x + w * 15 / 100, y + h * 54 / 100, w * 15 / 100, h * 6 / 100, 2, cutout);
        material_icon_circle(r, x + w * 68 / 100, y + h * 50 / 100, h * 4 / 100, cutout, 1);
        material_icon_circle(r, x + w * 78 / 100, y + h * 62 / 100, h * 4 / 100, cutout, 1);
        break;
    }
    case MAINUI_TAB_BESTS:
    case MAINUI_TAB_PORTS: {
        home_icon_fill(r, x + w * 14 / 100, y + h * 8 / 100, w * 72 / 100, h * 82 / 100, 11, ink);
        home_icon_fill(r, x + w * 29 / 100, y + h * 18 / 100, w * 42 / 100, h * 42 / 100, 4, cutout);
        home_icon_fill(r, x + w * 24 / 100, y + h * 66 / 100, w * 7 / 100, h * 7 / 100, 3, cutout);
        home_icon_fill(r, x + w * 35 / 100, y + h * 69 / 100, w * 5 / 100, h * 5 / 100, 3, cutout);
        material_icon_circle(r, x + w * 66 / 100, y + h * 70 / 100, h * 4 / 100, cutout, 1);
        material_icon_circle(r, x + w * 76 / 100, y + h * 64 / 100, h * 4 / 100, cutout, 1);
        break;
    }
    case MAINUI_TAB_FAVORITES: {
        SDL_Point heart[] = {
            {cx, y + h * 91 / 100}, {x + w * 13 / 100, y + h * 48 / 100},
            {x + w * 8 / 100, y + h * 36 / 100}, {x + w * 11 / 100, y + h * 22 / 100},
            {x + w * 22 / 100, y + h * 13 / 100}, {x + w * 34 / 100, y + h * 16 / 100},
            {x + w * 43 / 100, y + h * 28 / 100}, {cx, y + h * 38 / 100},
            {x + w * 57 / 100, y + h * 28 / 100}, {x + w * 66 / 100, y + h * 16 / 100},
            {x + w * 78 / 100, y + h * 13 / 100}, {x + w * 89 / 100, y + h * 22 / 100},
            {x + w * 92 / 100, y + h * 36 / 100}, {x + w * 87 / 100, y + h * 48 / 100},
        };
        home_icon_polygon(r, heart, (int)(sizeof(heart) / sizeof(heart[0])), ink);
        break;
    }
    case MAINUI_TAB_RECENT:
        material_icon_circle(r, cx, cy, h * 42 / 100, ink, 1);
        material_icon_circle(r, cx, cy, h * 42 / 100, cutout, 0);
        home_icon_fill(r, cx - 2, y + h * 25 / 100, 5, h * 29 / 100, 2, cutout);
        home_icon_fill(r, cx - 1, cy - 2, w * 22 / 100, 5, 2, cutout);
        break;
    case MAINUI_TAB_APPS:
        for (int row = 0; row < 2; ++row) for (int column = 0; column < 2; ++column)
            home_icon_fill(r, x + w * (19 + 35 * column) / 100,
                           y + h * (15 + 40 * row) / 100, w * 27 / 100, h * 30 / 100, 7, ink);
        break;
    case MAINUI_HOME_GAMESWITCHER: {
        // Two stacked game cards, the front one with a play mark: the recent games to switch to.
        home_icon_fill(r, x + w * 36 / 100, y + h * 10 / 100, w * 50 / 100, h * 58 / 100, 9, ink);
        home_icon_fill(r, x + w * 12 / 100, y + h * 28 / 100, w * 56 / 100, h * 64 / 100, 11, cutout);
        home_icon_fill(r, x + w * 16 / 100, y + h * 32 / 100, w * 48 / 100, h * 56 / 100, 9, ink);
        SDL_Point play[] = {
            {x + w * 33 / 100, y + h * 47 / 100}, {x + w * 51 / 100, y + h * 60 / 100},
            {x + w * 33 / 100, y + h * 73 / 100},
        };
        home_icon_polygon(r, play, (int)(sizeof(play) / sizeof(play[0])), cutout);
        break;
    }
    default:
        material_icon(r, (enum mainui_tab)tab, x + (w - 66) / 2, y + (h - 66) / 2);
        break;
    }
}

static SDL_Color home_mix_color(SDL_Color from, SDL_Color to, float amount) {
    return (SDL_Color){
        (Uint8)lroundf(from.r + (to.r - from.r) * amount),
        (Uint8)lroundf(from.g + (to.g - from.g) * amount),
        (Uint8)lroundf(from.b + (to.b - from.b) * amount),
        (Uint8)lroundf(from.a + (to.a - from.a) * amount)
    };
}

static void home_motion_update(struct mainui_renderer *r, const struct mainui_frontend *fe,
                                int count, int first) {
    struct mainui_home_motion *motion = &r->home_motion;
    // The carousel and the wallpaper cross-fade take 240 ms, half with Animations Fast; Off snaps.
    double speed = mainui_appearance_motion(look(r));
    if (!motion->valid || motion->count != count || !r->animations_enabled || speed <= 0) {
        motion->valid = 1;
        motion->active = 0;
        motion->count = count;
        motion->focus = fe->model.home_focus;
        motion->search = fe->model.home_search;
        motion->position = motion->first = first;
        for (int tab = 0; tab < MAINUI_HOME_COUNT; ++tab) {
            motion->emphasis[tab] = tab == motion->focus && !motion->search;
            motion->wallpaper[tab] = tab == motion->focus;
        }
        return;
    }
    if (motion->active) {
        float elapsed = r->frame_ms > motion->started_ms ? (float)(r->frame_ms - motion->started_ms) : 0;
        float progress = fminf(elapsed / (float)(240.0 * speed), 1.0f);
        float eased = progress * progress * (3.0f - 2.0f * progress);
        motion->position = motion->from_position + (motion->first - motion->from_position) * eased;
        for (int tab = 0; tab < MAINUI_HOME_COUNT; ++tab) {
            float emphasis = tab == motion->focus && !motion->search;
            float wallpaper = tab == motion->focus;
            motion->emphasis[tab] = motion->from_emphasis[tab] + (emphasis - motion->from_emphasis[tab]) * eased;
            motion->wallpaper[tab] = motion->from_wallpaper[tab] + (wallpaper - motion->from_wallpaper[tab]) * eased;
        }
        motion->active = progress < 1.0f;
    }
    if (motion->focus != fe->model.home_focus || motion->search != fe->model.home_search || motion->first != first) {
        // Retarget from the currently displayed state, including an interrupted crossfade.
        motion->from_position = motion->position;
        memcpy(motion->from_emphasis, motion->emphasis, sizeof(motion->emphasis));
        memcpy(motion->from_wallpaper, motion->wallpaper, sizeof(motion->wallpaper));
        motion->focus = fe->model.home_focus;
        motion->search = fe->model.home_search;
        motion->first = first;
        motion->started_ms = r->frame_ms;
        motion->active = 1;
    }
}

// A Home destination's wallpaper path; 0 for a destination without one.
static int home_tab_wallpaper(int tab, char *path, size_t size) {
    const char *filename;
    switch (tab) {
    case MAINUI_HOME_GAMESWITCHER: filename = "GameSwitcher.png"; break;
    case MAINUI_TAB_GAMES: filename = "Games.png"; break;
    case MAINUI_TAB_BESTS: filename = "Bests.png"; break;
    case MAINUI_TAB_FAVORITES: filename = "Favorites.png"; break;
    case MAINUI_TAB_RECENT: filename = "Recent.png"; break;
    case MAINUI_TAB_APPS: filename = "Apps.png"; break;
    case MAINUI_TAB_PORTS: filename = "Ports.png"; break;
    case MAINUI_TAB_NETPLAY: filename = "Netplay.png"; break;
    case MAINUI_TAB_SETTINGS: filename = "Settings.png"; break;
    default: return 0;
    }
    return snprintf(path, size, "/mnt/SDCARD/BrickOS/wallpapers/%s", filename) < (int)size;
}

static struct mainui_render_image *home_tab_art(struct mainui_renderer *r, int tab) {
    char path[PATH_BYTES];
    return home_tab_wallpaper(tab, path, sizeof(path)) ? image(r, path) : NULL;
}

static void home_wallpaper_layer(struct mainui_renderer *r, struct mainui_render_image *art, Uint8 alpha) {
    if (!art) {
        SDL_SetRenderDrawBlendMode(r->sdl, SDL_BLENDMODE_BLEND);
        SDL_SetRenderDrawColor(r->sdl, r->material.background.r, r->material.background.g,
                               r->material.background.b, alpha);
        SDL_Rect background = {0, 0, MAINUI_SCREEN_W, MAINUI_SCREEN_H};
        SDL_RenderFillRect(r->sdl, &background);
        return;
    }
    if (art && art->w > 0 && art->h > 0) {
        double target_ratio = (double)MAINUI_SCREEN_W / MAINUI_SCREEN_H;
        double source_ratio = (double)art->w / art->h;
        SDL_Rect source = {0, 0, art->w, art->h};
        if (source_ratio > target_ratio) {
            source.w = (int)(art->h * target_ratio);
            source.x = (art->w - source.w) / 2;
        } else if (source_ratio < target_ratio) {
            source.h = (int)(art->w / target_ratio);
            source.y = (art->h - source.h) / 2;
        }
        SDL_Rect dest = {0, 0, MAINUI_SCREEN_W, MAINUI_SCREEN_H};
        SDL_SetTextureBlendMode(art->texture, SDL_BLENDMODE_BLEND);
        SDL_SetTextureColorMod(art->texture, 255, 255, 255);
        SDL_SetTextureAlphaMod(art->texture, alpha);
        SDL_RenderCopy(r->sdl, art->texture, &source, &dest);
        SDL_SetTextureAlphaMod(art->texture, 255);
    }
}

// What the wallpaper layers drew: the tint they mix, how much they weigh, and how much of it
// was a picture.
struct wallpaper_mix { float tint_r, tint_g, tint_b, accumulated, pictured; };

static void destination_layers(struct mainui_renderer *r, int destination, int animate, struct wallpaper_mix *mix) {
    for (int tab = 0; tab < MAINUI_HOME_COUNT; ++tab) {
        float weight = animate ? r->home_motion.wallpaper[tab] : tab == destination;
        if (weight <= 0) continue;
        SDL_Color color = home_card_surface(tab, 1);
        mix->tint_r += color.r * weight;
        mix->tint_g += color.g * weight;
        mix->tint_b += color.b * weight;
        // Without a BrickOS wallpaper the layer is the BrickOS background, never the theme's bg.png.
        struct mainui_render_image *art = home_tab_art(r, tab);
        mix->accumulated += weight;
        if (art) mix->pictured += weight;
        // Normalize each overlay against the layers underneath, so rapid navigation
        // keeps the existing mixture rather than fading through a dark frame.
        home_wallpaper_layer(r, art, (Uint8)lroundf(255 * weight / mix->accumulated));
    }
}

static void wallpaper_dimming(struct mainui_renderer *r, const struct wallpaper_mix *mix) {
    float tint_r = mix->tint_r, tint_g = mix->tint_g, tint_b = mix->tint_b;
    // Preserve the selected artwork while keeping the title, tiles and footer readable, as
    // Wallpaper dimming chooses. The Black background has nothing to dim where no wallpaper is
    // drawn: it stays pure black there.
    struct mainui_dimming_values dimming = mainui_appearance_dimming(look(r));
    float cover = 1;
    if (mainui_appearance_background_level(look(r)) <= 0) cover = mix->accumulated > 0 ? mix->pictured / mix->accumulated : 0;
    SDL_SetRenderDrawBlendMode(r->sdl, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(r->sdl, 7, 9, 24, (Uint8)lroundf(dimming.shade * cover));
    SDL_Rect shade = {0, 0, MAINUI_SCREEN_W, MAINUI_SCREEN_H};
    if (dimming.shade) SDL_RenderFillRect(r->sdl, &shade);
    SDL_Color tint = {(Uint8)lroundf(tint_r), (Uint8)lroundf(tint_g), (Uint8)lroundf(tint_b),
                      (Uint8)lroundf(dimming.tint * cover)};
    SDL_SetRenderDrawColor(r->sdl, tint.r, tint.g, tint.b, tint.a);
    if (dimming.tint) SDL_RenderFillRect(r->sdl, &shade);
    for (int band = 0; band < 48; ++band) {
        int y = 384 + band * 8;
        Uint8 alpha = (Uint8)lroundf((dimming.band_base + band * dimming.band_step) * cover);
        SDL_SetRenderDrawColor(r->sdl, 5, 8, 22, alpha);
        SDL_Rect row = {0, y, MAINUI_SCREEN_W, 8};
        SDL_RenderFillRect(r->sdl, &row);
    }
}

static void draw_destination_wallpaper(struct mainui_renderer *r, int destination, int animate) {
    struct wallpaper_mix mix = {0};
    destination_layers(r, destination, animate, &mix);
    wallpaper_dimming(r, &mix);
}

// A system's or Bests collection's background (docs/material-you-frontend.md#system-backgrounds):
// BrickOS/wallpapers/systems/<folder>.png, else the background its config.json names; NULL when
// neither decodes, and the destination wallpaper shows.
static struct mainui_render_image *system_picture(struct mainui_renderer *r, const struct mainui_frontend *fe, int visible) {
    const struct mainui_system *s = visible >= 0 ? mainui_frontend_system(fe, visible) : NULL;
    if (!s) return NULL;
    char path[PATH_BYTES];
    if (s->folder[0] && snprintf(path, sizeof(path), "/mnt/SDCARD/BrickOS/wallpapers/systems/%s.png", s->folder) < (int)sizeof(path)) {
        struct mainui_render_image *own = image(r, path);
        if (own) return own;
    }
    return s->backdrop.background[0] == '/' ? image(r, s->backdrop.background) : NULL;
}

// Games or Bests at the root: the destination wallpaper, the system picture fading out, and the
// one fading in on the cards' clock, under one dimming. A side with no picture is the
// destination wallpaper, so a stock card shows Games.png as before.
static void draw_system_background(struct mainui_renderer *r, const struct mainui_frontend *fe, int destination) {
    struct wallpaper_mix mix = {0};
    destination_layers(r, destination, 0, &mix);
    float fade = r->animations_enabled ? mainui_frontend_background_fade(fe, r->frame_ms) : 1;
    struct mainui_render_image *from = fade < 1 ? system_picture(r, fe, fe->system_background.from) : NULL;
    struct mainui_render_image *shown = system_picture(r, fe, fe->system_background.shown);
    if (from) home_wallpaper_layer(r, from, 255);
    if (shown) home_wallpaper_layer(r, shown, (Uint8)lroundf(255 * fade));
    else if (from) home_wallpaper_layer(r, home_tab_art(r, destination), (Uint8)lroundf(255 * fade));
    if (from || shown) mix.pictured = mix.accumulated;
    wallpaper_dimming(r, &mix);
}

// The app bar's title for the window at a level of the stack.
static const char *brickos_level_title(struct mainui_frontend *fe, int level) {
    const struct mainui_window *w = &fe->model.stack[level];
    if (w->kind == MAINUI_WIN_HOME) return "";
    if (w->kind == MAINUI_WIN_TABS) return brickos_tab_label(fe, fe->model.tab);
    if (w->kind == MAINUI_WIN_ROMS && w->system >= 0) {
        if (w->rompath[0]) {
            const char *folder = strrchr(w->rompath, '/');
            if (folder && folder[1]) return folder + 1;
        }
        int bests = w->system >= MAINUI_BEST_SYSTEM_BASE;
        const struct mainui_system *s = mainui_frontend_system(fe, w->system);
        if (s) return bests ? tab_name(fe, MAINUI_TAB_BESTS) : s->label;
    }
    if (w->kind == MAINUI_WIN_FILES) return strcmp(fe->settings.language, "pt.lang") == 0 ? "Gestor de ficheiros" : "File Manager";
    if (w->kind == MAINUI_WIN_SETTINGS) {
        return w->screen >= 0 && w->screen < MAINUI_SCREEN_COUNT ? settings_title(fe, w) : tab_name(fe, MAINUI_TAB_SETTINGS);
    }
    if (w->kind == MAINUI_WIN_TEXT) {
        static char title[256];
        return ext_label(fe, w->index, title, sizeof(title));
    }
    if (w->kind == MAINUI_WIN_KEYBOARD) {
        if (w->purpose == MAINUI_TYPE_SEARCH) return say(fe, 134, "Search");
        if (w->purpose == MAINUI_TYPE_WIFI_PASSWORD) return say(fe, 111, "Wi-Fi");
        return say(fe, 114, "Hotspot");
    }
    if (w->kind == MAINUI_WIN_SEARCH) return strcmp(fe->settings.language, "pt.lang") == 0 ? "Resultados da pesquisa" : "Search Results";
    if (w->kind == MAINUI_WIN_EDIT) return say(fe, 168, "Edit");
    if (w->kind == MAINUI_WIN_DIALOG && w->dialog == MAINUI_DIALOG_UPGRADE)
        return strcmp(fe->settings.language, "pt.lang") == 0 ? "Atualização do sistema" : "System update";
    if (w->kind == MAINUI_WIN_DIALOG && w->dialog == MAINUI_DIALOG_CLEAR_RECENT) return say(fe, 77, "Clear List");
    if (w->kind == MAINUI_WIN_DIALOG && w->dialog == MAINUI_DIALOG_REMOVE_FAVORITE) return say(fe, 81, "Remove Favorite");
    if (w->kind == MAINUI_WIN_DIALOG && w->dialog == MAINUI_DIALOG_CLEAR_FAVORITE) return say(fe, 79, "Clear Favorite");
    // Other dialogs, popups, progress windows and notices keep the title of the screen under
    // them, in the language.
    if ((w->kind == MAINUI_WIN_DIALOG || w->kind == MAINUI_WIN_POPUP || w->kind == MAINUI_WIN_PROGRESS ||
         w->kind == MAINUI_WIN_NOTICE) && level > 0)
        return brickos_level_title(fe, level - 1);
    return "BrickOS";
}

static const char *brickos_window_title(struct mainui_frontend *fe) {
    return fe->model.depth > 0 ? brickos_level_title(fe, fe->model.depth - 1) : "BrickOS";
}

enum {
    HOME_CARD_WIDTH = 196, HOME_CARD_FOCUSED_WIDTH = 216, HOME_CARD_PITCH = 246,
    HOME_CARD_HEIGHT = 270, HOME_CARD_FOCUSED_HEIGHT = 310,
    HOME_CARD_TOP = 404, HOME_CARD_FOCUSED_TOP = 368
};

static void home_card_paint(struct mainui_renderer *r, struct mainui_frontend *fe, int tab,
                             int x, int y, int width, int height, float emphasis, int selected) {
    SDL_Color surface = home_mix_color(home_card_surface(tab, 0), home_card_surface(tab, 1), emphasis);
    fill(r, x, y, width, height, surface);
    outline(r, x + 1, y + 1, width - 2, height - 2, 1, (SDL_Color){223, 231, 255, (Uint8)lroundf(72 * (1 - emphasis))});
    if (emphasis > 0) home_focus_outline(r, x + 2, y + 2, width - 4, height - 4, emphasis);
    char visible_label[128], rest[128];
    const struct mainui_font *label = selected ? &r->content : &r->nav;
    // A single word too wide for the card, such as GameSwitcher, is drawn whole and narrowed to
    // fit rather than ending in "...". A name with spaces, or in more than one font (which cannot
    // be narrowed), takes two lines: the second goes under the first, and the icon shrinks by its
    // height.
    const char *name = brickos_home_label(fe, tab);
    int label_y = y + 168 + (int)lroundf(28 * emphasis), lines = 1;
    rest[0] = 0;
    if (text_width(label, name) > width - 18 && (strpbrk(name, " \n") || mixed_text(label, name)))
        lines = two_lines(label, name, width - 18, visible_label, rest, sizeof(visible_label));
    int icon_size = 112 + (int)lroundf(20 * emphasis) - (lines == 2 ? material_line_height(label, rest) : 0);
    material_home_icon(r, tab, surface, x + (width - icon_size) / 2, y + 40, icon_size, icon_size);
    SDL_Color white = {255, 255, 255, 255};
    if (lines == 2) {
        text(r, label, visible_label, x + width / 2, label_y, white, CENTER);
        text(r, label, rest, x + width / 2, label_y + material_line_height(label, visible_label), white, CENTER);
    } else if (text_width(label, name) <= width - 18 || !text_fit(r, label, name, x + width / 2, label_y, width - 18, white)) {
        shorten(label, name, width - 18, visible_label, sizeof(visible_label));
        text(r, label, visible_label, x + width / 2, label_y, white, CENTER);
    }
    if (emphasis > 0 && tab == MAINUI_TAB_GAMES)
        text(r, &r->small, language_is_portuguese(fe) ? "Jogar agora" : "Play now",
             x + width / 2, y + 247, (SDL_Color){218, 214, 255, (Uint8)lroundf(220 * emphasis)}, CENTER);
}

static SDL_Texture *home_card_texture(struct mainui_renderer *r, struct mainui_frontend *fe,
                                       int tab, int selected) {
    int width = selected ? HOME_CARD_FOCUSED_WIDTH : HOME_CARD_WIDTH;
    int height = selected ? HOME_CARD_FOCUSED_HEIGHT : HOME_CARD_HEIGHT;
    SDL_Surface *surface = SDL_CreateRGBSurfaceWithFormat(0, width + 16, height + 16, 32, SDL_PIXELFORMAT_ARGB8888);
    SDL_Renderer *software = surface ? SDL_CreateSoftwareRenderer(surface) : NULL;
    if (!software) { SDL_FreeSurface(surface); return NULL; }
    SDL_SetRenderDrawColor(software, 0, 0, 0, 0);
    SDL_RenderClear(software);
    SDL_Renderer *live = r->sdl;
    int suppressed = r->focus_pulse_suppressed;
    r->focus_pulse_suppressed = 1; // the cached endpoint must not bake an animation phase
    r->sdl = software;
    home_card_paint(r, fe, tab, 8, 8, width, height, (float)selected, selected);
    SDL_RenderPresent(software);
    r->sdl = live;
    r->focus_pulse_suppressed = suppressed;
    SDL_DestroyRenderer(software);
    // Software drawing on a transparent surface stores premultiplied edge colors.
    // Restore straight alpha before uploading to SDL's ordinary blended texture.
    for (int row = 0; row < surface->h; ++row) {
        Uint32 *pixels = (Uint32 *)((Uint8 *)surface->pixels + row * surface->pitch);
        for (int col = 0; col < surface->w; ++col) {
            Uint32 pixel = pixels[col], alpha = pixel >> 24;
            if (alpha > 0 && alpha < 255) {
                Uint32 red = ((pixel >> 16) & 255) * 255 / alpha;
                Uint32 green = ((pixel >> 8) & 255) * 255 / alpha;
                Uint32 blue = (pixel & 255) * 255 / alpha;
                pixels[col] = (alpha << 24) | ((red > 255 ? 255 : red) << 16) |
                              ((green > 255 ? 255 : green) << 8) | (blue > 255 ? 255 : blue);
            }
        }
    }
    SDL_Texture *texture = SDL_CreateTextureFromSurface(live, surface);
    SDL_FreeSurface(surface);
    if (texture) {
        SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_BLEND);
        SDL_SetTextureScaleMode(texture, SDL_ScaleModeLinear);
    }
    return texture;
}

static int home_card_prepare(struct mainui_renderer *r, struct mainui_frontend *fe, int tab) {
    struct mainui_home_card_cache *cache = &r->home_cards[tab];
    const char *label = brickos_home_label(fe, tab);
    if (!cache->attempted || strcmp(cache->label, label) != 0) {
        if (cache->normal) SDL_DestroyTexture(cache->normal);
        if (cache->focused) SDL_DestroyTexture(cache->focused);
        cache->normal = home_card_texture(r, fe, tab, 0);
        cache->focused = home_card_texture(r, fe, tab, 1);
        cache->attempted = 1;
        snprintf(cache->label, sizeof(cache->label), "%s", label);
    }
    return cache->normal && cache->focused;
}

static int home_card_cached(struct mainui_renderer *r, struct mainui_frontend *fe, int tab,
                             int x, int y, int width, int height, float emphasis, float opacity) {
    if (!home_card_prepare(r, fe, tab)) return 0;
    struct mainui_home_card_cache *cache = &r->home_cards[tab];
    SDL_Rect dest = {x - 8, y - 8, width + 16, height + 16};
    if (emphasis < 1) {
        SDL_SetTextureAlphaMod(cache->normal, (Uint8)lroundf(255 * opacity));
        SDL_RenderCopy(r->sdl, cache->normal, NULL, &dest);
    }
    if (emphasis > 0) {
        SDL_SetTextureAlphaMod(cache->focused, (Uint8)lroundf(255 * emphasis * opacity));
        SDL_RenderCopy(r->sdl, cache->focused, NULL, &dest);
    }
    material_focus_aura(r, x + 2, y + 2, width - 4, height - 4, home_focus_color(r), emphasis * opacity);
    return 1;
}

static void brickos_desktop(struct mainui_renderer *r, struct mainui_frontend *fe) {
    int tabs[MAINUI_HOME_COUNT];
    int n = mainui_model_home_items(&fe->model, tabs);
    int focus = brickos_tab_focus(&fe->model, tabs, n);
    if (n <= 0) return;
    int first = fe->model.home_first;
    if (first < 0) first = 0;
    if (first > n - MAINUI_MATERIAL_HOME_VISIBLE)
        first = n > MAINUI_MATERIAL_HOME_VISIBLE ? n - MAINUI_MATERIAL_HOME_VISIBLE : 0;
    int visible = n < MAINUI_MATERIAL_HOME_VISIBLE ? n : MAINUI_MATERIAL_HOME_VISIBLE;
    // A frame decodes only the wallpapers it shows and makes only the cards it draws, so a start
    // shows the Home at once; the others are prepared after it (mainui_render_prefetch).
    home_motion_update(r, fe, n, first);
    draw_destination_wallpaper(r, fe->model.home_focus, 1);
    int pitch = HOME_CARD_PITCH, card_width = HOME_CARD_WIDTH;
    int start_x = (MAINUI_SCREEN_W - ((visible - 1) * pitch + card_width)) / 2;
    int aura_padding = (HOME_CARD_FOCUSED_WIDTH - HOME_CARD_WIDTH) / 2 + FOCUS_AURA_MARGIN;
    SDL_Rect clip = {start_x - aura_padding, HOME_CARD_FOCUSED_TOP - FOCUS_AURA_MARGIN,
                     (visible - 1) * pitch + card_width + 2 * aura_padding,
                     HOME_CARD_FOCUSED_HEIGHT + 2 * FOCUS_AURA_MARGIN};
    SDL_RenderSetClipRect(r->sdl, &clip);
    int window_first = (int)lroundf(r->home_motion.position);
    if (window_first < 0) window_first = 0;
    if (window_first > n - visible) window_first = n - visible;
    float shift = r->home_motion.position - window_first;
    for (int i = window_first; i < window_first + visible; ++i) {
        float emphasis = r->home_motion.emphasis[tabs[i]];
        int selected = i == focus && !fe->model.home_search;
        int width = card_width + (int)lroundf((HOME_CARD_FOCUSED_WIDTH - HOME_CARD_WIDTH) * emphasis);
        int height = HOME_CARD_HEIGHT + (int)lroundf((HOME_CARD_FOCUSED_HEIGHT - HOME_CARD_HEIGHT) * emphasis);
        int x = start_x + (int)lroundf((i - r->home_motion.position) * pitch) - (width - card_width) / 2;
        int y = HOME_CARD_TOP + (int)lroundf((HOME_CARD_FOCUSED_TOP - HOME_CARD_TOP) * emphasis);
        if (x + width < clip.x || x >= clip.x + clip.w) continue;
        // Fade the outgoing/incoming edge slot around the window switch; three other
        // cards continue sliding, and no frame contains a fifth destination.
        float opacity = 1;
        if (shift > 0 && i == window_first) opacity = 1 - 2 * shift;
        if (shift < 0 && i == window_first + visible - 1) opacity = 1 + 2 * shift;
        if (!r->home_cache_enabled || !home_card_cached(r, fe, tabs[i], x, y, width, height, emphasis, opacity))
            home_card_paint(r, fe, tabs[i], x, y, width, height, emphasis, selected);
    }
    SDL_RenderSetClipRect(r->sdl, NULL);
    if (fe->model.home_search) {
        int sx = 42, sy = 132, sw = 302, sh = 50;
        fill(r, sx, sy, sw, sh, r->material.surface_variant);
        home_focus_outline(r, sx + 2, sy + 2, sw - 4, sh - 4, 1);
        material_icon_circle(r, sx + 20, sy + 24, 8, r->material.on_surface_variant, 0);
        material_icon_line(r, sx + 26, sy + 30, sx + 32, sy + 36, r->material.on_surface_variant, 2);
        text(r, &r->small, language_is_portuguese(fe) ? "Pesquisar jogos" : "Search games",
             sx + 46, sy + 12, r->material.on_surface, LEFT);
    }
}

static void brickos_settings_tiles(struct mainui_renderer *r, struct mainui_frontend *fe) {
    static const char *const icon_names[MAINUI_SETTINGS_TILES] = {
        "Display", "Volume", "WiFi", "Hotspot", "Bluetooth", "Themes", "Keymap", "Homepage", "System"
    };
    const struct mainui_tab_state *state = &fe->model.tabs[MAINUI_TAB_SETTINGS];
    int per_page = MAINUI_GRID_COLUMNS * MAINUI_GRID_ROWS;
    int first = state->focus / per_page * per_page;
    fe->model.tabs[MAINUI_TAB_SETTINGS].first = first;
    char page[48];
    int tiles = mainui_model_settings_tiles(&fe->model);
    snprintf(page, sizeof(page), "%s %d/%d", language_is_portuguese(fe) ? "PÁGINA" : "PAGE",
             state->focus / per_page + 1, (tiles + per_page - 1) / per_page);
    text(r, &r->small, page, 984, 88, r->material.on_surface, RIGHT);
    for (int index = first; index < tiles && index < first + per_page; ++index) {
        int position = index - first;
        int x = 48 + position % MAINUI_GRID_COLUMNS * 312;
        int y = 132 + position / MAINUI_GRID_COLUMNS * 270;
        int focused = index == state->focus;
        fill(r, x, y, 292, 244, focused ? r->material.primary_container : r->material.surface_container);
        brickos_bevel(r, x, y, 292, 244, !focused);
        if (focused) brickos_focus(r, x + 2, y + 2, 288, 240);
        char icon_path[PATH_BYTES], extension_title[256];
        int extension = index >= MAINUI_SETTINGS_TILES;
        // A settings extension's tile: its own icon, tinted like the others, else the generic one.
        const char *own = extension ? ext_tile_icon(fe, index - MAINUI_SETTINGS_TILES) : NULL;
        if (own) snprintf(icon_path, sizeof(icon_path), "%s", own);
        else snprintf(icon_path, sizeof(icon_path), "/mnt/SDCARD/BrickOS/icons/settings/%s.png",
                      extension ? "Extension" : icon_names[index]);
        if (extension) mainui_ext_tile_title(&fe->ext, index - MAINUI_SETTINGS_TILES, extension_title, sizeof(extension_title));
        const char *label = extension ? extension_title : say(fe, setting_labels[index], setting_names[index]);
        char visible[256], rest[256];
        const struct mainui_font *font = focused ? &r->focus_label : &r->content;
        SDL_Color ink = focused ? r->material.on_primary_container : r->material.on_surface;
        // One line at y + 160 under the 120 px icon. Two lines end 14 px above the tile's bottom,
        // using the room under one line first, and the icon is fitted between y + 34 and 6 px
        // above them.
        int lines = two_lines(font, label, 276, visible, rest, sizeof(visible));
        int first_h = material_line_height(font, visible), rest_h = lines == 2 ? material_line_height(font, rest) : 0;
        int label_y = y + 160, icon_size = 120;
        if (lines == 2 && y + 244 - 14 - first_h - rest_h < label_y) label_y = y + 244 - 14 - first_h - rest_h;
        if (label_y - 6 - (y + 34) < icon_size) icon_size = label_y - 6 - (y + 34);
        int icon_top = y + 34 + (label_y - 6 - (y + 34) - icon_size) / 2;
        struct mainui_render_image *icon = image_resized_style(r, icon_path, 120, 120, 1);
        if (!icon && own) icon = image_resized_style(r, "/mnt/SDCARD/BrickOS/icons/settings/Extension.png", 120, 120, 1);
        if (icon) {
            SDL_Color tint = focused ? r->material.on_primary_container : r->material.primary;
            SDL_SetTextureColorMod(icon->texture, tint.r, tint.g, tint.b);
            draw_fit(r, icon, x + 146 - icon_size / 2, icon_top, icon_size, icon_size);
            SDL_SetTextureColorMod(icon->texture, 255, 255, 255);
        } else material_icon(r, MAINUI_TAB_SETTINGS, x + 113, icon_top + (icon_size - 66) / 2);
        draw_text(r, font, visible, x + 146, label_y, first_h, ink, CENTER);
        if (lines == 2) draw_text(r, font, rest, x + 146, label_y + first_h, rest_h, ink, CENTER);
    }
}

static int material_font_height(const struct mainui_font *font) {
    if (font->font) return TTF_FontHeight(font->font);
    return font->size > 0 ? font->size : 24;
}

// Breaks each line wider than width at its last space that fits, as a larger Text size can make
// a message wider than its panel. A word wider than width stays whole.
static void wrap_lines(const struct mainui_font *font, char *message, int width) {
    for (char *line = message; line && *line;) {
        char *end = strchr(line, '\n'), *cut = NULL;
        if (end) *end = 0;
        if (text_width(font, line) > width) {
            for (char *p = strchr(line, ' '); p; p = strchr(p + 1, ' ')) {
                *p = 0;
                int fits = text_width(font, line) <= width;
                *p = ' ';
                if (!fits) break;
                cut = p;
            }
        }
        if (end) *end = '\n';
        if (cut) {
            *cut = '\n';
            line = cut + 1;
        } else {
            line = end ? end + 1 : NULL;
        }
    }
}

static void material_message_lines(struct mainui_renderer *r, const struct mainui_font *font,
                                   const char *message, int center_x, int top, int width, int height,
                                   SDL_Color color) {
    char copy[512];
    snprintf(copy, sizeof(copy), "%s", message ? message : "");
    wrap_lines(font, copy, width);
    int lines = 1;
    for (const char *p = copy; *p; ++p) lines += *p == '\n';
    int line_height = height / lines;
    int glyph_height = material_font_height(font);
    if (glyph_height + 4 > line_height) line_height = glyph_height + 4;
    char *line = copy;
    for (int i = 0; i < lines && line; ++i) {
        char *end = strchr(line, '\n');
        if (end) *end = 0;
        text(r, font, line, center_x, top + i * line_height + (line_height - glyph_height) / 2,
             color, CENTER);
        line = end ? end + 1 : NULL;
    }
}

static void material_scrim(struct mainui_renderer *r) {
    fill(r, 0, 0, MAINUI_SCREEN_W, MAINUI_SCREEN_H, (SDL_Color){3, 6, 14, 184});
}

static void material_panel(struct mainui_renderer *r, int x, int y, int w, int h) {
    fill(r, x + 5, y + 8, w, h, (SDL_Color){0, 0, 0, 96});
    fill(r, x, y, w, h, r->material.surface_container);
    outline(r, x, y, w, h, 2, r->material.outline);
}

static void material_popup_menu(struct mainui_renderer *r, struct mainui_frontend *fe,
                                const struct mainui_window *w) {
    material_scrim(r);
    if (w->item_count <= 0) return;
    int row_height = material_list_row_height(r);
    int header_height = material_max_line_height(&r->title) + 39;
    int rows = (MATERIAL_LIST_BOTTOM - 99 - header_height - 18 + MATERIAL_LIST_GAP) /
               (row_height + MATERIAL_LIST_GAP);
    if (rows < 1) rows = 1;
    if (rows > w->item_count) rows = w->item_count;
    int list_height = rows * (row_height + MATERIAL_LIST_GAP) - MATERIAL_LIST_GAP;
    int first = material_list_first(fe, w, w->item_count, row_height, list_height);
    int width = 648, height = header_height + list_height + 18;
    int x = (MAINUI_SCREEN_W - width) / 2, y = (MAINUI_SCREEN_H - height) / 2;
    material_panel(r, x, y, width, height);
    text(r, &r->title, language_is_portuguese(fe) ? "Menu rápido" : "Quick menu",
         x + 32, y + 23, r->material.on_surface, LEFT);
    for (int row = 0; row < rows && first + row < w->item_count; ++row) {
        int i = first + row;
        char buffer[64], visible[256];
        const char *label = popup_label(fe, w, w->items[i], buffer, sizeof(buffer));
        shorten(&r->content, label, width - 96, visible, sizeof(visible));
        brickos_list_row(r, x + 24, y + header_height + row * (row_height + MATERIAL_LIST_GAP),
                        width - 48, row_height, visible, "", i == w->focus, 1, 0);
    }
}

static void material_dialog_window(struct mainui_renderer *r, struct mainui_frontend *fe,
                                  const struct mainui_window *w) {
    static const struct { int title, message; const char *title_text, *message_text; } texts[] = {
        [MAINUI_DIALOG_SAVE_EDIT] = {93, 161, "Save", "Do you want to save modification?"},
        [MAINUI_DIALOG_CLEAR_RECENT] = {77, 78, "Clear List", "Clear recent game list,\nTF card content will\nnot be deleted"},
        [MAINUI_DIALOG_REMOVE_FAVORITE] = {81, 82, "Remove Favorite", "Remove game from this\nlist, TF card content will\nnot be deleted"},
        [MAINUI_DIALOG_CLEAR_FAVORITE] = {79, 80, "Clear Favorite", "Clear all game from this\nlist, TF card content will\nnot be deleted"},
        [MAINUI_DIALOG_DELETE_ROM] = {106, 107, "Delete ROM", "Delete ROM from TF Card?"},
        [MAINUI_DIALOG_FACTORY_RESET] = {67, 68, "Resume Default Setting", "Resume all system\nsettings.TF card content\nwill not be deleted"},
        [MAINUI_DIALOG_POWER_OFF] = {86, 86, "Power off", "Power off"},
        [MAINUI_DIALOG_FLASH_MODE] = {29, 146, "Flash Mode", "Reboot into Flash Mode"},
        [MAINUI_DIALOG_UPGRADE] = {56, 57, "Upgrade", "Update system?"},
        [MAINUI_DIALOG_REMOVE_DEVICE] = {52, 165, "Remove", "Remove device?"},
        [MAINUI_DIALOG_REFRESH_ROMS] = {54, 69, "Refresh Roms", "refresh roms on TF card"},
    };
    int extension = w->dialog == MAINUI_DIALOG_EXTENSION_CONFIRM, mapping = w->dialog == MAINUI_DIALOG_RESET_MAPPING;
    int appearance = w->dialog == MAINUI_DIALOG_RESET_APPEARANCE, portuguese = language_is_portuguese(fe);
    int power = w->dialog == MAINUI_DIALOG_POWER_OFF;
    if (!extension && !mapping && !appearance && (unsigned)w->dialog >= sizeof(texts) / sizeof(texts[0])) return;
    material_scrim(r);
    int x = 142, y = 175, width = 740, height = 420;
    material_panel(r, x, y, width, height);
    char extension_title[256], extension_message[256];
    const char *title = extension ? ext_label(fe, w->index, extension_title, sizeof(extension_title)) :
                        mapping ? mapping_text(fe, "Reset") :
                        appearance ? mainui_appearance_text("Reset appearance?", portuguese) :
                        power ? (portuguese ? "Energia" : "Power") :
                                  say(fe, texts[w->dialog].title, texts[w->dialog].title_text);
    const char *message = extension ? shorten(&r->popup_text, fe->ext_confirm, 680, extension_message, sizeof(extension_message)) :
                          mapping ? mapping_text(fe, "Remove all button mappings?") :
                          appearance ? mainui_appearance_text("Every option returns to its default.", portuguese) :
                          power ? (portuguese ? "Desligar ou reiniciar?" : "Power off or reboot?") :
                                    say(fe, texts[w->dialog].message, texts[w->dialog].message_text);
    text(r, &r->title, title, x + width / 2, y + 42, r->material.on_surface, CENTER);
    material_message_lines(r, &r->popup_text, message, x + width / 2, y + 134, width - 60, 132,
                           r->material.on_surface_variant);
    if (power) {
        // Power off, Reboot and Cancel: three equal buttons across the panel's button row.
        static const int order[3] = {MAINUI_DIALOG_OK, MAINUI_DIALOG_REBOOT, MAINUI_DIALOG_CANCEL};
        const char *labels[3] = {portuguese ? "Desligar" : say(fe, 86, "Power off"), portuguese ? "Reiniciar" : "Reboot",
                                 portuguese ? "Cancelar" : say(fe, 45, "Cancel")};
        for (int i = 0; i < 3; ++i)
            brickos_button(r, x + 40 + 228 * i, y + 310, 204, 64, labels[i], w->focus == order[i], 1);
        return;
    }
    brickos_button(r, x + 102, y + 310, 236, 64, say(fe, 45, "Cancel"),
                  w->focus == MAINUI_DIALOG_CANCEL, 1);
    brickos_button(r, x + 402, y + 310, 236, 64, say(fe, 46, "OK"),
                  w->focus == MAINUI_DIALOG_OK, 1);
}

static void material_progress_window(struct mainui_renderer *r, struct mainui_frontend *fe,
                                    const struct mainui_window *w) {
    static const struct { int index; const char *text; } messages[] = {
        [MAINUI_PROGRESS_REFRESH] = {53, "Refresh"},
        [MAINUI_PROGRESS_BLUETOOTH_ON] = {191, "Turning on bluetooth"},
        [MAINUI_PROGRESS_BLUETOOTH_OFF] = {190, "Turning off bluetooth"},
        [MAINUI_PROGRESS_WIFI] = {111, "WiFi"},
        [MAINUI_PROGRESS_WIFI_CONNECT] = {113, "Connect"},
        [MAINUI_PROGRESS_WIFI_FORGET] = {184, "Forget"},
        [MAINUI_PROGRESS_POWER_OFF] = {86, "Power off"},
        [MAINUI_PROGRESS_FLASH_MODE] = {29, "Flash Mode"},
    };
    int extension = w->progress == MAINUI_PROGRESS_EXTENSION, reboot = w->progress == MAINUI_PROGRESS_REBOOT;
    if (!extension && !reboot && (unsigned)w->progress >= sizeof(messages) / sizeof(messages[0])) return;
    material_scrim(r);
    material_panel(r, 232, 238, 560, 288);
    int cx = MAINUI_SCREEN_W / 2, cy = 330;
    unsigned active = (unsigned)(SDL_GetTicks() / 100 % 12);
    for (unsigned i = 0; i < 12; ++i) {
        double angle = i * 3.14159265358979323846 / 6.0;
        int px = cx + (int)lround(cos(angle) * 34), py = cy + (int)lround(sin(angle) * 34);
        SDL_Color color = r->material.primary;
        color.a = i == active ? 255 : (Uint8)(64 + (i * 16));
        material_icon_circle(r, px, py, i == active ? 5 : 3, color, 1);
    }
    if (extension) {
        // A settings extension's command: its label and its newest line; the footer offers Cancel.
        char label[256], visible[256];
        text(r, &r->content, shorten(&r->content, ext_label(fe, w->index, label, sizeof(label)), 520, visible, sizeof(visible)),
             cx, 400, r->material.on_surface, CENTER);
        text(r, &r->small, shorten(&r->small, fe->ext_line, 520, visible, sizeof(visible)),
             cx, 448, r->material.on_surface_variant, CENTER);
        return;
    }
    if (reboot) {
        text(r, &r->content, language_is_portuguese(fe) ? "A reiniciar..." : "Rebooting...", cx, 410, r->material.on_surface,
             CENTER);
        return;
    }
    text(r, &r->content, say(fe, messages[w->progress].index, messages[w->progress].text),
         cx, 410, r->material.on_surface, CENTER);
}

static void material_notice_window(struct mainui_renderer *r, struct mainui_frontend *fe,
                                  const struct mainui_window *w) {
    static const struct { int index; const char *text; } messages[] = {
        [MAINUI_NOTICE_DEVICE_REMOVED] = {164, "Device Removed"},
        [MAINUI_NOTICE_CONNECTION_FAILED] = {163, "Connection Failed"},
        [MAINUI_NOTICE_WIFI_FAILED] = {135, "Fail to connect\nCheck your password\nOr reboot router"},
        [MAINUI_NOTICE_UPGRADE_SPACE] = {58, "SD space low\n%dM left"},
        [MAINUI_NOTICE_BOOT_VISUAL_APPLIED] = {0, "Bitmap saved.\nReboot to apply."},
        [MAINUI_NOTICE_BOOT_VISUAL_FAILED] = {0, "Could not write bitmap.\nCurrent image kept."},
        [MAINUI_NOTICE_PREFERENCE_SAVE_FAILED] = {0, "Could not save preference.\nPrevious destination kept."},
        [MAINUI_NOTICE_LOADING_SCREEN_APPLIED] = {0, "Loading screen saved.\nIt shows the next time a game starts."},
        [MAINUI_NOTICE_LOADING_SCREEN_FAILED] = {0, "Could not write loading screen.\nPrevious images kept."},
        [MAINUI_NOTICE_MAPPING_SAVE_FAILED] = {0, MAPPING_SAVE_FAILED_TEXT},
        [MAINUI_NOTICE_APPEARANCE_SAVE_FAILED] = {0, "Appearance not saved"},
    };
    if ((unsigned)w->notice >= MAINUI_NOTICE_COUNT) return;
    char message[256];
    if (w->notice == MAINUI_NOTICE_EXTENSION_LINE) {
        material_scrim(r);
        material_panel(r, 152, 236, 720, 296);
        material_message_lines(r, &r->popup_text, shorten(&r->popup_text, fe->ext_notice, 660, message, sizeof(message)),
                               MAINUI_SCREEN_W / 2, 292, 660, 120, r->material.on_surface);
        brickos_button(r, 370, 442, 284, 60, say(fe, 46, "OK"), 1, 1);
        return;
    }
    const char *content = messages[w->notice].text;
    if (w->notice == MAINUI_NOTICE_BOOT_VISUAL_APPLIED)
        content = language_is_portuguese(fe) ? "Imagem guardada.\nReinicie para aplicar." : content;
    else if (w->notice == MAINUI_NOTICE_BOOT_VISUAL_FAILED)
        content = language_is_portuguese(fe) ? "Não foi possível gravar.\nA imagem atual foi mantida." : content;
    else if (w->notice == MAINUI_NOTICE_PREFERENCE_SAVE_FAILED)
        content = language_is_portuguese(fe) ? "Não foi possível guardar.\nO destino anterior foi mantido." : content;
    else if (w->notice == MAINUI_NOTICE_LOADING_SCREEN_APPLIED)
        content = language_is_portuguese(fe) ? "Ecrã de carregamento guardado.\nAparece no próximo jogo iniciado." : content;
    else if (w->notice == MAINUI_NOTICE_LOADING_SCREEN_FAILED)
        content = language_is_portuguese(fe) ? "Não foi possível gravar.\nAs imagens anteriores foram mantidas." : content;
    else if (w->notice == MAINUI_NOTICE_MAPPING_SAVE_FAILED)
        content = mapping_text(fe, content);
    else if (w->notice == MAINUI_NOTICE_APPEARANCE_SAVE_FAILED)
        content = mainui_appearance_text(content, language_is_portuguese(fe));
    if (w->notice == MAINUI_NOTICE_UPGRADE_SPACE)
        snprintf(message, sizeof(message), say(fe, 58, "SD space low\n%dM left"), w->index);
    else if (w->notice >= MAINUI_NOTICE_BOOT_VISUAL_APPLIED)
        snprintf(message, sizeof(message), "%s", content);
    else snprintf(message, sizeof(message), "%s", say(fe, messages[w->notice].index, content));
    material_scrim(r);
    material_panel(r, 152, 236, 720, 296);
    material_message_lines(r, &r->popup_text, message, MAINUI_SCREEN_W / 2, 292, 660, 120,
                           r->material.on_surface);
    brickos_button(r, 370, 442, 284, 60, say(fe, 46, "OK"), 1, 1);
}

static void material_edit_window(struct mainui_renderer *r, struct mainui_frontend *fe,
                                 const struct mainui_window *w) {
    int apps = w->system == MAINUI_TAB_APPS;
    int count = apps ? fe->app_count : fe->system_count;
    int first = (w->focus / 6) * 6;
    for (int slot = 0; slot < 6 && first + slot < count; ++slot) {
        int index = first + slot, x = 44 + (slot % 3) * 316, y = 116 + (slot / 3) * 268;
        int focused = index == w->focus;
        const char *label = apps ? fe->apps[index].label : fe->systems[index].label;
        const char *directory = apps ? fe->apps[index].directory : fe->systems[index].directory;
        const char *icon = apps ? fe->apps[index].icon : fe->systems[index].icontop;
        int shown = apps ? fe->apps[index].show : fe->systems[index].show;
        fill(r, x, y, 286, 224, focused ? r->material.primary_container : r->material.surface_container);
        brickos_bevel(r, x, y, 286, 224, 0);
        if (focused) brickos_focus(r, x + 2, y + 2, 282, 220);
        struct mainui_render_image *picture = NULL;
        if (icon[0]) {
            char path[PATH_BYTES];
            snprintf(path, sizeof(path), "%s/%s", directory, icon);
            picture = apps ? image(r, path) : system_icon(r, path);
        }
        if (picture) draw_fit(r, picture, x + 28, y + 24, 230, 126);
        else material_icon(r, apps ? MAINUI_TAB_APPS : MAINUI_TAB_GAMES, x + 110, y + 40);
        char visible[256];
        shorten(&r->content, label, 248, visible, sizeof(visible));
        text_box(r, &r->content, visible, x + 12, 262, y + 160,
                 focused ? r->material.on_primary_container : r->material.on_surface);
        material_icon_circle(r, x + 264, y + 22, 8, shown ? r->material.primary : r->material.outline, 1);
    }
}

static void brickos_chrome(struct mainui_renderer *r, struct mainui_frontend *fe) {
    SDL_SetRenderDrawBlendMode(r->sdl, SDL_BLENDMODE_NONE);
    for (int y = 0; y < MAINUI_SCREEN_H; ++y) {
        int amount = y * 90 / (MAINUI_SCREEN_H - 1);
        SDL_Color from = mainui_appearance_background_level(look(r)) > 0 ? r->material.surface_container
                                                                         : r->material.background;
        SDL_Color to = r->material.background;
        SDL_Color color = {
            (Uint8)((from.r * (255 - amount) + to.r * amount) / 255),
            (Uint8)((from.g * (255 - amount) + to.g * amount) / 255),
            (Uint8)((from.b * (255 - amount) + to.b * amount) / 255), 255};
        SDL_SetRenderDrawColor(r->sdl, color.r, color.g, color.b, color.a);
        SDL_RenderDrawLine(r->sdl, 0, y, MAINUI_SCREEN_W - 1, y);
    }
    SDL_SetRenderDrawBlendMode(r->sdl, SDL_BLENDMODE_BLEND);
    if (mainui_model_top(&fe->model)->kind != MAINUI_WIN_HOME) {
        // Nested lists, search, settings and modal overlays inherit the destination
        // at the root, including sessions that start directly in a catalog view.
        int destination = fe->model.stack[0].kind == MAINUI_WIN_HOME ? fe->model.home_focus : (int)fe->model.tab;
        if ((destination == MAINUI_TAB_GAMES || destination == MAINUI_TAB_BESTS) && fe->system_background.valid)
            draw_system_background(r, fe, destination);
        else draw_destination_wallpaper(r, destination, 0);
    }
}


// ---- BrickOS list views (docs/material-you-frontend.md#list-views) ---------------------------

// An entry of a game list, Recent or Favorites, as the Grid and Carousel views draw it.
struct view_entry {
    const char *label, *detail;
    char image[1024];
    int folder, favorite;
};

// The entries the window shows: the open game list's, or Recent's or Favorites'.
static int view_count(struct mainui_frontend *fe, const struct mainui_window *w) {
    if (w->kind == MAINUI_WIN_ROMS) {
        int n = mainui_frontend_roms(fe, w->system);
        return fe->games ? n : 0;
    }
    return fe->model.tab == MAINUI_TAB_RECENT ? fe->recent_count : fe->favorites_count;
}

static void view_entry(struct mainui_frontend *fe, const struct mainui_window *w, int index, struct view_entry *out) {
    memset(out, 0, sizeof(*out));
    out->label = out->detail = "";
    if (w->kind == MAINUI_WIN_ROMS) {
        const struct mainui_system *system = mainui_frontend_system(fe, w->system);
        const struct mainui_game *game = &fe->games[index];
        char rompath[1024];
        out->label = mainui_catalog_game_label(game);
        out->detail = game->name[0] ? game->name : out->label;
        out->folder = game->folder;
        out->favorite = mainui_frontend_is_favorite(fe, MAINUI_LIST_ROMS, w->system, index);
        if (system && !game->folder)
            mainui_catalog_game_paths(system, game, rompath, sizeof(rompath), out->image, sizeof(out->image));
        return;
    }
    const struct mainui_list_entry *entry = fe->model.tab == MAINUI_TAB_RECENT ? mainui_frontend_recent(fe, index) :
                                                                             mainui_frontend_favorite(fe, index);
    if (!entry) return;
    out->label = mainui_frontend_entry_label(fe, entry);
    out->detail = mainui_frontend_entry_system(fe, entry);
    if (!out->detail && entry->sublabel[0]) out->detail = entry->sublabel;
    if (!out->detail) out->detail = "";
    snprintf(out->image, sizeof(out->image), "%s", entry->imgpath);
}

// A cover, decoded at req_w x req_h with its aspect kept and drawn fitted in its box over the
// box's surface; without one, the folder or game symbol.
static void view_cover_box(struct mainui_renderer *r, const struct view_entry *entry, int x, int y, int w, int h,
                           int req_w, int req_h) {
    if (w <= 0 || h <= 0) return;
    fill(r, x, y, w, h, r->material.surface_variant);
    struct mainui_render_image *art = entry->folder ? NULL : view_cover(r, entry->image, req_w, req_h);
    if (art) draw_fit(r, art, x, y, w, h);
    else if (entry->folder) brickos_entry_icon(r, x + (w - 42) / 2, y + (h - 42) / 2, 1);
    else material_icon(r, MAINUI_TAB_GAMES, x + (w - 66) / 2, y + (h - 66) / 2);
}

// Grid: up to eight tiles a page, four columns by two rows, each with its cover, its title on up
// to two lines in the tile font, smaller than the rows', and, on Recent and Favorites, the system; game lists mark favorites. The page shows at the
// top right, as on the system grids, and the tiles start under it, at the same height on every list.
static void brickos_view_grid(struct mainui_renderer *r, struct mainui_frontend *fe, const struct mainui_window *w,
                              int focus, int n) {
    const int top = 120;
    int per_page = MAINUI_GAME_GRID_PAGE, page = focus / per_page, pages = (n + per_page - 1) / per_page;
    char label[48];
    snprintf(label, sizeof(label), "%s %d/%d", language_is_portuguese(fe) ? "PÁGINA" : "PAGE", page + 1, pages);
    text(r, &r->small, label, 984, 88, r->material.on_surface, RIGHT);
    int lists = w->kind != MAINUI_WIN_ROMS;
    for (int slot = 0; slot < per_page && page * per_page + slot < n; ++slot) {
        int index = page * per_page + slot, focused = index == focus;
        int x = 36 + slot % MAINUI_GAME_GRID_COLUMNS * 242, y = top + slot / MAINUI_GAME_GRID_COLUMNS * 274;
        int tw = 226, th = 262;
        struct view_entry entry;
        view_entry(fe, w, index, &entry);
        fill(r, x, y, tw, th, focused ? r->material.primary_container : r->material.surface_container);
        brickos_bevel(r, x, y, tw, th, !focused);
        if (focused) brickos_focus(r, x + 2, y + 2, tw - 4, th - 4);
        const struct mainui_font *title_font = &r->tile;
        char title[512], rest[512], detail[256];
        int lines = two_lines(title_font, entry.label, tw - 20, title, rest, sizeof(title));
        shorten(&r->small, lists ? entry.detail : "", tw - 20, detail, sizeof(detail));
        int title_h = material_line_height(title_font, title);
        int rest_h = lines == 2 ? material_line_height(title_font, rest) : 0;
        int detail_h = detail[0] ? material_line_height(&r->small, detail) : 0;
        int title_y = y + th - 10 - title_h - rest_h - (detail_h ? detail_h + 2 : 0);
        int box_h = title_y - 8 - (y + 10);
        view_cover_box(r, &entry, x + 10, y + 10, tw - 20, box_h, tw - 20, box_h);
        SDL_Color ink = focused ? r->material.on_primary_container : r->material.on_surface;
        draw_text(r, title_font, title, x + 10, title_y, title_h, ink, LEFT);
        if (rest_h) draw_text(r, title_font, rest, x + 10, title_y + title_h, rest_h, ink, LEFT);
        if (detail_h)
            draw_text(r, &r->small, detail, x + 10, title_y + title_h + rest_h + 2, detail_h,
                      focused ? r->material.on_primary_container : r->material.on_surface_variant, LEFT);
        if (entry.favorite) {
            material_icon_color(r, focused ? r->material.on_primary_container : r->material.primary);
            rounded_fill(r->sdl, x + tw - 30, y + 14, 16, 16, 8);
        }
    }
}

// A carousel cover d entries from the centre, a fraction while the covers slide: 300x380 at the
// centre and 200x260 one entry off, 330 pixels apart centre to centre, and 400 more for each
// entry further, off the screen. off is how far it is from the centre, 0 to 1.
struct view_slot { int x, y, w, h; float off; };
static struct view_slot view_slot(float d) {
    float off = fminf(fabsf(d), 1.f);
    int w = (int)lroundf(300 - 100 * off), h = (int)lroundf(380 - 120 * off);
    float reach = fabsf(d) <= 1 ? 330 * d : (d > 0 ? 1.f : -1.f) * (330 + 400 * (fabsf(d) - 1));
    int cx = (int)lroundf(512 + reach);
    return (struct view_slot){cx - w / 2, 320 - h / 2, w, h, off};
}

// How far the window's carousel still stands off its places: the frontend's slide, 0 at rest and
// in static captures.
static float view_shift(const struct mainui_renderer *r, const struct mainui_frontend *fe, const struct mainui_window *w) {
    if (!r->animations_enabled || !fe->carousel.valid || fe->carousel.kind != (int)w->kind) return 0;
    return mainui_frontend_carousel_shift(fe, r->frame_ms);
}

// Carousel: the focused cover large at the centre and its neighbours smaller at its sides, each
// title under its cover on up to two lines, and under the centre its details: the file name and n/count in a game
// list, the system on Recent and Favorites. With two entries the other sits on the side the last
// step left it on; with one, only the centre shows. While the covers slide, each cover's place,
// size and title colour follow its distance from the centre.
static void brickos_view_carousel(struct mainui_renderer *r, struct mainui_frontend *fe, const struct mainui_window *w,
                                  int focus, int n) {
    float shift = view_shift(r, fe, w);
    int offsets[7], count = 0;
    if (n == 2) {
        offsets[count++] = fe->carousel.step > 0 ? -1 : 1;
    } else if (n >= 3) {
        for (int k = -3; k <= 3; ++k) if (k) offsets[count++] = k;
    }
    offsets[count++] = 0;
    // The far covers first, so that the nearer ones overlap them; the centre last.
    for (int i = 0; i < count; ++i)
        for (int j = i + 1; j < count; ++j)
            if (fabsf(offsets[j] + shift) > fabsf(offsets[i] + shift)) {
                int t = offsets[i]; offsets[i] = offsets[j]; offsets[j] = t;
            }
    for (int i = 0; i < count; ++i) {
        float d = offsets[i] + shift;
        if (fabsf(d) > 2.5f) continue;
        int index = ((focus + offsets[i]) % n + n) % n;
        struct view_slot s = view_slot(d);
        struct view_entry entry;
        view_entry(fe, w, index, &entry);
        float emphasis = 1 - s.off;
        fill(r, s.x, s.y, s.w, s.h, mix_color(r->material.surface_container, r->material.primary_container, emphasis));
        brickos_bevel(r, s.x, s.y, s.w, s.h, emphasis < 0.5f);
        view_cover_box(r, &entry, s.x + 6, s.y + 6, s.w - 12, s.h - 12, 288, 368);
        if (fabsf(d) < 0.5f) brickos_focus(r, s.x + 2, s.y + 2, s.w - 4, s.h - 4);
        if (fabsf(d) >= 1.5f) continue;
        char title[512], rest[512];
        const struct mainui_font *title_font = fabsf(d) < 0.5f ? &r->focus_label : &r->content;
        int lines = two_lines(title_font, entry.label, s.w + 40, title, rest, sizeof(title));
        int title_y = s.y + s.h + 10, title_h = material_line_height(title_font, title);
        SDL_Color title_ink = mix_color(r->material.on_surface_variant, r->material.on_surface, emphasis);
        text_box(r, title_font, title, s.x - 20, s.w + 40, title_y, title_ink);
        if (lines == 2) {
            text_box(r, title_font, rest, s.x - 20, s.w + 40, title_y + title_h, title_ink);
            title_h += material_line_height(title_font, rest);
        }
        if (fabsf(d) >= 0.5f) continue;
        char detail[512], visible[512];
        if (w->kind == MAINUI_WIN_ROMS) snprintf(detail, sizeof(detail), "%s  %d/%d", entry.detail, index + 1, n);
        else snprintf(detail, sizeof(detail), "%s", entry.detail);
        shorten(&r->small, detail, 600, visible, sizeof(visible));
        if (visible[0]) text_box(r, &r->small, visible, 212, 600, title_y + title_h + 4, r->material.on_surface_variant);
    }
}

// A game list in the Grid or Carousel view, under its folder line.
static void brickos_rom_view(struct mainui_renderer *r, struct mainui_frontend *fe, const struct mainui_window *w, int view) {
    const struct mainui_system *system = mainui_frontend_system(fe, w->system);
    int n = view_count(fe, w);
    if (n <= 0) {
        brickos_empty_state(r, fe);
        return;
    }
    char path_line[1100], visible_path[1100];
    const char *directory = w->rompath[0] ? w->rompath : system ? system->rompath : "";
    snprintf(path_line, sizeof(path_line), "%s %s", language_is_portuguese(fe) ? "PASTA:" : "FOLDER:", directory);
    shorten(&r->small, path_line, view == MAINUI_VIEW_GRID ? 760 : 940, visible_path, sizeof(visible_path));
    text(r, &r->small, visible_path, 36, 87, r->material.on_surface, LEFT);
    int focus = w->focus < n ? w->focus : n - 1;
    if (view == MAINUI_VIEW_GRID) brickos_view_grid(r, fe, w, focus, n);
    else brickos_view_carousel(r, fe, w, focus, n);
}

// Recent or Favorites in the Grid or Carousel view.
static void brickos_tab_view(struct mainui_renderer *r, struct mainui_frontend *fe, const struct mainui_window *w, int view) {
    int n = view_count(fe, w);
    if (n <= 0) {
        brickos_empty_state(r, fe);
        return;
    }
    int focus = fe->model.tabs[fe->model.tab].focus;
    if (focus >= n) focus = n - 1;
    if (view == MAINUI_VIEW_GRID) brickos_view_grid(r, fe, w, focus, n);
    else brickos_view_carousel(r, fe, w, focus, n);
}

static void brickos_draw_content(struct mainui_renderer *r, struct mainui_frontend *fe, int level) {
    if (level < 0 || level >= fe->model.depth) return;
    const struct mainui_window *w = &fe->model.stack[level];
    if ((w->kind == MAINUI_WIN_POPUP || w->kind == MAINUI_WIN_DIALOG ||
         w->kind == MAINUI_WIN_PROGRESS || w->kind == MAINUI_WIN_NOTICE) && level > 0)
        brickos_draw_content(r, fe, level - 1);
    int suppressed = r->focus_pulse_suppressed;
    r->focus_pulse_suppressed = level != fe->model.depth - 1;
    switch (w->kind) {
    case MAINUI_WIN_HOME: brickos_desktop(r, fe); break;
    case MAINUI_WIN_TABS:
        if (mainui_model_view(&fe->model, w) != MAINUI_VIEW_LIST) brickos_tab_view(r, fe, w, mainui_model_view(&fe->model, w));
        else if (w->kind == MAINUI_WIN_TABS &&
            (fe->model.tab == MAINUI_TAB_RECENT || fe->model.tab == MAINUI_TAB_FAVORITES ||
             fe->model.tab == MAINUI_TAB_NETPLAY)) brickos_tab_list(r, fe);
        else if (brickos_catalog_tab(fe->model.tab)) brickos_catalog_grid(r, fe);
        else if (fe->model.tab == MAINUI_TAB_SETTINGS) brickos_settings_tiles(r, fe);
        else draw_window(r, fe, level);
        break;
    case MAINUI_WIN_ROMS:
        if (mainui_model_view(&fe->model, w) != MAINUI_VIEW_LIST) brickos_rom_view(r, fe, w, mainui_model_view(&fe->model, w));
        else brickos_roms(r, fe, w);
        break;
    case MAINUI_WIN_FILES: brickos_files(r, fe, w); break;
    case MAINUI_WIN_SETTINGS: brickos_settings_window(r, fe, w); break;
    case MAINUI_WIN_KEYBOARD: brickos_keyboard(r, fe, w); break;
    case MAINUI_WIN_SEARCH: brickos_search(r, fe, w); break;
    case MAINUI_WIN_EDIT: material_edit_window(r, fe, w); break;
    case MAINUI_WIN_POPUP: material_popup_menu(r, fe, w); break;
    case MAINUI_WIN_DIALOG:
        if (w->dialog == MAINUI_DIALOG_UPGRADE) brickos_upgrade_dialog(r, fe, w);
        else material_dialog_window(r, fe, w);
        break;
    case MAINUI_WIN_PROGRESS: material_progress_window(r, fe, w); break;
    case MAINUI_WIN_NOTICE: material_notice_window(r, fe, w); break;
    case MAINUI_WIN_TEXT: extension_text_window(r, fe, w, 1); break;
    default: draw_window(r, fe, level); break;
    }
    r->focus_pulse_suppressed = suppressed;
}

static void brickos_render_frame(struct mainui_renderer *r, struct mainui_frontend *fe) {
    brickos_chrome(r, fe);
    brickos_draw_content(r, fe, fe->model.depth - 1);
    // Draw the app bar and action hints above modal dimming, as a single modern app surface.
    struct mainui_font title = r->content;
    title.shift = 0;
    char visible_title[512];
    const struct mainui_window *top = mainui_model_top(&fe->model);
    if (top->kind != MAINUI_WIN_HOME) {
        shorten_line(&title, brickos_window_title(fe), 760, visible_title, sizeof(visible_title));
        text(r, &title, visible_title, 38, 29, r->material.on_surface, LEFT);
    }
    brickos_status(r, fe);
    brickos_footer(r, fe);
}

// Stock++ moves while the one-row cards slide or a background fades in.
static int stock_motion(const struct mainui_renderer *r, const struct mainui_frontend *fe, uint64_t now_ms) {
    if (r->presentation != MAINUI_PRESENTATION_STOCK) return 0;
    if (r->animations_enabled && mainui_frontend_carousel_shift(fe, now_ms) != 0) return 1;
    return mainui_frontend_backdrop_alpha(fe, now_ms) < 255 && strcmp(fe->backdrop.from, fe->backdrop.path) != 0;
}

int mainui_render_animating(const struct mainui_renderer *r, const struct mainui_frontend *fe) {
    if (r->animations_enabled && r->presentation == MAINUI_PRESENTATION_STOCK) return r->stock_motion_drawn;
    if (r->animations_enabled && r->presentation == MAINUI_PRESENTATION_MATERIAL_YOU && r->brickos_motion_drawn) return 1;
    return r->animations_enabled && r->presentation == MAINUI_PRESENTATION_MATERIAL_YOU &&
           mainui_model_top(&fe->model)->kind == MAINUI_WIN_HOME && r->home_motion.active;
}

// The loading-screen preview plays its frames at 10 per second while a preset has the focus.
static int loading_preview_due(const struct mainui_renderer *r, const struct mainui_frontend *fe, uint64_t now_ms) {
    const struct mainui_window *w = mainui_model_top(&fe->model);
    return w->kind == MAINUI_WIN_SETTINGS && w->screen == MAINUI_SCREEN_LOADING_SCREEN && w->focus > 0 &&
           w->focus <= fe->loading_screen_count && now_ms >= r->frame_ms && now_ms - r->frame_ms >= 100;
}

int mainui_render_frame_due(const struct mainui_renderer *r, const struct mainui_frontend *fe, uint64_t now_ms) {
    if (loading_preview_due(r, fe, now_ms)) return 1;
    // A slide or a background fade draws a frame each turn until it ends, and one more at rest
    // when the last frame caught it moving.
    if (r->presentation == MAINUI_PRESENTATION_STOCK && (stock_motion(r, fe, now_ms) || r->stock_motion_drawn)) return 1;
    // BrickOS: a carousel slide, the frame at rest after one, and covers still to decode.
    if (r->presentation == MAINUI_PRESENTATION_MATERIAL_YOU &&
        (r->covers_pending || r->brickos_motion_drawn ||
         (r->animations_enabled && (mainui_frontend_carousel_shift(fe, now_ms) != 0 || mainui_frontend_background_fade(fe, now_ms) < 1))))
        return 1;
    return mainui_render_animating(r, fe) ||
           (r->animations_enabled && r->presentation == MAINUI_PRESENTATION_MATERIAL_YOU &&
            r->focus_pulse_visible && now_ms >= r->frame_ms && now_ms - r->frame_ms >= 33);
}

void mainui_render_frame(struct mainui_renderer *r, struct mainui_frontend *fe) {
    mainui_render_frame_at(r, fe, SDL_GetTicks());
}

uint64_t mainui_render_decode_ms(const struct mainui_renderer *r) {
    Uint64 frequency = SDL_GetPerformanceFrequency();
    return frequency ? (uint64_t)(r->decode_ticks * 1000 / frequency) : 0;
}

void mainui_render_frame_at(struct mainui_renderer *r, struct mainui_frontend *fe, uint64_t now_ms) {
    r->frame_ms = now_ms;
    r->decode_ticks = 0;
    r->focus_pulse_visible = 0;
    r->covers_left = MAINUI_COVERS_PER_FRAME;
    r->covers_pending = 0;
    int loading_selector = 0;
    for (int i = 0; i < fe->model.depth; ++i)
        loading_selector |= fe->model.stack[i].kind == MAINUI_WIN_SETTINGS &&
                            fe->model.stack[i].screen == MAINUI_SCREEN_LOADING_SCREEN;
    if (!loading_selector) loading_preview_clear(r);
    int font_list = 0;
    for (int i = 0; i < fe->model.depth; ++i)
        font_list |= fe->model.stack[i].kind == MAINUI_WIN_SETTINGS && (fe->model.stack[i].screen == MAINUI_SCREEN_APPEARANCE_FONT ||
                                                                        fe->model.stack[i].screen == MAINUI_SCREEN_LANGUAGE);
    if (!font_list) font_samples_clear(r);
    if (r->presentation != MAINUI_PRESENTATION_MATERIAL_YOU || mainui_model_top(&fe->model)->kind != MAINUI_WIN_HOME) {
        r->home_motion.valid = 0;
        r->home_motion.active = 0;
    }
    // BrickOS draws with the Appearance choices at once; the cached home cards hold the last ones.
    r->appearance = &fe->appearance;
    if (memcmp(r->appearance_drawn, fe->appearance.values, sizeof(r->appearance_drawn)) != 0) {
        home_cache_clear(r);
        memcpy(r->appearance_drawn, fe->appearance.values, sizeof(r->appearance_drawn));
    }
    // The theme and language can change from their screens: skins, fonts, sizes, and colors
    // follow at once. BrickOS follows the Default theme, so a frontend switch reloads them too,
    // and so does a change of its Appearance font or Text size.
    const char *look = mainui_frontend_look_theme(fe);
    int brickos = fe->model.presentation == MAINUI_PRESENTATION_MATERIAL_YOU;
    char chosen[sizeof(r->font_file)] = "";
    if (brickos && fe->appearance.font[0]) snprintf(chosen, sizeof(chosen), "%s/%s", MAINUI_FONTS_DIR, fe->appearance.font);
    int text_size = brickos ? mainui_appearance_get(&fe->appearance, MAINUI_SET_APPEAR_TEXT_SIZE) : MAINUI_TEXT_NORMAL;
    if (strcmp(r->theme, look) != 0 || strcmp(r->language, fe->settings.language) != 0 ||
        strcmp(r->font_file, chosen) != 0 || r->text_size != text_size) {
        home_cache_clear(r);
        const char *font = "";
        for (int i = 0; i < r->cache_count; ++i)
            if (r->cache[i].texture) SDL_DestroyTexture(r->cache[i].texture);
        r->cache_count = 0;
        snprintf(r->theme, sizeof(r->theme), "%s", look);
        snprintf(r->language, sizeof(r->language), "%s", fe->settings.language);
        for (int i = 0; i < fe->theme_count; ++i)
            if (strcmp(fe->themes[i].path, look) == 0) font = fe->themes[i].font;
        snprintf(r->font_file, sizeof(r->font_file), "%s", chosen);
        r->text_size = text_size;
        // A chosen font that does not open draws as Default.
        double scale = brickos ? mainui_appearance_text_scale(&fe->appearance) : 1;
        if (load_theme(r, font, chosen, scale) < 0 && chosen[0]) load_theme(r, font, "", scale);
    }
    // Browsing box art fills the cache; it is emptied between frames once it passes the
    // headroom a frame needs, so no image a frame holds goes away while it draws.
    if (r->cache_count > MAINUI_RENDER_CACHE - MAINUI_RENDER_FRAME_IMAGES) {
        for (int i = 0; i < r->cache_count; ++i)
            if (r->cache[i].texture) SDL_DestroyTexture(r->cache[i].texture);
        r->cache_count = 0;
    }
    SDL_SetRenderDrawColor(r->sdl, 0, 0, 0, 255);
    SDL_RenderClear(r->sdl);
    if (r->presentation == MAINUI_PRESENTATION_MATERIAL_YOU) {
        r->brickos_motion_drawn = r->animations_enabled &&
                                  (mainui_frontend_carousel_shift(fe, now_ms) != 0 || mainui_frontend_background_fade(fe, now_ms) < 1);
        material_palette_update(r);
        brickos_render_frame(r, fe);
        return;
    }
    r->stock_motion_drawn = stock_motion(r, fe, now_ms);
    background(r, fe, now_ms);
    // A popup or dialog draws over the window below it.
    draw_window(r, fe, fe->model.depth - 1);
}

// ---- Pictures decoded ahead -----------------------------------------------------------------
// A picture decoded for the first time holds the frame that needs it (about 330 ms for a large
// system background on the Brick). While no slide or fade runs, a worker thread decodes the ones
// the next step may need, so the step finds them held: the system backgrounds of the Games or
// Bests focus's neighbours, the covers a Carousel step brings in, nearest the edges first, and
// the Home's other wallpapers, nearest the focus first. The Home's first frame waits for none of
// them: with every wallpaper decoded first, a return to the Home was black for two seconds.

static void *prefetch_worker(void *context) {
    struct mainui_render_prefetch *p = context;
    pthread_mutex_lock(&p->lock);
    while (!p->stop) {
        if (!p->queued || p->done) {
            pthread_cond_wait(&p->wake, &p->lock);
            continue;
        }
        char full[sizeof(p->full)];
        int w = p->w, h = p->h, style = p->style;
        memcpy(full, p->full, sizeof(full));
        pthread_mutex_unlock(&p->lock);
        SDL_Surface *surface = decoded_surface(full, &w, &h, style);
        pthread_mutex_lock(&p->lock);
        p->result = surface;
        p->w = w;
        p->h = h;
        p->done = 1;
    }
    pthread_mutex_unlock(&p->lock);
    return NULL;
}

static void prefetch_close(struct mainui_render_prefetch *p) {
    if (p->state != 1) return;
    pthread_mutex_lock(&p->lock);
    p->stop = 1;
    pthread_cond_signal(&p->wake);
    pthread_mutex_unlock(&p->lock);
    pthread_join(p->thread, NULL);
    if (p->result) SDL_FreeSurface(p->result);
    pthread_cond_destroy(&p->wake);
    pthread_mutex_destroy(&p->lock);
    memset(p, 0, sizeof(*p));
}

struct prefetch_job {
    char key[1100], path[1024];
    int w, h, style;  // 0 x 0 for the picture as it is
};

static int prefetch_set(struct prefetch_job *job, const char *key, const char *path, int w, int h, int style) {
    if (snprintf(job->key, sizeof(job->key), "%s", key) >= (int)sizeof(job->key) ||
        snprintf(job->path, sizeof(job->path), "%s", path) >= (int)sizeof(job->path)) return 0;
    job->w = w;
    job->h = h;
    job->style = style;
    return 1;
}

// A system's background in system_picture's order: its BrickOS file, unless there is none or it
// does not decode, then its config's background.
static int prefetch_system(struct mainui_renderer *r, const struct mainui_frontend *fe, int visible, struct prefetch_job *job) {
    const struct mainui_system *s = mainui_frontend_system(fe, visible);
    if (!s) return 0;
    char own[PATH_BYTES];
    if (s->folder[0] && snprintf(own, sizeof(own), "/mnt/SDCARD/BrickOS/wallpapers/systems/%s.png", s->folder) < (int)sizeof(own)) {
        int state = image_state(r, own, own);
        if (state == 0) return prefetch_set(job, own, own, 0, 0, 0);
        if (state == 1) return 0;
    }
    const char *background = s->backdrop.background;
    return background[0] == '/' && image_state(r, background, background) == 0 &&
           prefetch_set(job, background, background, 0, 0, 0);
}

// A Carousel cover, at the size the Carousel fits every cover in.
static int prefetch_cover(struct mainui_renderer *r, struct mainui_frontend *fe, const struct mainui_window *w, int index,
                          struct prefetch_job *job) {
    struct view_entry entry;
    char key[1100];
    view_entry(fe, w, index, &entry);
    if (entry.folder || !entry.image[0] || resized_key(key, sizeof(key), entry.image, 288, 368, RESIZED_FIT) < 0) return 0;
    return image_state(r, key, entry.image) == 0 && prefetch_set(job, key, entry.image, 288, 368, RESIZED_FIT);
}

// The screen the pictures follow, as one number: what it shows and where its focus is.
static unsigned long prefetch_screen(const struct mainui_frontend *fe, const struct mainui_window *w, int focus, int n) {
    unsigned long screen = (unsigned long)w->kind;
    int parts[] = {(int)fe->model.presentation, (int)fe->model.tab, fe->model.bar_focused, w->system, focus, n,
                   mainui_model_view(&fe->model, w), mainui_model_tab_layout(&fe->model, fe->model.tab), fe->model.depth};
    for (size_t i = 0; i < sizeof(parts) / sizeof(parts[0]); ++i) screen = screen * 1000003UL + (unsigned long)(parts[i] + 7);
    return screen;
}

static int prefetch_next(struct mainui_renderer *r, struct mainui_frontend *fe, const struct mainui_window *w, int focus, int n,
                         struct prefetch_job *job) {
    enum mainui_tab tab = fe->model.tab;
    if (n < 2) return 0;
    if (w->kind == MAINUI_WIN_TABS && !fe->model.bar_focused && (tab == MAINUI_TAB_GAMES || tab == MAINUI_TAB_BESTS)) {
        int layout = mainui_model_tab_layout(&fe->model, tab);
        int columns = layout == 0 ? 1 : layout == 2 ? MAINUI_COMPACT_COLUMNS : MAINUI_GRID_COLUMNS;
        int steps[] = {1, -1, columns, -columns};
        for (int i = 0; i < (columns > 1 ? 4 : 2); ++i) {
            int next = focus + steps[i];
            if (layout == 0) next = (next % n + n) % n;  // the row wraps
            else if (next < 0 || next >= n) continue;
            if (prefetch_system(r, fe, tab == MAINUI_TAB_GAMES ? next : MAINUI_BEST_SYSTEM_BASE + next, job)) return 1;
        }
        return 0;
    }
    if (mainui_model_view(&fe->model, w) != MAINUI_VIEW_CAROUSEL) return 0;
    static const int offsets[] = {3, -3, 2, -2, 1, -1};
    for (size_t i = 0; i < sizeof(offsets) / sizeof(offsets[0]); ++i)
        if (prefetch_cover(r, fe, w, ((focus + offsets[i]) % n + n) % n, job)) return 1;
    return 0;
}

// The Home destinations' wallpapers, nearest the focus first: the step's own fades in first.
static int prefetch_home(struct mainui_renderer *r, const int *tabs, int n, int focus, struct prefetch_job *job) {
    for (int distance = 1; distance < n; ++distance)
        for (int side = 1; side >= -1; side -= 2) {
            int at = focus + side * distance;
            char path[PATH_BYTES];
            if (at < 0 || at >= n || !home_tab_wallpaper(tabs[at], path, sizeof(path))) continue;
            if (image_state(r, path, path) == 0) return prefetch_set(job, path, path, 0, 0, 0);
        }
    return 0;
}

// Makes the Home card nearest the focus that is not made yet, on this thread, which owns the
// renderer: the frames make the visible ones, so these are the cards a slide brings in. Returns 1
// when it made one. Cards drawn with older Appearance choices wait for the frame that drops them.
static int home_card_next(struct mainui_renderer *r, struct mainui_frontend *fe, const int *tabs, int n, int focus) {
    if (!r->home_cache_enabled || memcmp(r->appearance_drawn, fe->appearance.values, sizeof(r->appearance_drawn)) != 0)
        return 0;
    for (int distance = 0; distance < n; ++distance)
        for (int side = 1; side >= -1; side -= 2) {
            int at = focus + side * distance;
            if (at < 0 || at >= n || (distance == 0 && side < 0)) continue;
            const struct mainui_home_card_cache *cache = &r->home_cards[tabs[at]];
            if (cache->attempted && strcmp(cache->label, brickos_home_label(fe, tabs[at])) == 0) continue;
            (void)home_card_prepare(r, fe, tabs[at]);
            return 1;
        }
    return 0;
}

// A finished picture becomes its cache entry, unless the file changed meanwhile or a frame
// decoded it first.
static void prefetch_install(struct mainui_renderer *r, const char *key, const char *full, SDL_Surface *surface, int w, int h,
                             int resized, const struct stat *queued_stamp) {
    struct mainui_render_image *entry = NULL;
    struct stat stamp;
    for (int i = 0; i < r->cache_count; ++i)
        if (strcmp(r->cache[i].path, key) == 0) entry = &r->cache[i];
    int changed = stat(full, &stamp) != 0 || !same_file(queued_stamp, &stamp);
    int held = entry && (entry->texture || entry->failed) && image_unchanged(entry, &stamp);
    if (!changed && !held && !entry && r->cache_count < MAINUI_RENDER_CACHE) {
        entry = &r->cache[r->cache_count++];
        memset(entry, 0, sizeof(*entry));
        snprintf(entry->path, sizeof(entry->path), "%s", key);
    }
    if (changed || held || !entry) {
        if (surface) SDL_FreeSurface(surface);
        return;
    }
    if (entry->texture) SDL_DestroyTexture(entry->texture);
    entry->texture = NULL;
    install_picture(r, entry, surface, w, h, resized, &stamp);
}

int mainui_render_prefetch(struct mainui_renderer *r, struct mainui_frontend *fe, uint64_t now_ms) {
    struct mainui_render_prefetch *p = &r->prefetch;
    if (!r->animations_enabled || fe->model.presentation != MAINUI_PRESENTATION_MATERIAL_YOU) return 0;
    // The screen's own window only; the Home has its destinations for neighbours.
    const struct mainui_window *top = mainui_model_top(&fe->model);
    int home = top->kind == MAINUI_WIN_HOME, tabs[MAINUI_HOME_COUNT], home_count = 0, home_focus = -1;
    if (home) {
        home_count = mainui_model_home_items(&fe->model, tabs);
        home_focus = brickos_tab_focus(&fe->model, tabs, home_count);
    }
    // A slide or fade under way draws every frame; its pictures wait until it ends.
    if (mainui_frontend_carousel_shift(fe, now_ms) != 0 || mainui_frontend_background_fade(fe, now_ms) < 1 ||
        (home && r->home_motion.active)) return 1;
    if (p->state == 0) {
        p->state = -1;
        if (pthread_mutex_init(&p->lock, NULL) != 0) return 0;
        if (pthread_cond_init(&p->wake, NULL) != 0) {
            pthread_mutex_destroy(&p->lock);
            return 0;
        }
        if (pthread_create(&p->thread, NULL, prefetch_worker, p) != 0) {
            pthread_cond_destroy(&p->wake);
            pthread_mutex_destroy(&p->lock);
            return 0;
        }
        p->state = 1;
    }
    if (p->state != 1) return 0;
    pthread_mutex_lock(&p->lock);
    if (p->queued) {
        if (!p->done) {
            pthread_mutex_unlock(&p->lock);
            // The Home's cards out of view are made meanwhile.
            if (home) (void)home_card_next(r, fe, tabs, home_count, home_focus);
            return 1;
        }
        SDL_Surface *surface = p->result;
        int w = p->w, h = p->h, resized = p->style != 0;
        struct stat stamp = p->stamp;
        char key[sizeof(p->key)], full[sizeof(p->full)];
        memcpy(key, p->key, sizeof(key));
        memcpy(full, p->full, sizeof(full));
        p->result = NULL;
        p->queued = p->done = 0;
        pthread_mutex_unlock(&p->lock);
        prefetch_install(r, key, full, surface, w, h, resized, &stamp);
        return 1;
    }
    pthread_mutex_unlock(&p->lock);
    // Below the count at which the frames empty the cache.
    int carousel = top->kind == MAINUI_WIN_ROMS || (top->kind == MAINUI_WIN_TABS &&
                   (fe->model.tab == MAINUI_TAB_RECENT || fe->model.tab == MAINUI_TAB_FAVORITES));
    int n = home ? home_count : carousel ? view_count(fe, top) :
            fe->model.tab == MAINUI_TAB_GAMES ? fe->visible_count : fe->shown_best_count;
    int focus = home ? home_focus : top->kind == MAINUI_WIN_ROMS ? top->focus : fe->model.tabs[fe->model.tab].focus;
    unsigned long screen = prefetch_screen(fe, top, focus, n);
    // With every wallpaper held, the Home still has its cards out of view to make.
    if (p->settled && p->settled_screen == screen) return home && home_card_next(r, fe, tabs, n, focus);
    struct prefetch_job job;
    char full[sizeof(p->full)];
    struct stat stamp;
    int next = r->cache_count < MAINUI_RENDER_CACHE - MAINUI_RENDER_FRAME_IMAGES &&
               (home ? prefetch_home(r, tabs, n, focus, &job) : prefetch_next(r, fe, top, focus, n, &job));
    if (!next ||
        snprintf(full, sizeof(full), "%s%s", r->fs->root, job.path) >= (int)sizeof(full) || stat(full, &stamp) != 0) {
        p->settled = 1;
        p->settled_screen = screen;
        return home && home_card_next(r, fe, tabs, n, focus);
    }
    p->settled = 0;
    pthread_mutex_lock(&p->lock);
    memcpy(p->key, job.key, sizeof(p->key));
    memcpy(p->full, full, sizeof(p->full));
    p->w = job.w;
    p->h = job.h;
    p->style = job.style;
    p->stamp = stamp;
    p->queued = 1;
    p->done = 0;
    pthread_cond_signal(&p->wake);
    pthread_mutex_unlock(&p->lock);
    return 1;
}

static void *early_worker(void *context) {
    struct mainui_render_early *e = context;
    e->surface = IMG_Load(e->full);
    return NULL;
}

int mainui_render_early_start(struct mainui_render_early *e, const struct mainui_frontend *fe) {
    memset(e, 0, sizeof(*e));
    if (fe->model.presentation != MAINUI_PRESENTATION_MATERIAL_YOU || mainui_model_top(&fe->model)->kind != MAINUI_WIN_HOME)
        return 0;
    int tabs[MAINUI_HOME_COUNT], n = mainui_model_home_items(&fe->model, tabs);
    int focus = brickos_tab_focus(&fe->model, tabs, n);
    if (focus < 0 || focus >= n || !home_tab_wallpaper(tabs[focus], e->path, sizeof(e->path))) return 0;
    int length = snprintf(e->full, sizeof(e->full), "%s%s", fe->fs.root ? fe->fs.root : "", e->path);
    if (length < 0 || length >= (int)sizeof(e->full) || stat(e->full, &e->stamp) != 0 || !S_ISREG(e->stamp.st_mode)) return 0;
    if (pthread_create(&e->thread, NULL, early_worker, e) != 0) return 0;
    e->started = 1;
    return 1;
}

void mainui_render_early_take(struct mainui_renderer *r, struct mainui_render_early *e) {
    if (!e->started) return;
    pthread_join(e->thread, NULL);
    e->started = 0;
    SDL_Surface *surface = e->surface;
    e->surface = NULL;
    if (!surface) return;
    struct stat now;
    int usable = r && stat(e->full, &now) == 0 && same_file(&e->stamp, &now) && r->cache_count < MAINUI_RENDER_CACHE;
    for (int i = 0; usable && i < r->cache_count; ++i)
        if (strcmp(r->cache[i].path, e->path) == 0) usable = 0;
    if (!usable) {
        SDL_FreeSurface(surface);
        return;
    }
    struct mainui_render_image *entry = &r->cache[r->cache_count++];
    memset(entry, 0, sizeof(*entry));
    snprintf(entry->path, sizeof(entry->path), "%s", e->path);
    install_picture(r, entry, surface, surface->w, surface->h, 0, &e->stamp);
}
