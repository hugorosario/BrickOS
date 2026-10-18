#define _POSIX_C_SOURCE 200809L
#include "render.h"
#include "../mainui/assets.h"

#include <stdio.h>
#include <string.h>

enum {
    MARGIN = 64,
    WIDTH = UNINSTALL_SCREEN_W - 2 * MARGIN,
    INDENT = 32,       // list items, after their dot
    BUTTON_W = 260,
    BUTTON_H = 64,
    BUTTON_Y = 618,
    BUTTON_GAP = 32,
    BAND = 4,          // the focus band around the focused button
    HINT_Y = 712,
    WRAP_LINES = 6,
    WRAP_BYTES = 512,
};

int uninstall_font(const char *root, const char *language, char *out, size_t size) {
    struct mainui_fs fs = {.root = root ? root : ""};
    char device[1024];
    // No theme: the Default theme's regular.ttf or full.ttf for the language.
    if (mainui_asset_font(&fs, NULL, NULL, language, device, sizeof(device)) < 0) return -1;
    return snprintf(out, size, "%s%s", fs.root, device) < (int)size ? 0 : -1;
}

int uninstall_renderer_open(struct uninstall_renderer *r, const char *font) {
    memset(r, 0, sizeof(*r));
    r->title = TTF_OpenFont(font, 40);
    r->body = TTF_OpenFont(font, 26);
    r->small = TTF_OpenFont(font, 22);
    if (!r->title || !r->body || !r->small) {
        uninstall_renderer_close(r);
        return -1;
    }
    // BrickOS's default accent, at the Dark background level.
    brickos_palette_build(&r->palette, (SDL_Color){103, 80, 164, 255}, 1.0);
    // The notice keeps the palette's tones in a red hue, as a warning.
    r->notice_background = material_tone((SDL_Color){214, 64, 69, 255}, 0.24, 0.45);
    r->notice_text = material_tone((SDL_Color){214, 64, 69, 255}, 0.88, 0.80);
    return 0;
}

void uninstall_renderer_close(struct uninstall_renderer *r) {
    if (r->title) TTF_CloseFont(r->title);
    if (r->body) TTF_CloseFont(r->body);
    if (r->small) TTF_CloseFont(r->small);
    memset(r, 0, sizeof(*r));
}

// ---- Drawing --------------------------------------------------------------------------------

static void fill(SDL_Surface *s, SDL_Rect rect, SDL_Color c) {
    SDL_FillRect(s, &rect, SDL_MapRGBA(s->format, c.r, c.g, c.b, c.a));
}

static void rounded(SDL_Surface *s, SDL_Rect r, int radius, SDL_Color c) {
    if (radius * 2 > r.h) radius = r.h / 2;
    for (int y = 0; y < r.h; ++y) {
        int dy = y < radius ? radius - 1 - y : y >= r.h - radius ? y - (r.h - radius) : 0, inset = 0;
        if (dy) {
            int dx = radius;
            while (dx > 0 && dx * dx + dy * dy > radius * radius) --dx;
            inset = radius - dx;
        }
        fill(s, (SDL_Rect){r.x + inset, r.y + y, r.w - 2 * inset, 1}, c);
    }
}

static void record(struct uninstall_layout *layout, SDL_Rect box, enum uninstall_line_kind kind) {
    if (!layout || layout->count >= UNINSTALL_LAYOUT_LINES) return;
    layout->lines[layout->count] = box;
    layout->kinds[layout->count++] = kind;
}

static int text_width(TTF_Font *font, const char *text) {
    int w = 0, h = 0;
    return text[0] && TTF_SizeUTF8(font, text, &w, &h) == 0 ? w : 0;
}

