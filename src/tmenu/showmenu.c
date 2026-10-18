// BrickOS in-game menu: a replacement for the TrimUI Brick's stock libtmenu.so.
//
// RetroArch calls ShowMenu when GUIDE is pressed during a game, and the call lasts
// the whole menu. This library reproduces the official one's screens, input, drawing,
// and effects (docs/tmenu-abi.md). A call that does not match the observed RetroArch
// call is passed to the official library at its absolute path.
#define _GNU_SOURCE
#include "host.h"
#include "input.h"
#include "menu.h"
#include "render.h"
#include "system.h"
#include "theme.h"
#include "presentation.h"
#include "../mainui/brickos_prefs.h"
#include "../common/version.h"
#ifdef BRICKOS_GAMESWITCHER
BRICKOS_DECLARE_COMPONENT("gameswitcher", BRICKOS_VERSION_GAMESWITCHER);
#else
BRICKOS_DECLARE_COMPONENT("tmenu", BRICKOS_VERSION_TMENU);
#endif

#include "../osdd/profile.h"
#ifdef BRICKOS_GAMESWITCHER
#include "../gameswitcher/bridge.h"
#endif

#include <dlfcn.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#define OFFICIAL_LIBRARY "/usr/trimui/lib/libtmenu.so"
#define INPUT_DIRECTORY "/dev/input"

// The device harness's build reads its paths from the environment; the installed
// library always uses the firmware's.
#ifdef TMENU_TEST_PATHS
static const char *path_from_env(const char *name, const char *fallback) {
    const char *value = getenv(name);
    return value && *value ? value : fallback;
}
#else
#define path_from_env(name, fallback) (fallback)
#endif

typedef int (*show_menu_fn)(struct tmenu_host *, SDL_Surface *, SDL_Surface *, const char *, tmenu_present_fn);

// The hat values per device position, kept between calls as the official library does.
static struct tmenu_hats hats;

struct session {
    struct tmenu_host *host;
    SDL_Surface *background;
    struct tmenu_system system;
};

static void report(const char *format, const char *detail) {
    fprintf(stderr, "brickos-tmenu: ");
    fprintf(stderr, format, detail);
    fputc('\n', stderr);
}

static int delegate(struct tmenu_host *host, SDL_Surface *screen, SDL_Surface *background, const char *name,
                    tmenu_present_fn present) {
    void *official = dlopen(path_from_env("TMENU_TEST_OFFICIAL", OFFICIAL_LIBRARY), RTLD_NOW | RTLD_LOCAL);
    show_menu_fn show = official ? (show_menu_fn)dlsym(official, "ShowMenu") : NULL;
    if (!show) {
        report("official menu unavailable: %s", dlerror());
        return 0;
    }
    return show(host, screen, background, name, present);
}

static int op_saved_slots(void *c, int max, int *slots) {
    int n=((struct session *)c)->host->saved_slots(max, slots);
#ifdef BRICKOS_GAMESWITCHER
    if (gs_bridge_enabled() && n>=0 && n<=max) {
        int visible=0;
        for (int i=0;i<n;++i) if (slots[i]!=99) slots[visible++]=slots[i];
        n=visible;
    }
#endif
    return n;
}
static int op_save_state(void *c, int slot) { return ((struct session *)c)->host->save_state(slot); }
static int op_load_state(void *c, int slot) { return ((struct session *)c)->host->load_state(slot); }
static void op_quit(void *c) {
    struct session *s=c;
#ifdef BRICKOS_GAMESWITCHER
    if (gs_bridge_enabled()) { (void)gs_bridge_quit(s->host,s->background); return; }
#endif
    s->host->quit();
}
static void op_fullscreen(void *c) { ((struct session *)c)->host->fullscreen_toggle(); }
static int op_keyshm_get(void *c, int index) { return tmenu_system_keyshm_get(&((struct session *)c)->system, index); }
static void op_keyshm_set(void *c, int index, int value) {
    tmenu_system_keyshm_set(&((struct session *)c)->system, index, value);
}
// The Volume item is never visible with the stock RetroArch, so its mixer path
// is not reached (docs/tmenu-abi.md, "Display, audio, and other services").
static void op_set_volume(void *c, int volume) { (void)c; (void)volume; }
static void op_sleep_us(void *c, unsigned microseconds) { (void)c; usleep(microseconds); }
static void op_save_thumbnail(void *c, int slot) {
    struct session *s = c;
    char *path = s->host->thumb_path(slot);
    if (path) tmenu_render_thumbnail(s->background, path);
    free(path);
}
static void op_sync(void *c) { (void)c; sync(); }
// systemms: gettimeofday in milliseconds.
static uint64_t op_now_ms(void *c) {
    (void)c;
    struct timeval now;
    gettimeofday(&now, NULL);
    return (uint64_t)now.tv_sec * 1000u + (uint64_t)now.tv_usec / 1000u;
}

