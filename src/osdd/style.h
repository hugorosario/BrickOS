#ifndef BRICKOS_OSDD_STYLE_H
#define BRICKOS_OSDD_STYLE_H

#include "toast.h"

#include <stddef.h>

// How one toast record is drawn. Without a theme profile, the stock look comes from the record
// and the stock assets. With the profile the record was consumed under, the typeface, size,
// color, and background come from the profile. The record's type, message, position, icon, and
// lifetime keep their stock roles either way, and a record with its own background image keeps
// the stock look whole, because that image is its content.
struct osdd_toast_style {
    int themed;
    const char *font_path;            // Stock: the typeface file to open.
    int font_px;                      // Stock: the size to open it at.
    void *font;                       // Themed: the profile's open typeface.
    unsigned char color[4];           // Red, green, blue, alpha.
    // The stock image whose size the toast takes; without a profile it is also drawn.
    const char *stock_background;
    const char *background_path;      // Themed: the profile's image, or NULL.
    void *background;                 // Themed: that image, open.
    int margins[4];                   // Its nine-patch margins: left, top, right, bottom.
    int solid;                        // Themed: a solid color instead of an image.
    unsigned char solid_color[4];
    int padding;                      // The inset of a wide toast with an icon.
    int tint_icon;                    // Themed: draw the icon as a silhouette in icon_color.
    unsigned char icon_color[4];
};

// Returns 0 with the style of a record, or -1 when the stock renderer refuses the record.
int osdd_toast_style(const struct osdd_toast *toast, struct osdd_toast_style *style,
                     char *error, size_t error_size);

#endif
