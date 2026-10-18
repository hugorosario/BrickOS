#define _POSIX_C_SOURCE 200809L
#include "theme.h"
#include "../tmenu/presentation.h"
#include "../mainui/appearance.h"
#include "../mainui/json.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

// A profile is in use while an occupied slot carries it; a free slot is never drawn.
static int in_use(void *context, const struct osdd_profile *profile) {
    const struct osdd_toast_state *toasts = context;
    if (toasts == NULL) return 0;
    for (int index = 0; index < OSDD_TOAST_SLOTS; ++index)
        if (toasts->slots[index].added_ms != 0 && toasts->slots[index].value.profile == profile) return 1;
    return 0;
}

void osdd_theme_init(struct osdd_theme *theme, const char *path,
                     const struct osdd_profile_assets *assets,
                     const struct osdd_toast_state *toasts) {
    memset(theme, 0, sizeof(*theme));
    theme->toasts = toasts;
    theme->processes = "/proc";
    osdd_theme_appearance(theme, MAINUI_APPEARANCE_PATH, MAINUI_FONTS_DIR, "/usr/trimui/res");
    // The default look until the first check reads the Appearance file.
    theme->look = (struct osdd_look){.seed = {103, 80, 164}, .background = 1};
    snprintf(theme->look.font, sizeof(theme->look.font), "%s", OSDD_PROFILE_STOCK_FONT);
    snprintf(theme->look.fallback, sizeof(theme->look.fallback), "%s", OSDD_PROFILE_STOCK_FONT);
    osdd_profile_loader_init(&theme->loader, path, assets, in_use, (void *)toasts);
}

void osdd_theme_frontend(struct osdd_theme *theme, const char *preferences,
                         const char *current, const char *settings, const char *override) {
    theme->preferences = preferences;
    theme->current = current;
    theme->settings = settings;
    theme->override = override;
    theme->frontend_checked = 0;
}

void osdd_theme_appearance(struct osdd_theme *theme, const char *appearance, const char *fonts,
                           const char *system_fonts) {
    theme->appearance = appearance;
    theme->fonts = fonts;
    theme->system_fonts = system_fonts;
    theme->appearance_read = 0;
    theme->frontend_checked = 0;
}

void osdd_theme_processes(struct osdd_theme *theme, const char *processes) {
    theme->processes = processes;
    theme->frontend_checked = 0;
}

static size_t read_config(const char *path, char *out, size_t capacity) {
    struct stat st;
    if (!path || stat(path, &st) < 0 || !S_ISREG(st.st_mode) || st.st_size < 0 ||
        (size_t)st.st_size >= capacity) return 0;
    FILE *file = fopen(path, "rb");
    if (!file) return 0;
    size_t n = fread(out, 1, capacity - 1, file);
    int ok = !ferror(file) && feof(file);
    fclose(file);
    out[n] = 0;
    return ok ? n : 0;
}

struct source_settings { char language[64]; };
static int settings_member(void *context, const struct mainui_json_member *m) {
    struct source_settings *source = context;
    if (m->type == MAINUI_JSON_STRING && strlen(m->text) == m->text_length && !strcmp(m->key, "language") &&
        m->text_length < sizeof(source->language))
        memcpy(source->language, m->text, m->text_length + 1);
    return 0;
}