// Draws one line of text with its top left corner at (x, y), and records its box. center
// centers it on x instead.
static void line(SDL_Surface *s, TTF_Font *font, const char *text, int x, int y, SDL_Color color, int center,
                 struct uninstall_layout *layout, enum uninstall_line_kind kind) {
    if (!text[0]) return;
    SDL_Surface *glyphs = TTF_RenderUTF8_Blended(font, text, color);
    if (!glyphs) return;
    SDL_Rect box = {center ? x - glyphs->w / 2 : x, y, glyphs->w, glyphs->h};
    SDL_Rect to = box;
    SDL_BlitSurface(glyphs, NULL, s, &to);
    SDL_FreeSurface(glyphs);
    record(layout, box, kind);
}

// The byte length of the UTF-8 character at text, never past its end.
static size_t character(const char *text) {
    unsigned char lead = (unsigned char)text[0];
    size_t n = lead < 0x80 ? 1 : (lead & 0xe0) == 0xc0 ? 2 : (lead & 0xf0) == 0xe0 ? 3 : (lead & 0xf8) == 0xf0 ? 4 : 1;
    size_t i = 1;
    while (i < n && text[i]) ++i;
    return i;
}

// Ends a line with "...", removing whole characters from its end until it fits.
static void ellipsize(TTF_Font *font, char *text, int width) {
    char candidate[WRAP_BYTES + 4];
    size_t length = strlen(text);
    for (;;) {
        snprintf(candidate, sizeof(candidate), "%.*s...", (int)length, text);
        if (length == 0 || text_width(font, candidate) <= width) break;
        do --length; while (length > 0 && ((unsigned char)text[length] & 0xc0) == 0x80);
    }
    size_t n = strlen(candidate);
    if (n >= WRAP_BYTES) n = WRAP_BYTES - 1;
    memcpy(text, candidate, n);
    text[n] = 0;
}

// Breaks text into lines of at most width pixels, at spaces, or inside a word wider than a
// line. Past max lines, the last one ends with "..." and *shortened is set.
static int wrap(TTF_Font *font, const char *text, int width, char lines[][WRAP_BYTES], int max, int *shortened) {
    char candidate[WRAP_BYTES];
    int count = 0;
    const char *p = text;
    while (count < max) {
        while (*p == ' ') ++p;
        if (!*p) break;
        size_t best = 0, end = 0;
        // The most whole words that fit.
        for (;;) {
            size_t j = end;
            while (p[j] == ' ') ++j;
            if (!p[j]) break;
            while (p[j] && p[j] != ' ') ++j;
            if (j >= WRAP_BYTES) break;
            memcpy(candidate, p, j);
            candidate[j] = 0;
            if (text_width(font, candidate) > width) break;
            best = end = j;
        }
        // A word wider than the line: as many of its characters as fit, at least one.
        if (!best) {
            for (size_t j = 0; p[j] && p[j] != ' ';) {
                size_t next = j + character(p + j);
                if (next >= WRAP_BYTES) break;
                memcpy(candidate, p, next);
                candidate[next] = 0;
                if (text_width(font, candidate) > width) break;
                best = j = next;
            }
            if (!best) best = character(p);
        }
        memcpy(lines[count], p, best);
        lines[count++][best] = 0;
        p += best;
    }
    while (*p == ' ') ++p;
    if (*p && count) {
        ellipsize(font, lines[count - 1], width);
        if (shortened) *shortened = 1;
    }
    return count;
}

// A paragraph: wrapped lines from y, at x or centered on it. Returns the y below it.
static int paragraph(SDL_Surface *s, TTF_Font *font, const char *text, int x, int y, int width, SDL_Color color,
                     int center, struct uninstall_layout *layout, enum uninstall_line_kind kind) {
    char lines[WRAP_LINES][WRAP_BYTES];
    int n = wrap(font, text, width, lines, WRAP_LINES, layout ? &layout->shortened : NULL);
    int skip = TTF_FontLineSkip(font);
    for (int i = 0; i < n; ++i) line(s, font, lines[i], x, y + i * skip, color, center, layout, kind);
    return y + n * skip;
}

