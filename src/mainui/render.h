#ifndef BRICKOS_MAINUI_RENDER_H
#define BRICKOS_MAINUI_RENDER_H

#include "frontend.h"

#include "../common/material_palette.h"
#include <SDL.h>
#include <SDL_ttf.h>
#include <pthread.h>
#include <sys/stat.h>

// Draws the frontend's state at 1024x768 with SDL2, the installed theme's skin images, and its
// font. Geometry, font sizes, and the skin image each part uses come from the official
// MainUI's captures, measured to the pixel (Games grid tiles 300x300 on a 320-pixel pitch, list
// rows, popups, dialogs, the home page). When a theme lacks a skin image, a plain shape in the
// captured colors stands in; visual parity is judged against the recorded fixtures (task 5.2).

#define MAINUI_SCREEN_W 1024
#define MAINUI_SCREEN_H 768
#define MAINUI_RENDER_CACHE 160
#define MAINUI_RENDER_FRAME_IMAGES 80  // the most images one frame loads (the keyboard and a list with box art stay far below)

struct mainui_status {
    int battery;     // percent
    int charging;
    char clock[8];   // "16:24"
};

struct mainui_render_image {
    char path[1100];
    SDL_Texture *texture;
    int w, h;
    struct stat source_stamp; // resized covers can be published or replaced during a session
    int failed;               // the file at source_stamp did not decode; a new file is tried again
};

// A text role from the theme's config.json "fontsize". The Default theme has no config.json and
// uses the sizes TRIMUI Green's config holds, which match the Default captures.
struct mainui_font {
    TTF_Font *font;
    TTF_Font *fallback; // installed language font, when a theme font lacks a glyph
    int size;
    int shift;  // moves the text down so it keeps the Default size's baseline
    int fallback_shift;
    // A BrickOS Appearance font: a text it cannot draw whole is drawn in runs, the fallback
    // drawing only the characters the font lacks (otherwise the fallback draws the whole text).
    int mixed;
};



struct mainui_home_motion {
    int valid, active, count, first, search;
    int focus;                      // a home destination: a tab or MAINUI_HOME_GAMESWITCHER
    uint64_t started_ms;
    float position, from_position;
    float emphasis[MAINUI_HOME_COUNT], from_emphasis[MAINUI_HOME_COUNT];
    float wallpaper[MAINUI_HOME_COUNT], from_wallpaper[MAINUI_HOME_COUNT];
};

struct mainui_home_card_cache {
    SDL_Texture *normal, *focused;
    SDL_Surface *icon;
    int icon_attempted;
    char label[128];
    int attempted;
};

// The focused loading-screen preset's background (0) and frames, loaded the first time each is
// drawn. It is kept apart from the general cache and emptied when the focus moves to another
// entry or the selector closes, so browsing presets never fills that cache with frames.
struct mainui_loading_preview {
    char folder[MAINUI_LOADING_SCREEN_PATH];
    uint64_t started_ms;
    SDL_Texture *images[MAINUI_LOADING_SCREEN_FRAMES + 1];
    int w[MAINUI_LOADING_SCREEN_FRAMES + 1], h[MAINUI_LOADING_SCREEN_FRAMES + 1];
    unsigned char failed[MAINUI_LOADING_SCREEN_FRAMES + 1];
};

// A font list sample: the font a row's name is drawn in, opened the first time the row is drawn
// and closed when the list closes or the text size changes.
#define MAINUI_FONT_SAMPLES 16
struct mainui_font_sample {
    char file[MAINUI_FONT_NAME]; // "" for Default, the language's font
    int size;
    TTF_Font *font;              // NULL when it does not open
};

// Pictures decoded ahead (render.c): while no slide or fade runs, a worker thread decodes, one at
// a time, a picture the next step may need; the main thread then makes it a cached texture. The
// job and its result pass under lock.
struct mainui_render_prefetch {
    pthread_t thread;
    pthread_mutex_t lock;
    pthread_cond_t wake;
    int state;              // 0 not started, 1 running, -1 it could not start
    int stop, queued, done;
    char key[1100], full[1100];
    int w, h, style;        // the fitted size and style; the size made once done
    struct stat stamp;      // the file as it was when queued
    SDL_Surface *result;    // NULL when it did not decode
    // The screen whose neighbours' pictures are all held: nothing to look for until it changes.
    int settled;
    unsigned long settled_screen;
};