static struct tmenu_frame_info frame_info(const SDL_Surface *s) {
    if (!s) return (struct tmenu_frame_info){0};
    return (struct tmenu_frame_info){1, s->w, s->h, s->pitch, s->format ? s->format->format : 0, s->pixels != NULL};
}

static struct tmenu_host *current_host;
static char *thumb_path(int slot) { return current_host->thumb_path(slot); }

static int show_menu(struct tmenu_host *host, SDL_Surface *screen, SDL_Surface *background, const char *name,
             tmenu_present_fn present) {
#ifdef TMENU_CRASH_TEST
    // The session tool's crash case: a library that faults inside RetroArch.
    raise(SIGSEGV);
#endif
    char reason[160];
    struct tmenu_frame_info screen_info = frame_info(screen), background_info = frame_info(background);
    enum tmenu_call_check check =
        tmenu_call_check(host, &screen_info, &background_info, name, present, reason, sizeof(reason));
    if (check == TMENU_CALL_UNKNOWN_HOST) {
        report("unrecognized call, using the official menu: %s", reason);
        return delegate(host, screen, background, name, present);
    }
#ifdef BRICKOS_GAMESWITCHER
    if (check==TMENU_CALL_OK) {
        int bridge_result=0;
        if (gs_bridge_show(host,background,&bridge_result)) return bridge_result;
    }
#endif

    struct session session = {.host = host, .background = background};
    struct tmenu_system_paths paths = tmenu_system_default_paths;
    paths.system_json = path_from_env("TMENU_TEST_SYSTEM_JSON", paths.system_json);
    paths.res = path_from_env("TMENU_TEST_RES", paths.res);
    paths.rumble = path_from_env("TMENU_TEST_RUMBLE", paths.rumble);
    paths.color_dir = path_from_env("TMENU_TEST_COLOR_DIR", paths.color_dir);
    tmenu_system_open(&session.system, &paths);
    if (check == TMENU_CALL_BAD_ARGUMENTS) {
        report("invalid menu argument: %s", reason);
        tmenu_system_close(&session.system);
        return 0;
    }

    const char *language = tmenu_system_language();
    const struct tmenu_ops ops = {
        .context = &session, .saved_slots = op_saved_slots, .save_state = op_save_state,
        .load_state = op_load_state, .quit = op_quit,
        .fullscreen = host->fullscreen_toggle && host->fullscreen_state ? op_fullscreen : NULL,
        .keyshm_get = op_keyshm_get, .keyshm_set = op_keyshm_set, .set_volume = op_set_volume,
        .sleep_us = op_sleep_us, .save_thumbnail = op_save_thumbnail, .sync = op_sync, .now_ms = op_now_ms,
    };
    static struct tmenu_texts texts;
    static struct tmenu_menu menu;
    struct tmenu_assets assets;
    struct tmenu_input input = {.count = 0};
    const char *res = session.system.paths.res;
    int result = 0;

    // The official order: slots, fonts and images, strings, input, rumble.
    int manual_slots=host->slot_count;
#ifdef BRICKOS_GAMESWITCHER
    if (gs_bridge_enabled() && manual_slots>99) manual_slots=99;
#endif
    tmenu_menu_open(&menu, &ops, &texts.strings, manual_slots, ops.fullscreen != NULL, 0);
    menu.close_after_save = memcmp(host->rom_name, TMENU_ASYNC_SAVESTATE_MARKER,
                                   sizeof(TMENU_ASYNC_SAVESTATE_MARKER)) == 0;
    int material = tmenu_material_selected(path_from_env("TMENU_TEST_PREFERENCES", MAINUI_BRICKOS_PREFERENCES_PATH),
                         path_from_env("TMENU_TEST_CURRENT", TMENU_MAINUI_CURRENT), getenv("BRICKOS_TMENU_PRESENTATION"));
    menu.rows = material ? 6 : TMENU_ROWS;
    int (*open_assets)(struct tmenu_assets *, const char *, const char *, const char *, char *, size_t) =
        material ? tmenu_assets_open_material : tmenu_assets_open;
    if (open_assets(&assets, session.system.theme, res, tmenu_language_font(language), reason,
                          sizeof(reason)) < 0) {
        report("%s", reason);
        tmenu_system_close(&session.system);
        return 0;
    }
    assets.portuguese = !strcasecmp(language, "pt.lang");
    assets.english = !strcasecmp(language, "en.lang");
    // In Stock++, the OSD profile counts only while EmulationStation runs: it writes the profile
    // and leaves it behind, and it waits for the games it starts. BrickOS uses its own typeface
    // and visual identity whether EmulationStation runs or not.
    struct tmenu_theme theme = {0};
    if (!material && tmenu_emulationstation_running(path_from_env("TMENU_TEST_PROC", "/proc")) &&
        tmenu_theme_open(&theme, path_from_env("TMENU_TEST_PROFILE", OSDD_PROFILE_PATH), &tmenu_theme_sdl_assets,
                         reason, sizeof(reason)) == 0)
        tmenu_assets_theme(&assets, &theme);
    tmenu_texts_open(&texts, res, language);
    if (tmenu_input_open(&input, path_from_env("TMENU_TEST_INPUT", INPUT_DIRECTORY)) <= 0) {
        // The official library would wait here for input that cannot arrive.
        report("%s", "no input device under /dev/input; resuming the game");
        tmenu_input_close(&input);
        tmenu_theme_close(&theme, &tmenu_theme_sdl_assets);
        tmenu_assets_close(&assets);
        tmenu_system_close(&session.system);
        return 0;
    }
    tmenu_system_rumble_off(&session.system);
    if (host->fullscreen_state) menu.fullscreen_on = host->fullscreen_state();

    current_host = host;
    enum tmenu_result step;
    do {
        tmenu_render_frame(screen, background, &assets, &theme, &menu, thumb_path);
        present(screen);
        do step = tmenu_menu_key(&menu, tmenu_input_read(&input, &hats));
        while (step == TMENU_RESULT_NONE);
    } while (step == TMENU_RESULT_REDRAW);
    result = tmenu_menu_return_value(step);

    tmenu_input_close(&input);
    tmenu_theme_close(&theme, &tmenu_theme_sdl_assets);
    tmenu_assets_close(&assets);
    tmenu_system_close(&session.system);
    return result;
}

__attribute__((visibility("default")))
int ShowMenu(struct tmenu_host *host, SDL_Surface *screen, SDL_Surface *background, const char *name,
             tmenu_present_fn present) {
    int result=show_menu(host,screen,background,name,present);
#ifdef BRICKOS_GAMESWITCHER
    gs_bridge_legacy_finished();
#endif
    return result;
}
