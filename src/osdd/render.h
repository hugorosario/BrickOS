#ifndef BRICKOS_OSDD_RENDER_H
#define BRICKOS_OSDD_RENDER_H

#include "look.h"
#include "profile.h"
#include "power.h"
#include "toast.h"
#include "../common/brickos_palette.h"

#include <SDL.h>
#include <SDL_ttf.h>
#include <stddef.h>

#define OSDD_RENDERER_ASSETS 48
#define OSDD_LOOK_FONTS 8

// A font of the BrickOS look at one size, kept open (failed when it does not open).
struct osdd_look_font {
    char path[256];
    int size, failed;
    TTF_Font *font;
};

struct osdd_renderer_asset {
    char path[192];
    SDL_Surface *surface;
};

struct osdd_renderer {
    SDL_Surface *frame;
    // Installed panel assets and the panel font, decoded on first use and kept until
    // close; reloading them for every frame delayed each panel update by 100-300 ms.
    struct osdd_renderer_asset assets[OSDD_RENDERER_ASSETS];
    size_t asset_count;
    TTF_Font *panel_font;
    struct osdd_look_font look_fonts[OSDD_LOOK_FONTS];
    int look_font_next;
};

// Opens a theme profile's typeface and image with SDL_ttf and SDL_image.
extern const struct osdd_profile_assets osdd_renderer_profile_assets;

int osdd_renderer_open(struct osdd_renderer *renderer, char *error, size_t error_size);
// Returns the decoded image at path, owned by the renderer, or NULL with an error.
SDL_Surface *osdd_renderer_asset(struct osdd_renderer *renderer, const char *path,
                                 char *error, size_t error_size);
int osdd_renderer_clear(struct osdd_renderer *renderer, char *error, size_t error_size);
int osdd_renderer_toast(struct osdd_renderer *renderer,
                        const struct osdd_toast *toast, char *error, size_t error_size);
// The power-off confirmation or progress: in the BrickOS look when look is set (the profile is
// then not used), otherwise in the profile's style or the stock one.
// The power dialog: Power off, Reboot and Cancel under the Power title, or the progress of the
// chosen action. With look it is drawn in the BrickOS look, in the look's language; otherwise
// in the profile's look or the stock look, in Portuguese when portuguese is set.
int osdd_renderer_power(struct osdd_renderer *renderer,
                        const struct osdd_power_dialog *power,
                        const struct osdd_profile *profile,
                        const struct osdd_look *look, int portuguese,
                        char *error, size_t error_size);
void osdd_renderer_close(struct osdd_renderer *renderer);

// ---- The BrickOS look (docs/osdd.md), for the toasts, the panel and the power dialog ----

// The look's palette, built as MainUI builds it.
void osdd_look_palette(const struct osdd_look *look, struct mainui_material_palette *palette);
// The corner radius of a w x h card in the look's shape, as MainUI's.
int osdd_look_corner(const struct osdd_look *look, int w, int h);
// A filled card, its corners rounded to radius (0: square), with straight alpha.
void osdd_look_round(SDL_Surface *frame, SDL_Rect area, int radius, SDL_Color color);
// A ring thickness pixels wide inside a card's edge, drawn over what is there.
void osdd_look_outline(SDL_Surface *frame, SDL_Rect area, int radius, int thickness, SDL_Color color);
// MainUI's focus around a card: glow rings at alpha 34 and 72 (none with the Outline halo), then
// a solid 3-pixel outline in the accent. The OSD's halo never pulses.
void osdd_look_focus(SDL_Surface *frame, SDL_Rect area, const struct osdd_look *look,
                     const struct mainui_material_palette *palette);
// One line of text in the look's fonts at size, ended with "..." to fit width: the Appearance
// font draws its characters and the system font the ones it lacks. Returns a new surface, or
// NULL when no font opens or the text is empty.
SDL_Surface *osdd_look_text(struct osdd_renderer *renderer, const struct osdd_look *look, int size,
                            const char *text, int width, SDL_Color color);

#endif
