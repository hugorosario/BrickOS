#include "style.h"
#include "profile.h"

#include <stdio.h>
#include <string.h>

static int fail(char *error, size_t length, const char *message) {
    if (error != NULL && length > 0) snprintf(error, length, "%s", message);
    return -1;
}

static const char *stock_background(const struct osdd_toast *toast) {
    static const char *backgrounds[] = {
        "/usr/trimui/osd/bg_msg_w1.png", "/usr/trimui/osd/bg_msg_w2.png",
        "/usr/trimui/osd/bg_msg_w3.png", "/usr/trimui/osd/bg_msg_w4.png"
    };
    if (toast->size < 0 || toast->size >= 4) return NULL;
    return backgrounds[toast->size];
}

// The stock payloads carry fontcolor as AARRGGBB; anything else is white.
static void record_color(const char *hex, unsigned char color[4]) {
    unsigned alpha, red, green, blue;
    color[0] = color[1] = color[2] = color[3] = 255;
    if (hex != NULL && strlen(hex) == 8 && sscanf(hex, "%2x%2x%2x%2x", &alpha, &red, &green, &blue) == 4) {
        color[0] = (unsigned char)red;
        color[1] = (unsigned char)green;
        color[2] = (unsigned char)blue;
        color[3] = (unsigned char)alpha;
    }
}

int osdd_toast_style(const struct osdd_toast *toast, struct osdd_toast_style *style,
                     char *error, size_t error_size) {
    if (toast == NULL || style == NULL) return fail(error, error_size, "invalid toast style input");
    memset(style, 0, sizeof(*style));
    style->padding = OSDD_PROFILE_DEFAULT_PADDING;
    const struct osdd_profile *profile = toast->profile;
    if (profile == NULL || toast->bg[0] != 0) {
        style->stock_background = toast->bg[0] != 0 ? toast->bg : stock_background(toast);
        if (style->stock_background == NULL) return fail(error, error_size, "unsupported stock toast size");
        if (toast->fontsize < 0 || toast->fontsize > 512)
            return fail(error, error_size, "unsupported stock font size");
        style->font_path = toast->font[0] == 0 ? OSDD_PROFILE_STOCK_FONT : toast->font;
        style->font_px = toast->fontsize > 0 ? toast->fontsize * 4 / 3 : 32;
        record_color(toast->fontcolor, style->color);
        return 0;
    }
    // Themed: the record keeps its size class, which gives the toast its stock size.
    style->stock_background = stock_background(toast);
    if (style->stock_background == NULL) return fail(error, error_size, "unsupported stock toast size");
    style->themed = 1;
    style->font = profile->font;
    style->font_px = profile->font_size;
    memcpy(style->color, profile->font_color, sizeof(style->color));
    if (profile->background != NULL) {
        style->background_path = profile->background_path;
        style->background = profile->background;
        memcpy(style->margins, profile->margins, sizeof(style->margins));
    } else if (profile->has_background_color) {
        style->solid = 1;
        memcpy(style->solid_color, profile->background_color, sizeof(style->solid_color));
    }
    style->padding = profile->padding;
    if (profile->has_icon_color) {
        style->tint_icon = 1;
        memcpy(style->icon_color, profile->icon_color, sizeof(style->icon_color));
    }
    return 0;
}
