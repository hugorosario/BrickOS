#include "render.h"

#include "system.h"
#include "material.h"

#include <SDL_image.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SCREEN_W 1024
#define SCREEN_H 768
#define ROW_H 60
#define CARD_Y 143
#define CARD_GAP 40
#define CARD_FIRST_X (-434)
#define THUMB_W 564
#define THUMB_H 315
#define THUMB_Y 163
#define LABEL_Y 508
#define LABEL_H 39
#define NOTIFY_Y 718
#define NOTIFY_H 50

// gMenuTextColor and gMenuTextSelColor in the official library's data.
static const SDL_Color text_color = {0xf0, 0xf0, 0xf0, 0x00};
static const SDL_Color focus_color = {0xf0, 0xf0, 0xf0, 0x00};

// C integer division, as the official library's (x + (x >> 31)) >> 1.
static int half(int value) { return value / 2; }

enum align { ALIGN_START, ALIGN_CENTER, ALIGN_END };

// draw_menu_string: renders text and blits it into rect.
static void draw_text(SDL_Surface *screen, TTF_Font *font, const char *text, SDL_Rect rect, SDL_Color color,
                      enum align align) {
    if (!font || !text) return;
    SDL_Surface *surface = TTF_RenderUTF8_Blended(font, text, color);
    if (!surface) return;
    SDL_Rect dst = rect;
    if (align == ALIGN_CENTER) {
        dst.x = rect.x + half(rect.w - surface->w);
        dst.y = rect.y + half(rect.h - surface->h);
    } else if (align == ALIGN_END) {
        dst.x = rect.x + rect.w - surface->w;
    }
    SDL_BlitSurface(surface, NULL, screen, &dst);
    SDL_FreeSurface(surface);
}

static TTF_Font *open_font(const char *theme, const char *res, const char *name, int size) {
    char path[512];
    TTF_Font *font = NULL;
    if (theme) {
        snprintf(path, sizeof(path), "%s/%s", theme, name);
        font = TTF_OpenFont(path, size);
    }
    if (!font) {
        snprintf(path, sizeof(path), "%s/%s", res, name);
        font = TTF_OpenFont(path, size);
    }
    return font;
}

static SDL_Surface *open_image(const char *theme, const char *res, const char *name) {
    char path[512];
    SDL_Surface *image = NULL;
    if (theme) {
        snprintf(path, sizeof(path), "%s/%s", theme, name);
        image = IMG_Load(path);
    }
    if (!image) {
        snprintf(path, sizeof(path), "%s/%s", res, name);
        image = IMG_Load(path);
    }
    return image;
}

int tmenu_assets_open(struct tmenu_assets *a, const char *theme, const char *res, const char *font_name,
                      char *error, size_t error_size) {
    memset(a, 0, sizeof(*a));
    struct { SDL_Surface **surface; const char *name; int required; } images[] = {
        {&a->menu_bg, "skin/menu-bg.png", 1},        {&a->record, "skin/bg-record-n.png", 1},
        {&a->record_focus, "skin/bg-record-f.png", 1}, {&a->list_sel, "skin/menu-list-sel.png", 1},
        {&a->arrow_focus, "skin/ic-right-arrow-f.png", 1}, {&a->arrow, "skin/ic-right-arrow-a.png", 0},
        {&a->mask, "skin/background-mask.png", 0},
    };
    a->font = open_font(theme, res, font_name, 32);
    if (!a->font) goto font_failed;
    a->small_font = open_font(theme, res, font_name, 16);
    if (!a->small_font) goto font_failed;
    for (size_t i = 0; i < sizeof(images) / sizeof(images[0]); ++i) {
        *images[i].surface = open_image(theme, res, images[i].name);
        if (!*images[i].surface && images[i].required) {
            snprintf(error, error_size, "open %s failed", images[i].name);
            tmenu_assets_close(a);
            return -1;
        }
    }
    return 0;
font_failed:
    snprintf(error, error_size, "open font %s failed: %s", font_name, TTF_GetError());
    tmenu_assets_close(a);
    return -1;
}

int tmenu_assets_open_material(struct tmenu_assets *a, const char *theme, const char *res,
                              const char *font_name, char *error, size_t error_size) {
    memset(a, 0, sizeof(*a));
    a->font = open_font(theme, res, font_name, 32);
    a->small_font = open_font(theme, res, font_name, 16);
    if (!a->font || !a->small_font) {
        snprintf(error, error_size, "open font %s failed: %s", font_name, TTF_GetError());
        tmenu_assets_close(a);
        return -1;
    }
    a->material = 1;
    return 0;
}

// ---- The themed look -------------------------------------------------------------------------

static void *theme_open_font(void *context, const char *path, int size) {
    (void)context;
    return TTF_OpenFont(path, size);
}

