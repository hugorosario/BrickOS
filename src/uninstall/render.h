#ifndef BRICKOS_UNINSTALL_RENDER_H
#define BRICKOS_UNINSTALL_RENDER_H

#include "model.h"
#include "../common/brickos_palette.h"

#include <SDL.h>
#include <SDL_ttf.h>
#include <stddef.h>

// Draws the Uninstall BrickOS app's screens into a software ARGB surface of the Brick's size,
// in the BrickOS palette (brickos_palette.h, the default accent, Dark) with the stock Default
// theme's font. The confirmation screen lists what goes and what stays, with Cancel and
// Uninstall at the bottom and a focus band on the focused choice; a notice above the lists says
// that the last uninstall did not finish, with its reason. The error screen gives the reason and
// one button back to MainUI; the restarting screen says that the Brick restarts.

#define UNINSTALL_SCREEN_W 1024
#define UNINSTALL_SCREEN_H 768
#define UNINSTALL_LAYOUT_LINES 64

enum uninstall_line_kind {
    UNINSTALL_LINE_TITLE, UNINSTALL_LINE_NOTICE, UNINSTALL_LINE_HEADING, UNINSTALL_LINE_BODY,
    UNINSTALL_LINE_CHOICE, UNINSTALL_LINE_HINT,
};

// Where a frame put its texts, for tests: each line of text as the box its pixels can reach.
struct uninstall_layout {
    int count;
    SDL_Rect lines[UNINSTALL_LAYOUT_LINES];
    enum uninstall_line_kind kinds[UNINSTALL_LAYOUT_LINES];
    SDL_Rect cancel, uninstall;  // the confirmation screen's choices
    SDL_Rect back;               // the error screen's button
    SDL_Rect focus;              // the focus band, empty when none shows
    SDL_Rect notice;             // the notice's band, empty when none shows
    SDL_Rect content;            // everything above the choices
    int shortened;               // texts cut to fit, with "..."
};

struct uninstall_renderer {
    TTF_Font *title, *body, *small;
    struct mainui_material_palette palette;
    SDL_Color notice_background, notice_text;
};

// The stock Default theme's font for a system.json language value, as MainUI's fallback finds
// it, as a host path below root. Returns 0, or -1 when there is none.
int uninstall_font(const char *root, const char *language, char *out, size_t size);

int uninstall_renderer_open(struct uninstall_renderer *r, const char *font);
void uninstall_renderer_close(struct uninstall_renderer *r);
// Draws the model's screen; layout, when not NULL, receives where everything went.
void uninstall_render(const struct uninstall_renderer *r, SDL_Surface *screen, const struct uninstall_model *m,
                      struct uninstall_layout *layout);

#endif
