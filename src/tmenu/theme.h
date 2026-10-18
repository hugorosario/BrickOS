#ifndef BRICKOS_TMENU_THEME_H
#define BRICKOS_TMENU_THEME_H

#include <stddef.h>

// The in-game menu's optional themed look, from the theme profile a frontend writes for the OSD
// (ingame-menu-theme). It is read each time the menu opens and dropped when it closes, so no
// state or watcher outlives a menu. The profile gives the ink; placement, sizes, layout, and
// animation stay stock. Any failure leaves that menu stock.

// Opens the typeface and the background image: SDL_ttf and SDL_image in the library, fakes in
// the host tests. A NULL handle means the file cannot be used.
struct tmenu_theme_assets {
    void *context;
    void *(*open_font)(void *context, const char *path, int size);
    void (*close_font)(void *context, void *font);
    void *(*open_image)(void *context, const char *path);
    void (*close_image)(void *context, void *image);
};

// A member the profile leaves out keeps the stock ink: the stock typeface, image, or color.
struct tmenu_theme {
    int themed;
    void *font, *small_font;          // The profile typeface at 32 and 16 points, or NULL.
    void *panel;                      // The panel and card image, or NULL.
    int margins[4];                   // Its nine-patch margins: left, top, right, bottom.
    int has_panel_color;              // A solid panel and card color, when there is no image.
    unsigned char panel_color[4];
    int has_text;                     // fontColor: the item names.
    unsigned char text[4];
    int has_secondary;                // secondaryColor, else the text color: values, labels, notices.
    unsigned char secondary[4];
    int has_selector;                 // selectorColor: the focused row and card.
    unsigned char selector[4];
    int has_selected;                 // selectedColor: the texts over them, else their usual ink.
    unsigned char selected[4];
    int has_icon;                     // iconColor: the arrows, as silhouettes.
    unsigned char icon[4];
};

// Reads the profile at path. Returns 0 with a themed look, or -1 with an error and a stock one
// (themed 0), when the file is missing, invalid, or names a typeface or image that does not open.
int tmenu_theme_open(struct tmenu_theme *theme, const char *path, const struct tmenu_theme_assets *assets,
                     char *error, size_t error_size);
void tmenu_theme_close(struct tmenu_theme *theme, const struct tmenu_theme_assets *assets);

#endif