static int regular_file(const char *path) {
    struct stat st;
    return stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

// The BrickOS look. The Appearance file gives the accent, background, shape, halo and font, and
// is read again only when it changes; its rules for a missing or invalid file, or an unknown
// value, are MainUI's (appearance.c). The system font for the language, as MainUI picks it, draws
// the characters the Appearance font lacks, and everything when there is no Appearance font.
// The system language from the settings file, or "" without one.
static void settings_language(const struct osdd_theme *theme, char language[64]) {
    char text[8192];
    struct source_settings settings = {{0}};
    size_t n = read_config(theme->settings, text, sizeof(text));
    if (n) mainui_json_each(text, n, settings_member, &settings);
    memcpy(language, settings.language, sizeof(settings.language));
}

static void brickos_look(struct osdd_theme *theme, const char *language) {
    struct stat st;
    int present = theme->appearance && stat(theme->appearance, &st) == 0;
    if (!theme->appearance_read || present != theme->appearance_present ||
        (present && (st.st_mtim.tv_sec != theme->appearance_sec || st.st_mtim.tv_nsec != theme->appearance_nsec ||
                     st.st_size != theme->appearance_size || st.st_ino != theme->appearance_ino))) {
        static struct mainui_appearance appearance; // its member list is large
        if (!present || mainui_appearance_load(&appearance, theme->appearance) < 0) mainui_appearance_defaults(&appearance);
        struct mainui_rgb seed = mainui_appearance_seed(&appearance);
        theme->look.seed[0] = seed.r;
        theme->look.seed[1] = seed.g;
        theme->look.seed[2] = seed.b;
        theme->look.background = mainui_appearance_background_level(&appearance);
        theme->look.shape = mainui_appearance_get(&appearance, MAINUI_SET_APPEAR_SHAPE);
        theme->look.halo = mainui_appearance_get(&appearance, MAINUI_SET_APPEAR_FOCUS);
        snprintf(theme->appearance_font, sizeof(theme->appearance_font), "%s", appearance.font);
        theme->appearance_read = 1;
        theme->appearance_present = present;
        theme->appearance_sec = present ? (long long)st.st_mtim.tv_sec : 0;
        theme->appearance_nsec = present ? (long long)st.st_mtim.tv_nsec : 0;
        theme->appearance_size = present ? (long long)st.st_size : 0;
        theme->appearance_ino = present ? (unsigned long long)st.st_ino : 0;
    }
    char path[1280];
    // MainUI's rule: regular.ttf for these languages, full.ttf for the others.
    static const char *const regular[] = {"en.lang", "ch.lang", "es.lang", "pt.lang", "it.lang", "fr.lang", "de.lang"};
    int is_regular = !language[0];
    for (size_t i = 0; i < sizeof(regular) / sizeof(regular[0]); ++i) is_regular |= !strcmp(language, regular[i]);
    snprintf(path, sizeof(path), "%s/%s.ttf", theme->system_fonts, is_regular ? "regular" : "full");
    snprintf(theme->look.fallback, sizeof(theme->look.fallback), "%s",
             strlen(path) < sizeof(theme->look.fallback) && regular_file(path) ? path : OSDD_PROFILE_STOCK_FONT);
    snprintf(theme->look.font, sizeof(theme->look.font), "%s", theme->look.fallback);
    theme->look.portuguese = !strcmp(language, "pt.lang");
    if (theme->appearance_font[0] && theme->fonts) {
        snprintf(path, sizeof(path), "%s/%s", theme->fonts, theme->appearance_font);
        if (strlen(path) < sizeof(theme->look.font) && regular_file(path)) memcpy(theme->look.font, path, strlen(path) + 1);
    }
}

int osdd_theme_tick(struct osdd_theme *theme, uint64_t now_ms) {
    int changed = osdd_profile_loader_tick(&theme->loader, now_ms);
    if (theme->frontend_checked && now_ms >= theme->frontend_check_ms &&
        now_ms - theme->frontend_check_ms < OSDD_PROFILE_CHECK_MS) return changed;
    theme->frontend_checked = 1;
    theme->frontend_check_ms = now_ms;
    // The loader keeps watching the file, so a valid profile is ready when EmulationStation starts.
    theme->emulationstation = tmenu_emulationstation_running(theme->processes);
    if (!(theme->preferences || theme->current || theme->override)) return changed;
    // Reuse the same preference/default/override precedence as the in-game menu.
    theme->material = tmenu_material_selected(theme->preferences, theme->current, theme->override);
    char language[64];
    settings_language(theme, language);
    theme->portuguese = !strcmp(language, "pt.lang");
    if (theme->material) brickos_look(theme, language);
    return changed;
}

const struct osdd_profile *osdd_theme_profile(const struct osdd_theme *theme) {
    return theme->emulationstation && !theme->material ? theme->loader.current : NULL;
}

void osdd_theme_attach(const struct osdd_theme *theme, struct osdd_toast *toast) {
    toast->profile = osdd_theme_profile(theme);
    toast->material = theme->material;
    if (toast->material) toast->look = theme->look;
}

void osdd_theme_close(struct osdd_theme *theme) {
    osdd_profile_loader_close(&theme->loader);
}