struct mainui_renderer {
    SDL_Renderer *sdl;
    const struct mainui_fs *fs;
    char theme[MAINUI_THEME_PATH_BYTES];
    char language[64];
    // nav_font1, content_font1, content_font2, stat_font1 (the hints), menu_font1, popup_font1..3,
    // content_font5 (the focused grid tile's label), nav_font2 (the status bar), and content_font2
    // again for BrickOS's footer hints and status bar, which keep their size at every Text size,
    // and BrickOS's game tile titles in the Grid view, which no theme sizes
    struct mainui_font nav, content, small, stat, menu, title, popup_text, popup_button, focus_label, status_text, hint,
        tile;
    // What the fonts were opened with besides the theme and the language: BrickOS's Appearance
    // font (a path under the fonts folder, or "" for the Default font) and Text size.
    char font_file[MAINUI_FONT_NAME + sizeof(MAINUI_FONTS_DIR)];
    int text_size;
    // From "fontcolor": nav_color1..3, content_color2, content_color3, popup_color1,
    // highlight_color1; status_color is nav_color3 again, with another default.
    SDL_Color nav_dim, nav_focus, home_dim, dim, number, title_color, highlight, status_color;
    struct mainui_material_palette material;
    int material_accent_valid;
    struct mainui_render_image cache[MAINUI_RENDER_CACHE];
    int cache_count;
    struct mainui_status status;
    enum mainui_presentation presentation;
    int animations_enabled; // live frontend opts in; static diagnostic captures remain settled
    uint64_t frame_ms;
    int stock_motion_drawn; // the last Stock++ frame caught the cards or the background moving
    int brickos_motion_drawn; // the last BrickOS frame caught a carousel sliding
    // The covers a live BrickOS frame may still decode, and whether one waits for the next frame.
    int covers_left, covers_pending;
    struct mainui_home_motion home_motion;
    int home_cache_enabled;
    struct mainui_home_card_cache home_cards[MAINUI_HOME_COUNT];
    SDL_Texture *focus_aura[25]; // reusable rounded glow, indexed by corner radius
    int focus_pulse_visible, focus_pulse_suppressed;
    struct mainui_loading_preview loading_preview;
    // BrickOS's Themes > Appearance choices, the frontend's from the first frame on (the defaults
    // before it), and the ones the cached home cards were drawn with.
    const struct mainui_appearance *appearance;
    int appearance_drawn[MAINUI_APPEAR_SETTINGS];
    struct mainui_font_sample font_samples[MAINUI_FONT_SAMPLES];
    int font_sample_count;
    struct mainui_render_prefetch prefetch;
    // How long the last frame spent decoding pictures it did not hold, in performance-counter
    // ticks: the stall that holds the slides and fades it showed (mainui_render_decode_ms).
    Uint64 decode_ticks;
};

// Opens the theme's font for a language; returns 0, or -1 when no font opens.
int mainui_render_open(struct mainui_renderer *r, SDL_Renderer *sdl, const struct mainui_fs *fs,
                       const char *theme, const char *theme_font, const char *language);
void mainui_render_close(struct mainui_renderer *r);
// Draws one frame of the frontend's state.
void mainui_render_frame(struct mainui_renderer *r, struct mainui_frontend *fe);
// Explicit monotonic time supports deterministic animation captures and tests.
void mainui_render_frame_at(struct mainui_renderer *r, struct mainui_frontend *fe, uint64_t now_ms);
int mainui_render_animating(const struct mainui_renderer *r, const struct mainui_frontend *fe);
int mainui_render_frame_due(const struct mainui_renderer *r, const struct mainui_frontend *fe, uint64_t now_ms);
// The rows of a settings extension's output view for TEXT after its long lines wrap, with the
// view's font and width in the renderer's presentation: the frontend's text_rows hook.
int mainui_render_text_rows(void *renderer, const char *text);
// Decodes ahead, in a worker, the pictures BrickOS's next step may need: the system backgrounds of
// the Games or Bests focus's neighbours, the covers that a Carousel step brings in, and on the
// Home the other destinations' wallpapers, whose cards out of view it also makes, one a turn. Call
// it each turn of the loop; it waits while a slide or fade runs. Returns 1 while it has work left.
int mainui_render_prefetch(struct mainui_renderer *r, struct mainui_frontend *fe, uint64_t now_ms);
// How many milliseconds the last frame spent decoding pictures it did not hold yet.
uint64_t mainui_render_decode_ms(const struct mainui_renderer *r);

// The first frame's wallpaper, decoded on a thread of its own while SDL starts: when BrickOS opens
// on its desktop, the focused destination's picture. It needs neither SDL's start nor a renderer.
struct mainui_render_early {
    pthread_t thread;
    int started;
    char path[1100], full[1100]; // the cache key, and the file under the root
    struct stat stamp;           // the file as it was when the decode started
    SDL_Surface *surface;        // NULL when it did not decode
};
// Starts the decode when the frontend's first view is BrickOS's desktop and its wallpaper exists.
// Returns 1 when a decode started.
int mainui_render_early_start(struct mainui_render_early *e, const struct mainui_frontend *fe);
// Waits for the decode and gives its picture to r's cache, unless the file changed meanwhile; the
// frame then decodes it as before. With r NULL it only waits and frees the picture.
void mainui_render_early_take(struct mainui_renderer *r, struct mainui_render_early *e);

#endif
