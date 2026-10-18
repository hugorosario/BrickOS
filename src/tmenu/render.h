#ifndef BRICKOS_TMENU_RENDER_H
#define BRICKOS_TMENU_RENDER_H

#include "json.h"
#include "menu.h"
#include "theme.h"

#include <SDL.h>
#include <SDL_ttf.h>

// Draws the menu with the same SDL calls, in the same order, as the official library
// (docs/tmenu-abi.md, "Frames"), so the same surfaces and fonts give the same pixels.

struct tmenu_assets {
    int material, portuguese, english;
    TTF_Font *font, *small_font;   // 32 and 16 points
    SDL_Surface *menu_bg, *record, *record_focus, *list_sel, *arrow_focus, *arrow, *mask;
    SDL_Surface *themed_arrow;     // The arrow as a silhouette in the theme's icon color, or NULL.
};

// Opens the fonts and images from the theme, then res. Returns 0, or -1 when an
// asset the official library requires is missing; error names it.
int tmenu_assets_open(struct tmenu_assets *assets, const char *theme, const char *res, const char *font_name,
                      char *error, size_t error_size);
int tmenu_assets_open_material(struct tmenu_assets *assets, const char *theme, const char *res,
                              const char *font_name, char *error, size_t error_size);
void tmenu_assets_close(struct tmenu_assets *assets);
// The theme's typeface and image, opened with SDL_ttf and SDL_image.
extern const struct tmenu_theme_assets tmenu_theme_sdl_assets;
// Prepares the surfaces a themed look draws from the stock ones: the tinted arrow.
void tmenu_assets_theme(struct tmenu_assets *assets, const struct tmenu_theme *theme);

struct tmenu_texts {
    struct tmenu_json json;
    int loaded;  // 0 when the language file is missing: English defaults
    struct tmenu_strings strings;
};

// Reads res/lang/<language>.short.
void tmenu_texts_open(struct tmenu_texts *texts, const char *res, const char *language);

// Paths of slot thumbnails, from the host table.
typedef char *(*tmenu_thumb_path_fn)(int slot);

// theme is NULL, or not themed, for the stock look.
void tmenu_render_frame(SDL_Surface *screen, SDL_Surface *background, const struct tmenu_assets *assets,
                        const struct tmenu_theme *theme, struct tmenu_menu *menu, tmenu_thumb_path_fn thumb_path);

// Writes the 564x315 thumbnail of background to path, as the official library does.
void tmenu_render_thumbnail(SDL_Surface *background, const char *path);

#endif
