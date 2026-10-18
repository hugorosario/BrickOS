#define _POSIX_C_SOURCE 200809L
#include "appearance.h"

#include <stdio.h>
#include <string.h>

// One Appearance row: the member it is stored under, its options' stored names and labels,
// and its default.
struct option {
    const char *key;
    const char *label, *label_pt;
    int count, fallback;
    const char *const *names, *const *english, *const *portuguese;
};

static const char *const accent_names[MAINUI_ACCENTS] = {
    "violet", "indigo", "blue", "teal", "green", "lime", "amber", "orange", "red", "pink",
};
static const char *const accent_en[MAINUI_ACCENTS] = {
    "Violet", "Indigo", "Blue", "Teal", "Green", "Lime", "Amber", "Orange", "Red", "Pink",
};
static const char *const accent_pt[MAINUI_ACCENTS] = {
    "Violeta", "Índigo", "Azul", "Verde-azulado", "Verde", "Lima", "Âmbar", "Laranja", "Vermelho", "Rosa",
};
static const char *const background_names[MAINUI_BACKGROUNDS] = {"dark", "darker", "black"};
static const char *const background_en[MAINUI_BACKGROUNDS] = {"Dark", "Darker", "Black"};
static const char *const background_pt[MAINUI_BACKGROUNDS] = {"Escuro", "Mais escuro", "Preto"};
// Indexed by enum mainui_text_size and enum mainui_dimming, whose 0 is the default.
static const char *const text_names[MAINUI_TEXT_SIZES] = {"normal", "small", "large", "extra-large"};
static const char *const text_en[MAINUI_TEXT_SIZES] = {"Normal", "Small", "Large", "Extra large"};
static const char *const text_pt[MAINUI_TEXT_SIZES] = {"Normal", "Pequeno", "Grande", "Muito grande"};
static const char *const shape_names[MAINUI_SHAPES] = {"rounded", "subtle", "square"};
static const char *const shape_en[MAINUI_SHAPES] = {"Rounded", "Subtle", "Square"};
static const char *const shape_pt[MAINUI_SHAPES] = {"Arredondados", "Subtis", "Retos"};
static const char *const dimming_names[MAINUI_DIMMINGS] = {"soft", "none", "strong"};
static const char *const dimming_en[MAINUI_DIMMINGS] = {"Soft", "None", "Strong"};
static const char *const dimming_pt[MAINUI_DIMMINGS] = {"Suave", "Nenhum", "Forte"};
static const char *const halo_names[MAINUI_HALOS] = {"pulse", "steady", "outline"};
static const char *const halo_en[MAINUI_HALOS] = {"Pulse", "Steady", "Outline"};
static const char *const halo_pt[MAINUI_HALOS] = {"Pulsar", "Fixo", "Só contorno"};
static const char *const motion_names[MAINUI_MOTIONS] = {"normal", "fast", "off"};
static const char *const motion_en[MAINUI_MOTIONS] = {"Normal", "Fast", "Off"};
static const char *const motion_pt[MAINUI_MOTIONS] = {"Normais", "Rápidas", "Desligadas"};
static const char *const view_names[MAINUI_VIEWS] = {"list", "grid", "carousel"};
static const char *const view_en[MAINUI_VIEWS] = {"List", "Grid", "Carousel"};
static const char *const view_pt[MAINUI_VIEWS] = {"Lista", "Grelha", "Carrossel"};

static const struct option options[MAINUI_APPEAR_SETTINGS] = {
    {"accent", "Accent colour", "Cor de destaque", MAINUI_ACCENTS, MAINUI_ACCENT_VIOLET, accent_names, accent_en, accent_pt},
    {"background", "Background", "Fundo", MAINUI_BACKGROUNDS, MAINUI_BACKGROUND_DARK, background_names, background_en,
     background_pt},
    {"text_size", "Text size", "Tamanho do texto", MAINUI_TEXT_SIZES, MAINUI_TEXT_NORMAL, text_names, text_en, text_pt},
    {"shape", "Card shape", "Forma dos cartões", MAINUI_SHAPES, MAINUI_SHAPE_ROUNDED, shape_names, shape_en, shape_pt},
    {"wallpaper_dim", "Wallpaper dimming", "Escurecimento do wallpaper", MAINUI_DIMMINGS, MAINUI_DIM_SOFT, dimming_names,
     dimming_en, dimming_pt},
    {"focus", "Focus halo", "Halo de foco", MAINUI_HALOS, MAINUI_HALO_PULSE, halo_names, halo_en, halo_pt},
    {"animations", "Animations", "Animações", MAINUI_MOTIONS, MAINUI_MOTION_NORMAL, motion_names, motion_en, motion_pt},
    {"game_list_view", "Game lists", "Listas de jogos", MAINUI_VIEWS, MAINUI_VIEW_LIST, view_names, view_en, view_pt},
    {"recent_view", "Recent", "Recentes", MAINUI_VIEWS, MAINUI_VIEW_LIST, view_names, view_en, view_pt},
    {"favorites_view", "Favorites", "Favoritos", MAINUI_VIEWS, MAINUI_VIEW_LIST, view_names, view_en, view_pt},
};

