#include "theme.h"

#include "../osdd/profile.h"

#include <stdio.h>
#include <string.h>

void tmenu_theme_close(struct tmenu_theme *theme, const struct tmenu_theme_assets *assets) {
    if (theme == NULL) return;
    if (assets != NULL && assets->close_font != NULL) {
        if (theme->font) assets->close_font(assets->context, theme->font);
        if (theme->small_font) assets->close_font(assets->context, theme->small_font);
    }
    if (theme->panel && assets != NULL && assets->close_image != NULL)
        assets->close_image(assets->context, theme->panel);
    memset(theme, 0, sizeof(*theme));
}

static int fail(struct tmenu_theme *theme, const struct tmenu_theme_assets *assets, char *error,
                size_t error_size, const char *format, const char *detail) {
    if (error != NULL && error_size > 0) snprintf(error, error_size, format, detail);
    tmenu_theme_close(theme, assets);
    return -1;
}

int tmenu_theme_open(struct tmenu_theme *theme, const char *path, const struct tmenu_theme_assets *assets,
                     char *error, size_t error_size) {
    memset(theme, 0, sizeof(*theme));
    if (path == NULL || assets == NULL || assets->open_font == NULL || assets->open_image == NULL)
        return fail(theme, assets, error, error_size, "%s", "missing theme input");
    static struct osdd_profile profile;
    if (osdd_profile_read(path, &profile, error, error_size) < 0) {
        memset(theme, 0, sizeof(*theme));
        return -1;
    }
    // The menu's own sizes, whatever fontSize says: they keep the stock placement and rows.
    if (profile.font_path[0]) {
        theme->font = assets->open_font(assets->context, profile.font_path, 32);
        theme->small_font = theme->font ? assets->open_font(assets->context, profile.font_path, 16) : NULL;
        if (theme->font == NULL || theme->small_font == NULL)
            return fail(theme, assets, error, error_size, "typeface cannot be opened: %s", profile.font_path);
    }
    if (profile.background_path[0]) {
        theme->panel = assets->open_image(assets->context, profile.background_path);
        if (theme->panel == NULL)
            return fail(theme, assets, error, error_size, "background cannot be opened: %s", profile.background_path);
        memcpy(theme->margins, profile.margins, sizeof(theme->margins));
    } else if (profile.has_background_color) {
        theme->has_panel_color = 1;
        memcpy(theme->panel_color, profile.background_color, sizeof(theme->panel_color));
    }
    if (profile.has_font_color) {
        theme->has_text = 1;
        memcpy(theme->text, profile.font_color, sizeof(theme->text));
    }
    // Stock values and labels share the item color, so without secondaryColor they follow it.
    if (profile.has_secondary_color) {
        theme->has_secondary = 1;
        memcpy(theme->secondary, profile.secondary_color, sizeof(theme->secondary));
    } else if (profile.has_font_color) {
        theme->has_secondary = 1;
        memcpy(theme->secondary, profile.font_color, sizeof(theme->secondary));
    }
    if (profile.has_selector_color) {
        theme->has_selector = 1;
        memcpy(theme->selector, profile.selector_color, sizeof(theme->selector));
    }
    if (profile.has_selected_color) {
        theme->has_selected = 1;
        memcpy(theme->selected, profile.selected_color, sizeof(theme->selected));
    }
    if (profile.has_icon_color) {
        theme->has_icon = 1;
        memcpy(theme->icon, profile.icon_color, sizeof(theme->icon));
    }
    theme->themed = 1;
    return 0;
}