static void theme_close_font(void *context, void *font) {
    (void)context;
    TTF_CloseFont(font);
}

static void *theme_open_image(void *context, const char *path) {
    (void)context;
    return IMG_Load(path);
}

static void theme_close_image(void *context, void *image) {
    (void)context;
    SDL_FreeSurface(image);
}

const struct tmenu_theme_assets tmenu_theme_sdl_assets = {
    NULL, theme_open_font, theme_close_font, theme_open_image, theme_close_image
};

static int themed(const struct tmenu_theme *t) { return t != NULL && t->themed; }

static SDL_Color ink(const unsigned char c[4]) { return (SDL_Color){c[0], c[1], c[2], c[3]}; }

// A surface as a silhouette in one color: every pixel takes the color, and its own alpha scaled
// by the color's.
static SDL_Surface *silhouette(SDL_Surface *image, const unsigned char color[4]) {
    SDL_Surface *copy = image ? SDL_ConvertSurfaceFormat(image, SDL_PIXELFORMAT_ARGB8888, 0) : NULL;
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

void tmenu_assets_theme(struct tmenu_assets *a, const struct tmenu_theme *t) {
    if (!themed(t) || !t->has_icon) return;
    a->themed_arrow = silhouette(a->arrow ? a->arrow : a->arrow_focus, t->icon);
}

// Blends a color over a rectangle, as an image of that color would.
static void fill(SDL_Surface *screen, SDL_Rect rect, const unsigned char color[4]) {
    SDL_Surface *patch = SDL_CreateRGBSurfaceWithFormat(0, rect.w, rect.h, 32, SDL_PIXELFORMAT_ARGB8888);
    if (patch == NULL) return;
    SDL_FillRect(patch, NULL, SDL_MapRGBA(patch->format, color[0], color[1], color[2], color[3]));
    SDL_SetSurfaceBlendMode(patch, SDL_BLENDMODE_BLEND);
    SDL_BlitSurface(patch, NULL, screen, &rect);
    SDL_FreeSurface(patch);
}

// Stretches an image over a rectangle, keeping its margins unscaled (a nine-patch). Margins that
// do not fit the image or the rectangle stretch the whole image.
static void nine_patch(SDL_Surface *image, const int margins[4], SDL_Surface *screen, SDL_Rect to) {
    int left = margins[0], top = margins[1], right = margins[2], bottom = margins[3];
    if (left + right >= image->w || top + bottom >= image->h || left + right > to.w || top + bottom > to.h) {
        SDL_BlitScaled(image, NULL, screen, &to);
        return;
    }
    int sx[4] = {0, left, image->w - right, image->w}, sy[4] = {0, top, image->h - bottom, image->h};
    int dx[4] = {to.x, to.x + left, to.x + to.w - right, to.x + to.w};
    int dy[4] = {to.y, to.y + top, to.y + to.h - bottom, to.y + to.h};
    for (int row = 0; row < 3; ++row)
        for (int column = 0; column < 3; ++column) {
            SDL_Rect source = {sx[column], sy[row], sx[column + 1] - sx[column], sy[row + 1] - sy[row]};
            SDL_Rect target = {dx[column], dy[row], dx[column + 1] - dx[column], dy[row + 1] - dy[row]};
            if (source.w > 0 && source.h > 0 && target.w > 0 && target.h > 0)
                SDL_BlitScaled(image, &source, screen, &target);
        }
}

// A panel or card in the stock image's place and size: the theme's image or color, or the stock
// image when the profile gives neither.
static void panel(SDL_Surface *screen, const struct tmenu_theme *t, SDL_Surface *stock, SDL_Rect rect) {
    if (themed(t) && t->panel) nine_patch(t->panel, t->margins, screen, rect);
    else if (themed(t) && t->has_panel_color) fill(screen, rect, t->panel_color);
    else SDL_BlitSurface(stock, NULL, screen, &rect);
}

// The focus highlight in the stock image's place and size: the selector color, or the image.
static void highlight(SDL_Surface *screen, const struct tmenu_theme *t, SDL_Surface *stock, int x, int y) {
    SDL_Rect rect = {x, y, stock->w, stock->h};
    if (themed(t) && t->has_selector) fill(screen, rect, t->selector);
    else SDL_BlitSurface(stock, NULL, screen, &rect);
}

void tmenu_assets_close(struct tmenu_assets *a) {
    if (a->themed_arrow) SDL_FreeSurface(a->themed_arrow);
    if (a->font) TTF_CloseFont(a->font);
    if (a->small_font) TTF_CloseFont(a->small_font);
    SDL_Surface *images[] = {a->menu_bg, a->record, a->arrow, a->mask, a->list_sel, a->record_focus, a->arrow_focus};
    for (size_t i = 0; i < sizeof(images) / sizeof(images[0]); ++i)
        if (images[i]) SDL_FreeSurface(images[i]);
    memset(a, 0, sizeof(*a));
}

// gDefaultStrs: used only when the language file is missing.
static const char *default_text(int key) {
    switch (key) {
    case 92: return "Continue";
    case 93: return "Save";
    case 94: return "Load";
    case 95: return "Volume";
    case 96: return "Exit Game";
    case 97: return "Record %d/%d";
    case 100: return "load fail";
    case 101: return "save fail";
    case 102: return "slot%d saved";
    case 103: return "Fullscreen";
    case 104: return "On";
    case 105: return "Off";
    case 118: return "Netplay";
    case 126: return "Advanced Menu";
    default: return NULL;
    }
}

// getLangInJSON: the file's string for key, NULL when it has none.
static const char *text_for(const struct tmenu_texts *t, int key) {
    if (!t->loaded) return default_text(key);
    char name[16];
    snprintf(name, sizeof(name), "%d", key);
    const struct tmenu_json_member *m = tmenu_json_get(&t->json, name);
    return m && m->type == TMENU_JSON_STRING ? m->text : NULL;
}

void tmenu_texts_open(struct tmenu_texts *t, const char *res, const char *language) {
    memset(t, 0, sizeof(*t));
    char path[512];
    snprintf(path, sizeof(path), "%s/lang/%s.short", res, language);
    size_t length = 0;
    char *text = tmenu_read_file(path, &length);
    t->loaded = text && tmenu_json_parse(&t->json, text, length) == 0;
    free(text);
    static const int item_keys[TMENU_ITEMS] = {92, 93, 94, 95, 126, 118, 103, 96};
    for (int i = 0; i < TMENU_ITEMS; ++i) t->strings.items[i] = text_for(t, item_keys[i]);
    t->strings.record = text_for(t, 97);
    t->strings.load_fail = text_for(t, 100);
    t->strings.save_fail = text_for(t, 101);
    t->strings.slot_saved = text_for(t, 102);
    t->strings.on = text_for(t, 104);
    t->strings.off = text_for(t, 105);
}

static void blit_at(SDL_Surface *image, SDL_Surface *screen, int x, int y) {
    SDL_Rect dst = {x, y, 0, 0};
    SDL_BlitSurface(image, NULL, screen, &dst);
}

static void draw_main(SDL_Surface *screen, const struct tmenu_assets *a, const struct tmenu_theme *t,
                      struct tmenu_menu *m) {
    SDL_Rect bg = {half(SCREEN_W - a->menu_bg->w), half(SCREEN_H - a->menu_bg->h), a->menu_bg->w, a->menu_bg->h};
    panel(screen, t, a->menu_bg, bg);
    TTF_Font *font = themed(t) && t->font ? t->font : a->font;
    int row = 0;
    for (int item = m->first; item < TMENU_ITEMS && row <= TMENU_ROWS; ++item) {
        if (!m->visible[item]) continue;
        int y = bg.y + ROW_H + row * ROW_H;
        SDL_Color color = text_color, value_color = text_color;
        if (m->focus == item) {
            color = value_color = focus_color;
            int sel_y = y + half(ROW_H - a->list_sel->h);
            highlight(screen, t, a->list_sel, bg.x + 20, bg.y > sel_y ? bg.y : sel_y);
        }
        if (themed(t) && t->has_text) color = ink(t->text);
        if (themed(t) && t->has_secondary) value_color = ink(t->secondary);
        if (themed(t) && t->has_selected && m->focus == item) color = value_color = ink(t->selected);
        SDL_Rect rect = {bg.x, y, bg.w, ROW_H};
        draw_text(screen, font, m->strings->items[item], rect, color, ALIGN_CENTER);
        SDL_Surface *arrow = a->themed_arrow ? a->themed_arrow : a->arrow ? a->arrow : a->arrow_focus;
        if (item == TMENU_ITEM_SAVE ||
            (item == TMENU_ITEM_LOAD && m->load_index >= 0 && m->load_index < m->saved_count)) {
            blit_at(arrow, screen, bg.x + bg.w - 20 - arrow->w, y + half(ROW_H - arrow->h));
        } else if (item == TMENU_ITEM_VOLUME) {
            char text[32];
            snprintf(text, sizeof(text), "%02d/%02d", m->ops.keyshm_get(m->ops.context, 0), 20);
            draw_text(screen, font, text, rect, value_color, ALIGN_END);
        } else if (item == TMENU_ITEM_FULLSCREEN) {
            draw_text(screen, font, m->fullscreen_on ? m->strings->on : m->strings->off, rect, value_color, ALIGN_END);
        }
        ++row;
    }
}

// Draws three record cards; slots holds the ring of slot numbers, first the left one.
static void draw_cards(SDL_Surface *screen, const struct tmenu_assets *a, const struct tmenu_theme *t,
                       struct tmenu_menu *m, tmenu_thumb_path_fn thumb_path, const int *ring, int ring_size, int left) {
    TTF_Font *font = themed(t) && t->font ? t->font : a->font;
    SDL_Color label_color = themed(t) && t->has_secondary ? ink(t->secondary) : text_color;
    for (int card = 0; card < 3; ++card) {
        int slot = ring[(left + card) % ring_size];
        int x = card * (a->record->w + CARD_GAP) + CARD_FIRST_X;
        SDL_Rect card_rect = {x, CARD_Y, a->record->w, a->record->h};
        panel(screen, t, a->record, card_rect);
        if (card == 1) highlight(screen, t, a->record_focus, x, CARD_Y);
        if (slot >= 0 && slot < m->slot_count && m->slot_saved[slot]) {
            char *path = thumb_path(slot);
            if (path) {
                SDL_Surface *thumb = IMG_Load(path);
                if (thumb) {
                    float scale_w = (float)thumb->w / (float)THUMB_W, scale_h = (float)thumb->h / (float)THUMB_H;
                    float scale = scale_w <= scale_h ? scale_h : scale_w;
                    int h = (int)((float)thumb->h / scale), w = (int)((float)thumb->w / scale);
                    SDL_Rect dst = {x + 20 + half(THUMB_W - w), THUMB_Y + half(THUMB_H - h), w, h};
                    SDL_BlitScaled(thumb, NULL, screen, &dst);
                    SDL_FreeSurface(thumb);
                }
                free(path);
            }
        }
        char label[32];
        if (m->strings->record) {
            snprintf(label, sizeof(label), m->strings->record, slot + 1, m->slot_count);
            SDL_Rect rect = {x + 20, LABEL_Y, THUMB_W, LABEL_H};
            SDL_Color on_card = card == 1 && themed(t) && t->has_selected ? ink(t->selected) : label_color;
            draw_text(screen, font, label, rect, on_card, ALIGN_CENTER);
        }
    }
}

void tmenu_render_frame(SDL_Surface *screen, SDL_Surface *background, const struct tmenu_assets *a,
                        const struct tmenu_theme *t, struct tmenu_menu *m, tmenu_thumb_path_fn thumb_path) {
    if (a->material) {
        tmenu_render_material(screen, background, a, t, m, thumb_path);
        return;
    }
    if (background) {
        int x = half(SCREEN_W - background->w), y = half(SCREEN_H - background->h);
        SDL_Rect dst = {x < 0 ? 0 : x, y < 0 ? 0 : y, 0, 0};
        SDL_BlitSurface(background, NULL, screen, &dst);
    }
    if (a->mask) SDL_BlitSurface(a->mask, NULL, screen, NULL);
    char notice[TMENU_NOTIFY_BYTES + 1];
    if (tmenu_menu_notification(m, notice)) {
        SDL_Rect rect = {0, NOTIFY_Y, SCREEN_W, NOTIFY_H};
        draw_text(screen, themed(t) && t->font ? t->font : a->font, notice, rect,
                  themed(t) && t->has_secondary ? ink(t->secondary) : text_color, ALIGN_START);
    }
    if (m->screen == TMENU_SCREEN_SAVE) {
        int ring[TMENU_MAX_SLOTS];
        for (int i = 0; i < m->slot_count; ++i) ring[i] = i;
        int left = m->save_index - 1 < 0 ? m->save_index - 1 + m->slot_count : m->save_index - 1;
        draw_cards(screen, a, t, m, thumb_path, ring, m->slot_count, left);
    } else if (m->screen == TMENU_SCREEN_LOAD && m->saved_count > 0) {
        int left = m->load_index - 1 < 0 ? m->load_index - 1 + m->saved_count : m->load_index - 1;
        draw_cards(screen, a, t, m, thumb_path, m->saved, m->saved_count, left);
    } else {
        draw_main(screen, a, t, m);
    }
}

void tmenu_render_thumbnail(SDL_Surface *background, const char *path) {
    if (!background || !path) return;
    SDL_Surface *thumb = SDL_CreateRGBSurface(0, THUMB_W, THUMB_H, 32, 0xff0000, 0xff00, 0xff, 0xff000000);
    if (!thumb) return;
    SDL_BlitScaled(background, NULL, thumb, NULL);
    SDL_SaveBMP_RW(thumb, SDL_RWFromFile(path, "wb"), 1);
    SDL_FreeSurface(thumb);
}