// The hue each accent gives the palette (the palette keeps only a seed's hue). Violet is the
// seed BrickOS has always used. Teal, Green and Lime sit at hues 185, 139 and 107, where text on
// the focused row keeps a 4.5:1 contrast (around 70-100 and 165-180 it falls below).
static const struct mainui_rgb seeds[MAINUI_ACCENTS] = {
    {103, 80, 164}, {63, 81, 181}, {33, 150, 243}, {0, 138, 150}, {57, 200, 103},
    {88, 205, 57}, {255, 193, 7}, {255, 128, 0}, {244, 67, 54}, {233, 30, 99},
};

static const struct {
    const char *english, *portuguese;
} texts[] = {
    {"Appearance", "Aparência"},
    {"Font", "Fonte"},
    {"Default", "Predefinida"},
    {"Reset appearance", "Repor aparência"},
    {"Reset appearance?", "Repor a aparência?"},
    {"Every option returns to its default.", "Todas as opções voltam ao valor predefinido."},
    {"Appearance not saved", "Aparência não guardada"},
};

static const struct option *option_for(int setting) {
    return setting >= MAINUI_SET_APPEAR_ACCENT && setting <= MAINUI_SET_APPEAR_LAST ?
           &options[setting - MAINUI_SET_APPEAR_ACCENT] : NULL;
}

void mainui_appearance_defaults(struct mainui_appearance *appearance) {
    memset(appearance, 0, sizeof(*appearance));
    for (int i = 0; i < MAINUI_APPEAR_SETTINGS; ++i) appearance->values[i] = options[i].fallback;
}

int mainui_appearance_options(int setting) {
    const struct option *o = option_for(setting);
    return o ? o->count : 0;
}

int mainui_appearance_default(int setting) {
    const struct option *o = option_for(setting);
    return o ? o->fallback : 0;
}

int mainui_appearance_get(const struct mainui_appearance *appearance, int setting) {
    const struct option *o = option_for(setting);
    if (!o) return 0;
    int value = appearance->values[setting - MAINUI_SET_APPEAR_ACCENT];
    return value >= 0 && value < o->count ? value : o->fallback;
}

void mainui_appearance_set(struct mainui_appearance *appearance, int setting, int value) {
    const struct option *o = option_for(setting);
    if (o) appearance->values[setting - MAINUI_SET_APPEAR_ACCENT] = value >= 0 && value < o->count ? value : o->fallback;
}

const char *mainui_appearance_name(int setting, int value) {
    const struct option *o = option_for(setting);
    return o && value >= 0 && value < o->count ? o->names[value] : NULL;
}

const char *mainui_appearance_label(int setting, int portuguese) {
    const struct option *o = option_for(setting);
    return o ? portuguese ? o->label_pt : o->label : "";
}

const char *mainui_appearance_value_label(int setting, int value, int portuguese) {
    const struct option *o = option_for(setting);
    if (!o) return "";
    if (value < 0 || value >= o->count) value = o->fallback;
    return portuguese ? o->portuguese[value] : o->english[value];
}

const char *mainui_appearance_text(const char *english, int portuguese) {
    for (size_t i = 0; portuguese && english && i < sizeof(texts) / sizeof(texts[0]); ++i)
        if (strcmp(texts[i].english, english) == 0) return texts[i].portuguese;
    return english;
}

int mainui_appearance_font_name_valid(const char *name, size_t length) {
    return length > 0 && length < MAINUI_FONT_NAME && !memchr(name, '/', length) && !memchr(name, '\0', length) &&
           strcmp(name, ".") != 0 && strcmp(name, "..") != 0;
}

int mainui_appearance_load(struct mainui_appearance *appearance, const char *path) {
    mainui_appearance_defaults(appearance);
    FILE *file = fopen(path, "rb");
    if (!file) return 0;
    static char text[MAINUI_APPEARANCE_BYTES + 1];
    size_t length = fread(text, 1, sizeof(text), file);
    int failed = ferror(file);
    fclose(file);
    if (failed || length > MAINUI_APPEARANCE_BYTES) return -1;
    text[length] = 0;
    if (mainui_json_parse(&appearance->document, text, length) < 0) {
        memset(&appearance->document, 0, sizeof(appearance->document));
        return -1;
    }
    for (int i = 0; i < MAINUI_APPEAR_SETTINGS; ++i) {
        const struct mainui_json_member *m = mainui_json_last(&appearance->document, options[i].key);
        if (!m || m->type != MAINUI_JSON_STRING) continue;
        for (int value = 0; value < options[i].count; ++value)
            if (strcmp(m->text, options[i].names[value]) == 0) appearance->values[i] = value;
    }
    const struct mainui_json_member *font = mainui_json_last(&appearance->document, "font");
    if (font && font->type == MAINUI_JSON_STRING && mainui_appearance_font_name_valid(font->text, font->text_length))
        memcpy(appearance->font, font->text, font->text_length + 1);
    return 1;
}

