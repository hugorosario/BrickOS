#include "presentation.h"

#include <string.h>

int mainui_presentation_parse(const char *name, enum mainui_presentation *out) {
    if (!name || !out) return -1;
    if (strcmp(name, "stock") == 0) *out = MAINUI_PRESENTATION_STOCK;
    else if (strcmp(name, "material-you") == 0)
        *out = MAINUI_PRESENTATION_MATERIAL_YOU;
    else return -1;
    return 0;
}

const char *mainui_presentation_name(enum mainui_presentation presentation) {
    switch (presentation) {
    case MAINUI_PRESENTATION_STOCK: return "stock";
    case MAINUI_PRESENTATION_MATERIAL_YOU: return "material-you";
    default: return "invalid";
    }
}