// A heading and its items, each item after a dot. Returns the y below it.
static int section(const struct uninstall_renderer *r, SDL_Surface *s, const struct uninstall_model *m,
                   enum uninstall_text heading, const enum uninstall_text *items, int count, int y,
                   struct uninstall_layout *layout) {
    line(s, r->small, uninstall_text(m, heading), MARGIN, y, r->palette.primary, 0, layout, UNINSTALL_LINE_HEADING);
    y += TTF_FontLineSkip(r->small) + 4;
    int dot = 10, skip = TTF_FontLineSkip(r->body);
    for (int i = 0; i < count; ++i) {
        rounded(s, (SDL_Rect){MARGIN + 6, y + (skip - dot) / 2, dot, dot}, dot / 2, r->palette.on_surface_variant);
        y = paragraph(s, r->body, uninstall_text(m, items[i]), MARGIN + INDENT, y, WIDTH - INDENT, r->palette.on_surface, 0,
                      layout, UNINSTALL_LINE_BODY) + 6;
    }
    return y;
}

// A button with its label centered; the focused one has the focus band around it.
static SDL_Rect button(const struct uninstall_renderer *r, SDL_Surface *s, SDL_Rect box, const char *label, int focused,
                       struct uninstall_layout *layout) {
    if (focused) {
        SDL_Rect band = {box.x - BAND, box.y - BAND, box.w + 2 * BAND, box.h + 2 * BAND};
        rounded(s, band, 20, r->palette.primary);
        if (layout) layout->focus = band;
    }
    rounded(s, box, 16, focused ? r->palette.primary_container : r->palette.surface_container);
    int h = TTF_FontHeight(r->body);
    line(s, r->body, label, box.x + box.w / 2, box.y + (box.h - h) / 2,
         focused ? r->palette.on_primary_container : r->palette.on_surface, 1, layout, UNINSTALL_LINE_CHOICE);
    return box;
}

static void confirmation(const struct uninstall_renderer *r, SDL_Surface *s, const struct uninstall_model *m,
                         struct uninstall_layout *layout) {
    int y = 40;
    line(s, r->title, uninstall_text(m, UNINSTALL_TEXT_TITLE), MARGIN, y, r->palette.on_surface, 0, layout,
         UNINSTALL_LINE_TITLE);
    y += TTF_FontLineSkip(r->title) + 14;
    char notice[1024];
    uninstall_notice(m, notice, sizeof(notice));
    if (notice[0]) {
        char lines[3][WRAP_BYTES];
        int n = wrap(r->small, notice, WIDTH - 8, lines, 3, layout ? &layout->shortened : NULL);
        int skip = TTF_FontLineSkip(r->small);
        SDL_Rect band = {MARGIN - 16, y, WIDTH + 32, n * skip + 24};
        rounded(s, band, 16, r->notice_background);
        for (int i = 0; i < n; ++i)
            line(s, r->small, lines[i], MARGIN + 4, y + 12 + i * skip, r->notice_text, 0, layout, UNINSTALL_LINE_NOTICE);
        if (layout) layout->notice = band;
        y += band.h + 20;
    }
    static const enum uninstall_text removes[] = {UNINSTALL_TEXT_REMOVES_COMPONENTS, UNINSTALL_TEXT_REMOVES_FOLDER};
    static const enum uninstall_text keeps[] = {UNINSTALL_TEXT_KEEPS_DATA, UNINSTALL_TEXT_KEEPS_IMAGES};
    y = section(r, s, m, UNINSTALL_TEXT_REMOVES, removes, 2, y, layout) + 12;
    y = section(r, s, m, UNINSTALL_TEXT_KEEPS, keeps, 2, y, layout) + 12;
    y = paragraph(s, r->body, uninstall_text(m, UNINSTALL_TEXT_RESTART), MARGIN, y, WIDTH, r->palette.on_surface_variant, 0,
                  layout, UNINSTALL_LINE_BODY);
    if (layout) layout->content = (SDL_Rect){MARGIN - 16, 40, WIDTH + 32, y - 40};
    SDL_Rect cancel = {UNINSTALL_SCREEN_W / 2 - BUTTON_GAP / 2 - BUTTON_W, BUTTON_Y, BUTTON_W, BUTTON_H};
    SDL_Rect uninstall = {UNINSTALL_SCREEN_W / 2 + BUTTON_GAP / 2, BUTTON_Y, BUTTON_W, BUTTON_H};
    button(r, s, cancel, uninstall_text(m, UNINSTALL_TEXT_CANCEL), m->focus == UNINSTALL_CHOICE_CANCEL, layout);
    button(r, s, uninstall, uninstall_text(m, UNINSTALL_TEXT_UNINSTALL), m->focus == UNINSTALL_CHOICE_UNINSTALL, layout);
    if (layout) {
        layout->cancel = cancel;
        layout->uninstall = uninstall;
    }
    line(s, r->small, uninstall_text(m, UNINSTALL_TEXT_HINT), UNINSTALL_SCREEN_W / 2, HINT_Y, r->palette.on_surface_variant,
         1, layout, UNINSTALL_LINE_HINT);
}