struct output {
    char *text;
    size_t size, length;
    int failed;
};

static void append(struct output *out, const char *bytes, size_t length) {
    if (out->failed || out->length + length >= out->size) { out->failed = 1; return; }
    memcpy(out->text + out->length, bytes, length);
    out->length += length;
    out->text[out->length] = 0;
}

static void append_text(struct output *out, const char *text) { append(out, text, strlen(text)); }

// A JSON string, quotes included, with cJSON's escapes.
static void append_string(struct output *out, const char *bytes, size_t length) {
    if (out->failed) return;
    int written = mainui_json_escape(out->text + out->length, out->size - out->length, bytes, length);
    if (written < 0) { out->failed = 1; return; }
    out->length += (size_t)written;
}

int mainui_appearance_format(const struct mainui_appearance *appearance, char *text, size_t size, size_t *length) {
    if (!text || !length || !size) return -1;
    struct output out = {text, size, 0, 0};
    append_text(&out, "{\"version\":1");
    for (int i = 0; i < MAINUI_APPEAR_SETTINGS; ++i) {
        append_text(&out, ",\"");
        append_text(&out, options[i].key);
        append_text(&out, "\":\"");
        append_text(&out, options[i].names[mainui_appearance_get(appearance, MAINUI_SET_APPEAR_ACCENT + i)]);
        append_text(&out, "\"");
    }
    append_text(&out, ",\"font\":");
    append_string(&out, appearance->font, strlen(appearance->font));
    for (size_t i = 0; i < appearance->document.count; ++i) {
        const struct mainui_json_member *m = &appearance->document.members[i];
        int known = strcmp(m->key, "version") == 0 || strcmp(m->key, "font") == 0;
        for (int k = 0; k < MAINUI_APPEAR_SETTINGS && !known; ++k) known = strcmp(m->key, options[k].key) == 0;
        if (known) continue;
        append_text(&out, ",");
        append_string(&out, m->key, strlen(m->key));
        append_text(&out, ":");
        if (m->type == MAINUI_JSON_STRING) append_string(&out, m->text, m->text_length);
        else append(&out, m->text, m->text_length);
    }
    append_text(&out, "}\n");
    if (out.failed || out.length > MAINUI_APPEARANCE_BYTES) return -1;
    *length = out.length;
    return 0;
}

struct mainui_rgb mainui_appearance_seed(const struct mainui_appearance *appearance) {
    return seeds[mainui_appearance_get(appearance, MAINUI_SET_APPEAR_ACCENT)];
}

double mainui_appearance_background_level(const struct mainui_appearance *appearance) {
    static const double levels[MAINUI_BACKGROUNDS] = {1.0, 0.55, 0.0};
    return levels[mainui_appearance_get(appearance, MAINUI_SET_APPEAR_BACKGROUND)];
}

double mainui_appearance_text_scale(const struct mainui_appearance *appearance) {
    static const double scales[MAINUI_TEXT_SIZES] = {1.0, 0.88, 1.15, 1.3};
    return scales[mainui_appearance_get(appearance, MAINUI_SET_APPEAR_TEXT_SIZE)];
}

int mainui_appearance_corner(const struct mainui_appearance *appearance, int w, int h) {
    return mainui_appearance_shape_corner(mainui_appearance_get(appearance, MAINUI_SET_APPEAR_SHAPE), w, h);
}

int mainui_appearance_shape_corner(int shape, int w, int h) {
    switch (shape) {
    case MAINUI_SHAPE_SQUARE: return 0;
    case MAINUI_SHAPE_SUBTLE: {
        int radius = h / 10;
        if (radius > 10) radius = 10;
        return radius > w / 10 ? w / 10 : radius;
    }
    default: {
        int radius = h / 5;
        if (radius > 24) radius = 24;
        return radius > w / 5 ? w / 5 : radius;
    }
    }
}

struct mainui_dimming_values mainui_appearance_dimming(const struct mainui_appearance *appearance) {
    static const struct mainui_dimming_values values[MAINUI_DIMMINGS] = {
        {62, 28, 32, 3}, {0, 0, 32, 3}, {120, 44, 48, 4},
    };
    return values[mainui_appearance_get(appearance, MAINUI_SET_APPEAR_WALLPAPER)];
}

enum mainui_halo mainui_appearance_halo(const struct mainui_appearance *appearance) {
    return (enum mainui_halo)mainui_appearance_get(appearance, MAINUI_SET_APPEAR_FOCUS);
}

double mainui_appearance_motion(const struct mainui_appearance *appearance) {
    static const double scales[MAINUI_MOTIONS] = {1.0, 0.5, 0.0};
    return scales[mainui_appearance_get(appearance, MAINUI_SET_APPEAR_ANIMATIONS)];
}