static void error_screen(const struct uninstall_renderer *r, SDL_Surface *s, const struct uninstall_model *m,
                         struct uninstall_layout *layout) {
    int y = 160;
    y = paragraph(s, r->title, uninstall_text(m, UNINSTALL_TEXT_ERROR_TITLE), MARGIN, y, WIDTH, r->palette.on_surface, 0,
                  layout, UNINSTALL_LINE_TITLE) + 24;
    y = paragraph(s, r->body, uninstall_error_text(m), MARGIN, y, WIDTH, r->palette.on_surface, 0, layout,
                  UNINSTALL_LINE_BODY) + 8;
    enum uninstall_text detail = m->error == UNINSTALL_ERROR_RESTART ? UNINSTALL_TEXT_ERROR_NEXT_START
                                                                     : UNINSTALL_TEXT_ERROR_UNCHANGED;
    y = paragraph(s, r->body, uninstall_text(m, detail), MARGIN, y, WIDTH, r->palette.on_surface_variant, 0, layout,
                  UNINSTALL_LINE_BODY);
    if (layout) layout->content = (SDL_Rect){MARGIN, 160, WIDTH, y - 160};
    SDL_Rect back = {(UNINSTALL_SCREEN_W - 2 * BUTTON_W) / 2, BUTTON_Y, 2 * BUTTON_W, BUTTON_H};
    button(r, s, back, uninstall_text(m, UNINSTALL_TEXT_BACK), 1, layout);
    if (layout) layout->back = back;
}

static void restarting(const struct uninstall_renderer *r, SDL_Surface *s, const struct uninstall_model *m,
                       struct uninstall_layout *layout) {
    int y = 300;
    y = paragraph(s, r->title, uninstall_text(m, UNINSTALL_TEXT_RESTARTING), UNINSTALL_SCREEN_W / 2, y, WIDTH,
                  r->palette.on_surface, 1, layout, UNINSTALL_LINE_TITLE) + 20;
    y = paragraph(s, r->body, uninstall_text(m, UNINSTALL_TEXT_RESTARTING_DETAIL), UNINSTALL_SCREEN_W / 2, y, WIDTH,
                  r->palette.on_surface_variant, 1, layout, UNINSTALL_LINE_BODY);
    if (layout) layout->content = (SDL_Rect){MARGIN, 300, WIDTH, y - 300};
}

void uninstall_render(const struct uninstall_renderer *r, SDL_Surface *screen, const struct uninstall_model *m,
                      struct uninstall_layout *layout) {
    if (layout) memset(layout, 0, sizeof(*layout));
    fill(screen, (SDL_Rect){0, 0, screen->w, screen->h}, r->palette.background);
    switch (m->screen) {
    case UNINSTALL_SCREEN_CONFIRM: confirmation(r, screen, m, layout); break;
    case UNINSTALL_SCREEN_ERROR: error_screen(r, screen, m, layout); break;
    case UNINSTALL_SCREEN_RESTARTING: restarting(r, screen, m, layout); break;
    }
}
