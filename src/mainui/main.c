// BrickOS MainUI: the C/SDL2 replacement for the TrimUI Brick's official frontend.
//
//   MainUI                       the frontend, until a launch hands over to the init
//   MainUI --duration-ms N       a bounded diagnostic run of N ms (1000..600000)
//          [--dry-effects]       record effects instead of carrying them out
//          [--record FILE]       startup, input, frame, and exit records (default stdout)
//          [--root PATH]         prefix of every device path, for tests
//   MainUI --versions            the SDL libraries it was built against and loaded
//
// It exits 0 after handing a launch to the init (the official MainUI does too), after the
// diagnostic time, or when SDL asks it to quit, and 1 when it cannot start.
#define _POSIX_C_SOURCE 200809L
#include "audio.h"
#include "device.h"
#include "are-device.h"
#include "frontend.h"
#include "input.h"
#include "markers.h"
#include "music_picture.h"
#include "presentation.h"
#include "render.h"
#include "sound.h"
#include "../common/version.h"
BRICKOS_DECLARE_COMPONENT("mainui", BRICKOS_VERSION_MAINUI);

#include <SDL.h>
#include <SDL_image.h>
#include <SDL_mixer.h>
#include <SDL_ttf.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

static void print_version(const char *name, const SDL_version *built, const SDL_version *loaded) {
    printf("%s built=%u.%u.%u loaded=%u.%u.%u\n", name, built->major, built->minor, built->patch,
           loaded->major, loaded->minor, loaded->patch);
}

// The versions come from each library's own call, so every one of them must resolve at load.
static void print_versions(void) {
    SDL_version built, loaded;
    SDL_VERSION(&built);
    SDL_GetVersion(&loaded);
    print_version("SDL2", &built, &loaded);
    SDL_IMAGE_VERSION(&built);
    print_version("SDL2_image", &built, IMG_Linked_Version());
    SDL_TTF_VERSION(&built);
    print_version("SDL2_ttf", &built, TTF_Linked_Version());
    SDL_MIXER_VERSION(&built);
    print_version("SDL2_mixer", &built, Mix_Linked_Version());
}

// Runs a shell command with the firmware libraries ahead of any inherited SD library path.
static int run_command(void *context, const char *command) {
    (void)context;
    char line[2048];
    if (snprintf(line, sizeof(line), "LD_LIBRARY_PATH=/usr/trimui/lib:/usr/lib %s", command) >= (int)sizeof(line)) return -1;
    return system(line) == 0 ? 0 : -1;
}

// Runs a shell command the same way and keeps what it prints (wpa_cli queries).
static int capture_command(void *context, const char *command, char *out, size_t size) {
    (void)context;
    char line[2048], rest[256];
    out[0] = 0;
    if (snprintf(line, sizeof(line), "LD_LIBRARY_PATH=/usr/trimui/lib:/usr/lib %s 2>/dev/null", command) >= (int)sizeof(line)) return -1;
    FILE *pipe = popen(line, "r");
    if (!pipe) return -1;
    size_t n = fread(out, 1, size - 1, pipe);
    out[n] = 0;
    while (fread(rest, 1, sizeof(rest), pipe) > 0) {}
    return pclose(pipe) == -1 ? -1 : 0;
}

static void bgm_volume(void *context, int level) {
    mainui_audio_apply_volume(context, level);
}

static void theme_audio(void *context, const struct mainui_fs *fs, const char *theme) {
    mainui_audio_theme(context, fs, theme);
}

static int open_descriptors(void) {
    DIR *dir = opendir("/proc/self/fd");
    if (!dir) return -1;
    int n = 0;
    struct dirent *entry;
    while ((entry = readdir(dir))) n += entry->d_name[0] != '.';
    closedir(dir);
    return n - 1;  // the directory itself
}

// A joystick and the evdev code of each SDL button (SDL numbers buttons by code).
struct controller {
    SDL_Joystick *joystick;
    SDL_JoystickID id;
    int codes[64];
    int count;
    struct mainui_input input;
    // Buttons held when GameSwitcher's picker ended: each acts only once pressed again.
    Uint64 stale_up;
};

#define CONTROLLERS 8
// A frame that spent more than this many milliseconds decoding pictures stalled the motion it
// showed (see the loop).
#define FRAME_STALL_MS 20

static void map_buttons(struct controller *c) {
    // Without the evdev node, TRIMUI Player1's layout: B A Y X L1 R1 SELECT START MENU F1 F2.
    static const int layout[] = {304, 305, 307, 308, 310, 311, 314, 315, 316, 317, 318, 0, 0, 114, 115};
    c->count = (int)(sizeof(layout) / sizeof(layout[0]));
    memcpy(c->codes, layout, sizeof(layout));
#if SDL_VERSION_ATLEAST(2, 24, 0)
    const char *path = SDL_JoystickPath(c->joystick);
    int fd = path ? open(path, O_RDONLY | O_CLOEXEC) : -1;
    if (fd < 0) return;
    unsigned long bits[(KEY_MAX + 1) / (8 * sizeof(unsigned long)) + 1];
    memset(bits, 0, sizeof(bits));
    if (ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(bits)), bits) >= 0)
        c->count = mainui_input_sdl_buttons(bits, (int)(sizeof(bits) / sizeof(bits[0])), c->codes, 64);
    close(fd);
#endif
}

static void open_controllers(struct controller *controllers, FILE *record, uint64_t repeat_epoch) {
    for (int i = 0; i < SDL_NumJoysticks(); ++i) {
        SDL_JoystickID id = SDL_JoystickGetDeviceInstanceID(i);
        int open = 0, free_slot = -1;
        for (int c = 0; c < CONTROLLERS; ++c) {
            if (controllers[c].joystick && controllers[c].id == id) open = 1;
            // Retired controllers keep their already-produced release actions until drained.
            if (!controllers[c].joystick && !controllers[c].input.count && free_slot < 0) free_slot = c;
        }
        if (open || free_slot < 0) continue;
        struct controller *c = &controllers[free_slot];
        c->joystick = SDL_JoystickOpen(i);
        if (!c->joystick) continue;
        c->id = SDL_JoystickInstanceID(c->joystick);
        mainui_input_init(&c->input);
        mainui_input_repeat_clock(&c->input, repeat_epoch);
        map_buttons(c);
        fprintf(record, "controller added id=%d buttons=%d\n", (int)c->id, c->count);
    }
}

static struct controller *controller_for(struct controller *controllers, SDL_JoystickID id) {
    for (int c = 0; c < CONTROLLERS; ++c)
        if (controllers[c].joystick && controllers[c].id == id) return &controllers[c];
    return NULL;
}

static int update_io_controls(struct mainui_frontend *fe, const struct controller *controllers,
                              const struct mainui_input *keyboard) {
    const struct mainui_window *w = mainui_model_top(&fe->model);
    uint32_t held = 0;
    if (w->kind == MAINUI_WIN_SETTINGS && w->screen == MAINUI_SCREEN_IO_TEST) {
        held = mainui_input_test_held(keyboard);
        for (int c = 0; c < CONTROLLERS; ++c)
            if (controllers[c].joystick) held |= mainui_input_test_held(&controllers[c].input);
    }
    return mainui_frontend_io(fe, held);
}

static void discard_osd_input(struct controller *controllers, uint64_t now) {
    enum mainui_key key;
    for (int c = 0; c < CONTROLLERS; ++c)
        if (controllers[c].joystick || controllers[c].input.count)
            mainui_input_next_visible(&controllers[c].input, now, 1, &key);
}

static void read_input_gate(struct mainui_input_gate *gate, struct controller *controllers,
                            const char *root, FILE *record, uint64_t now) {
    int changed = mainui_input_gate_read(gate, root);
    if (changed & MAINUI_INPUT_GRAB_CHANGED)
        for (int c = 0; c < CONTROLLERS; ++c) mainui_input_reset_system(&controllers[c].input);
    if (gate->osd) discard_osd_input(controllers, now);
    if (changed) fprintf(record, "input gate grab=%d osd=%d ms=%llu\n", gate->grab, gate->osd, (unsigned long long)now);
}

static const char *key_name(enum mainui_key key) {
    static const char *const names[MAINUI_KEY_COUNT] = {
        "up", "down", "left", "right", "a", "b", "x", "y", "l1", "r1", "l2", "r2", "start", "select", "menu", "menu+start",
    };
    return (unsigned)key < MAINUI_KEY_COUNT ? names[key] : "?";
}

static void interrupt_repeats(struct controller *controllers, uint64_t *epoch, uint64_t now) {
    *epoch = now;
    for (int source = 0; source < CONTROLLERS; ++source)
        mainui_input_interrupt_repeat(&controllers[source].input, now);
}

static void input_sound(struct mainui_frontend *fe, struct mainui_audio *audio,
                        const SDL_Event *event, int osd) {
    const struct mainui_window *w = mainui_model_top(&fe->model);
    if (w->kind == MAINUI_WIN_SETTINGS && w->screen == MAINUI_SCREEN_IO_TEST) return;
    enum mainui_sound_input source = event->type == SDL_JOYBUTTONDOWN ? MAINUI_SOUND_INPUT_BUTTON_DOWN :
                                    event->type == SDL_JOYHATMOTION ? MAINUI_SOUND_INPUT_HAT : MAINUI_SOUND_INPUT_OTHER;
    int value = event->type == SDL_JOYHATMOTION ? event->jhat.value : 0;
    // Before the sound opens (audio is NULL) the click is not played.
    if (audio && !osd && mainui_sound_input_click(source, value, fe->settings.values[MAINUI_SET_ENABLEKEYTONE]))
        mainui_audio_click(audio);
}

// The view a key leaves: the top window and its focus, for the end-to-end plans
// (docs/system-e2e.md, Plans). Kinds by name; screens, dialogs and popups by their enum values.
static void record_view(const struct mainui_frontend *fe, FILE *record, uint64_t now) {
    static const char *const kinds[] = {"home", "tabs", "roms", "popup", "dialog", "settings", "keyboard",
                                        "search", "edit", "progress", "notice", "files", "text"};
    const struct mainui_window *top = mainui_model_top(&fe->model);
    const char *kind = (unsigned)top->kind < sizeof(kinds) / sizeof(kinds[0]) ? kinds[top->kind] : "other";
    int page = (unsigned)fe->model.tab < MAINUI_TAB_COUNT ? fe->model.tabs[fe->model.tab].focus : -1;
    fprintf(record, "view depth=%d kind=%s screen=%d dialog=%d popup=%d focus=%d first=%d system=%d tab=%d bar=%d "
            "page=%d home=%d search=%d ms=%llu",
            fe->model.depth, kind, (int)top->screen, (int)top->dialog, (int)top->popup, top->focus, top->first,
            top->system, (int)fe->model.tab, fe->model.bar_focused, page, fe->model.home_focus, fe->model.home_search,
            (unsigned long long)now);
    // The focused item ends the line, spaces and all: a game list's file, the Games page's
    // system folder, or the Apps page's app folder.
    const struct mainui_system *system = top->kind == MAINUI_WIN_TABS && fe->model.tab == MAINUI_TAB_GAMES &&
        !fe->model.bar_focused ? mainui_frontend_system(fe, page) : NULL;
    if (top->kind == MAINUI_WIN_ROMS && fe->games && top->system == fe->games_system && top->focus >= 0 &&
        top->focus < fe->game_count)
        fprintf(record, " item=%s", fe->games[top->focus].name);
    else if (system)
        fprintf(record, " item=%s", system->folder);
    else if (top->kind == MAINUI_WIN_TABS && fe->model.tab == MAINUI_TAB_APPS && !fe->model.bar_focused && page >= 0 &&
             page < fe->shown_app_count) {
        // An app's folder name, such as GameSwitcher, whose entry opens its picker over MainUI.
        const char *directory = fe->apps[fe->shown_apps[page]].directory, *name = strrchr(directory, '/');
        fprintf(record, " item=%s", name ? name + 1 : directory);
    }
    fputc('\n', record);
    fflush(record);
}

static int dispatch_inputs(struct mainui_frontend *fe, struct mainui_audio *audio,
                           struct controller *controllers, struct mainui_input_gate *gate,
                           FILE *record, uint64_t now, int *keys) {
    int dirty = 0;
    for (int source = 0; !fe->exit_requested && source < CONTROLLERS; ++source) {
        if (!controllers[source].joystick && !controllers[source].input.count) continue;
        enum mainui_key key;
        int repeated;
        while (!fe->exit_requested) {
            read_input_gate(gate, controllers, fe->fs.root, record, now);
            if (!mainui_input_next_event(&controllers[source].input, now, &key, &repeated)) break;
            if (gate->osd) continue;
            const struct mainui_window *front = mainui_model_top(&fe->model);
            // KeyTestWindow2 exits on START-up while MENU is still held. MENU-up
            // first clears that condition; ordinary window chords retain their
            // own release policy. Dispatch is per SDL event, before the next poll.
            if (front->kind == MAINUI_WIN_SETTINGS && front->screen == MAINUI_SCREEN_IO_TEST &&
                key == MAINUI_KEY_MENU_START && !controllers[source].input.menu) continue;
            ++*keys;
            int secret = front->kind == MAINUI_WIN_KEYBOARD &&
                         (front->purpose == MAINUI_TYPE_WIFI_PASSWORD || front->purpose == MAINUI_TYPE_HOTSPOT_PASSWORD);
            fprintf(record, "key name=%s ms=%llu\n", secret ? "hidden" : key_name(key), (unsigned long long)now);
            // The end-to-end probe reads key lines while MainUI runs.
            fflush(record);
            // Raw events already clicked before dispatch. Only generated repeats click here.
            if (audio && !(front->kind == MAINUI_WIN_SETTINGS && front->screen == MAINUI_SCREEN_IO_TEST) &&
                mainui_sound_input_click(repeated ? MAINUI_SOUND_INPUT_REPEAT : MAINUI_SOUND_INPUT_OTHER, 0,
                                        fe->settings.values[MAINUI_SET_ENABLEKEYTONE])) mainui_audio_click(audio);
            dirty |= mainui_frontend_key_at(fe, key, now);
            record_view(fe, record, now);
        }
    }
    return dirty;
}

static const char *audio_route(const struct mainui_frontend *fe);

// The POWER release that wakes the console opens the mixer again (static: the official
// wait_event removes keymon's marker, then reopen_audio). audio is NULL before the sound opens.
static void wake_up_audio(struct mainui_frontend *fe, struct mainui_audio *audio, const SDL_Event *event, int dry,
                          FILE *record, uint64_t now) {
    if (event->type != SDL_KEYUP || event->key.keysym.sym != SDLK_POWER || access("/tmp/system_suspended_once", F_OK) != 0)
        return;
    if (!dry) unlink("/tmp/system_suspended_once");
    fprintf(record, "audio wake-up ms=%llu\n", (unsigned long long)now);
    if (audio) mainui_audio_reopen(audio, audio->route, fe->settings.values[MAINUI_SET_VOL]);
}

// The sound opens once its route is known, on the connected Bluetooth audio device the manager
// lists or on the speaker. The official opens it before its first frame, after waiting up to
// 1.5 s for that list; BrickOS draws and answers meanwhile. The route is chosen once: opening on
// the speaker and switching a second later can leave SDL's speaker thread stuck in its write,
// and closing it then never returns. The menu sounds follow the frontend: the Default theme's
// in BrickOS, the stock theme's in Stock++.
static void open_sound(struct mainui_frontend *fe, struct mainui_audio *audio, FILE *record, uint64_t now) {
    mainui_audio_open(audio, &fe->fs, mainui_frontend_look_theme(fe), audio_route(fe), fe->settings.values[MAINUI_SET_BGMVOL]);
    fprintf(record, "start sound route=%s ms=%llu\n", audio->route[0] ? "bluealsa" : "speaker", (unsigned long long)now);
    fe->bgm_context = audio;
    fe->bgm_volume = bgm_volume;
    fe->theme_context = audio;
    fe->theme_audio = theme_audio;
}

// The connected Bluetooth audio device the sound goes to, "" for the speaker.
static const char *audio_route(const struct mainui_frontend *fe) {
    const struct mainui_bt_device *device = mainui_frontend_audio_device(fe);
    return device ? device->addr : "";
}

static void read_status(struct mainui_status *status) {
    time_t now = time(NULL);
    struct tm local;
    localtime_r(&now, &local);
    snprintf(status->clock, sizeof(status->clock), "%02d:%02d", local.tm_hour, local.tm_min);
    FILE *f = fopen("/sys/class/power_supply/axp2202-battery/capacity", "r");
    if (f) {
        if (fscanf(f, "%d", &status->battery) != 1) status->battery = 100;
        fclose(f);
    }
    // The official bar shows the bolt while the battery reads "Charging" or USB power is in, so
    // also with the battery "Full" (static: draw_battery_icon).
    char line[32] = "";
    status->charging = 0;
    f = fopen("/sys/class/power_supply/axp2202-battery/status", "r");
    if (f) {
        if (fgets(line, sizeof(line), f) && strncmp(line, "Charging", 8) == 0) status->charging = 1;
        fclose(f);
    }
    f = fopen("/sys/class/power_supply/axp2202-usb/online", "r");
    if (f) {
        if (fgets(line, sizeof(line), f) && line[0] == '1') status->charging = 1;
        fclose(f);
    }
}

int main(int argc, char **argv) {
    BRICKOS_HANDLE_VERSION(argc, argv);
    long duration_ms = 0;
    int dry = 0;
    enum mainui_presentation presentation = MAINUI_PRESENTATION_STOCK;
    int presentation_explicit = 0;
    const char *record_path = NULL, *root = "";
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--versions") == 0) { print_versions(); return 0; }
        else if (strcmp(argv[i], "--duration-ms") == 0 && i + 1 < argc) {
            const char *value = argv[++i];
            char *end;
            errno = 0;
            duration_ms = strtol(value, &end, 10);
            if (!*value || strspn(value, "0123456789") != strlen(value) || *end || errno == ERANGE ||
                duration_ms < 1000 || duration_ms > 600000) {
                fprintf(stderr, "--duration-ms must be 1000..600000\n");
                return 2;
            }
        }
        else if (strcmp(argv[i], "--dry-effects") == 0) dry = 1;
        else if ((strcmp(argv[i], "--presentation") == 0 || strcmp(argv[i], "--default-presentation") == 0) && i + 1 < argc) {
            int explicit_choice = strcmp(argv[i], "--presentation") == 0;
            enum mainui_presentation parsed;
            if (mainui_presentation_parse(argv[++i], &parsed) < 0) {
                fprintf(stderr, "--presentation must be stock or material-you\n");
                return 2;
            }
            if (explicit_choice || !presentation_explicit) presentation = parsed;
            presentation_explicit |= explicit_choice;
        }
        else if (strcmp(argv[i], "--record") == 0 && i + 1 < argc) record_path = argv[++i];
        else if (strcmp(argv[i], "--root") == 0 && i + 1 < argc) root = argv[++i];
        else {
            fprintf(stderr, "usage: MainUI [--presentation stock|material-you] [--default-presentation stock|material-you] [--duration-ms N] [--dry-effects] [--record FILE] [--root PATH] | --versions\n");
            return 2;
        }
    }
    FILE *record = record_path ? fopen(record_path, "w") : stdout;
    if (!record) { perror(record_path); return 1; }
    int descriptors = open_descriptors();
    Uint64 start = SDL_GetPerformanceCounter(), frequency = SDL_GetPerformanceFrequency();
#define NOW_MS() ((uint64_t)((SDL_GetPerformanceCounter() - start) * 1000 / frequency))
    fprintf(record, "start pid=%d presentation=%s duration_ms=%ld dry=%d fds=%d\n", (int)getpid(),
            mainui_presentation_name(presentation), duration_ms, dry, descriptors);

    static struct mainui_frontend fe;
    static struct mainui_device device;
    static struct mainui_are_device are_device;
    static struct mainui_renderer renderer;
    static struct mainui_render_early early;
    static struct mainui_audio audio;
    struct controller controllers[CONTROLLERS] = {0};
    SDL_Window *window = NULL;
    SDL_Renderer *sdl = NULL;
    int result = 1, clean = 0, audio_started = 0;
    device.disp_fd = -1;
    if (mainui_frontend_open(&fe, root, dry, record) < 0) {
        fprintf(record, "exit reason=no-catalog ms=%llu\n", (unsigned long long)NOW_MS());
        goto cleanup;
    }
    presentation = mainui_brickos_preferences_presentation(&fe.brickos_preferences, presentation, presentation_explicit);
    fprintf(record, "frontend selected presentation=%s explicit=%d\n", mainui_presentation_name(presentation), presentation_explicit);
    mainui_frontend_set_presentation(&fe, presentation);
    if (!dry) {
        mainui_device_open(&device);
        mainui_device_ops(&device, &fe.ops);
        if (!root || !root[0]) {
            const char *stock_elf = getenv("BRICKOS_MAINUI_STOCK");
            if (!stock_elf || !stock_elf[0]) stock_elf = "/usr/trimui/bin/MainUI";
            if (mainui_are_device_encoder(&are_device, stock_elf, &fe.encoder))
                fprintf(record, "ARE adapter unavailable\n");
        }
        fe.runner.run = run_command;
    }
    // A dry run keeps the read-only queries: the frontend asks no scan or change through them.
    fe.runner.capture = capture_command;
    mainui_frontend_start(&fe);
    mainui_frontend_count_games(&fe, 1);
    fprintf(record, "frontend systems=%d apps=%d recent=%d favorites=%d names=%d ms=%llu\n", fe.visible_count, fe.shown_app_count,
            fe.recent_count, fe.favorites_count, fe.names.lookup != NULL, (unsigned long long)NOW_MS());
    // Each start step's end, so a slower start shows where (docs/mainui.md, Start).
    fprintf(record, "start catalog ms=%llu\n", (unsigned long long)NOW_MS());
    // The sound needs the Bluetooth manager's device list for its route. The list is asked for
    // now, so the answer comes while SDL starts and the first frame draws.
    int bluetooth_wait = !dry && fe.settings.values[MAINUI_SET_BLUETOOTH];
    uint64_t bluetooth_asked = NOW_MS();
    if (bluetooth_wait) mainui_frontend_tick(&fe, bluetooth_asked);
    // The desktop's first wallpaper decodes on a thread of its own while SDL starts.
    IMG_Init(IMG_INIT_PNG | IMG_INIT_JPG);
    mainui_render_early_start(&early, &fe);
    // alsa-lib's own PCM lock deadlocks with the bluealsa plugin under SDL: the audio thread waits
    // on it inside its write, opening or closing never returns, and nothing reaches the earbuds,
    // as with the official MainUI. SDL uses each PCM from one thread, so the lock is not needed.
    setenv("LIBASOUND_THREAD_SAFE", "0", 0);
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_JOYSTICK | SDL_INIT_EVENTS | SDL_INIT_AUDIO) != 0) {
        fprintf(record, "exit reason=sdl-init error=%s\n", SDL_GetError());
        goto cleanup;
    }
    // The official's joystick hints, set after SDL_Init and before any joystick opens.
    SDL_SetHint("SDL_ACCELEROMETER_AS_JOYSTICK", "0");
    SDL_SetHint("SDL_JOYSTICK_HIDAPI_PS4_RUMBLE", "1");
    SDL_SetHint("SDL_JOYSTICK_HIDAPI_PS5_RUMBLE", "1");
    SDL_SetHint("SDL_JOYSTICK_HIDAPI_STEAM", "1");
    SDL_SetHint("SDL_JOYSTICK_ROG_CHAKRAM", "1");
    SDL_SetHint("SDL_JOYSTICK_ALLOW_BACKGROUND_EVENTS", "1");
    SDL_SetHint("SDL_LINUX_JOYSTICK_DEADZONES", "1");
    // The idle music picture for the official OSD is written on a worker, so only once SDL_image
    // is set up.
    mainui_frontend_music_writer(&fe, mainui_music_picture_write);
    if (TTF_Init() < 0) {
        fprintf(record, "exit reason=font-init error=%s\n", TTF_GetError());
        goto cleanup;
    }
    fprintf(record, "start sdl ms=%llu\n", (unsigned long long)NOW_MS());
    // The menu sounds and the music (task 5.3) open after the first frame, see open_sound.
    audio.dry = dry;
    audio.record = record;
    if (!dry) {
        audio.run = run_command;
        audio.capture = capture_command;
    }
    int volume_errors = 0;

    // Creating the window clears the framebuffer on screen. The picture it clears is written
    // back: the last frame of the MainUI before, of a game, or the boot splash stays until this
    // run's first frame. Before, a return from GameSwitcher to the Home showed black for about
    // two seconds.
    struct mainui_screen_copy screen;
    int screen_saved = !dry && mainui_device_screen_save(&screen) == 0;
    if (!dry && !screen_saved) fprintf(record, "screen copy unavailable\n");
    if (!dry && mainui_device_ui_layer(&device) < 0) fprintf(record, "ui layer reset failed\n");
    window = SDL_CreateWindow("MainUI", 0, 0, MAINUI_SCREEN_W, MAINUI_SCREEN_H, SDL_WINDOW_SHOWN);
    if (screen_saved)
        fprintf(record, "screen kept=%d ms=%llu\n", mainui_device_screen_restore(&screen) == 0, (unsigned long long)NOW_MS());
    fprintf(record, "start window ms=%llu\n", (unsigned long long)NOW_MS());
    // The official's renderer (static: SDL_CreateRenderer with flags 6), whose blending rounds:
    // the software one ends a level or two darker under a skin drawn with alpha.
    sdl = window ? SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC) : NULL;
    // BrickOS opens on the Default theme's fonts and sizes; Stock++ on the stock theme's.
    const char *look_theme = mainui_frontend_look_theme(&fe), *theme_font = "";
    for (int i = 0; i < fe.theme_count; ++i)
        if (strcmp(fe.themes[i].path, look_theme) == 0) theme_font = fe.themes[i].font;
    if (!sdl || mainui_render_open(&renderer, sdl, &fe.fs, look_theme, theme_font, fe.settings.language) < 0) {
        fprintf(record, "exit reason=video error=%s\n", SDL_GetError());
        goto cleanup;
    }
    renderer.presentation = presentation;
    renderer.animations_enabled = 1;
    renderer.home_cache_enabled = 1;
    mainui_render_early_take(&renderer, &early);
    const char *driver = SDL_GetCurrentVideoDriver();
    fprintf(record, "sdl video=%s ms=%llu\n", driver ? driver : "none", (unsigned long long)NOW_MS());
    fprintf(record, "start renderer ms=%llu\n", (unsigned long long)NOW_MS());
    uint64_t repeat_epoch = NOW_MS();
    open_controllers(controllers, record, repeat_epoch);
    struct mainui_input input;
    mainui_input_init(&input);
    struct mainui_input_gate input_gate = {0};

    read_status(&renderer.status);
    mainui_render_frame_at(&renderer, &fe, NOW_MS());
    SDL_RenderPresent(sdl);
    fprintf(record, "first-frame ms=%llu\n", (unsigned long long)NOW_MS());
    record_view(&fe, record, NOW_MS());
    // The wrapper's health check: this run rendered.
    if (mainui_marker_write(MAINUI_MARKER_READY) < 0) fprintf(record, "marker ready failed\n");
    // This boot's images have been shown: the first MainUI of the boot writes the next boot's
    // for a random Bootlogo or Splashscreen, on a worker that close waits for.
    if (mainui_frontend_boot_rotation(&fe)) fprintf(record, "boot rotation started ms=%llu\n", (unsigned long long)NOW_MS());
    // Without Bluetooth the route is the speaker, known now.
    if (!bluetooth_wait) {
        open_sound(&fe, &audio, record, NOW_MS());
        audio_started = 1;
    }
    fflush(record);

    const char *reason = "quit";
    int frames = 1, keys = 0, running = 1, dirty = 0;
    uint64_t last_status = NOW_MS(), last_sync = last_status, last_tick = last_status, last_frame = last_status;
    // SDL gives up on an output it cannot write to; the sound opens again on the same route.
    uint64_t audio_retry_at = 0, audio_lost_at = 0;
    int audio_retries = 0;
    // A quit while GameSwitcher's picker runs: when the picker was asked to end.
    int picker_quit = 0, picker_cancelled = 0;
    uint64_t picker_quit_ms = 0;
    while (running) {
        uint64_t now = NOW_MS();
        if (duration_ms && now >= (uint64_t)duration_ms) { reason = "timeout"; break; }
        // With Bluetooth on, the sound opens once the device list is known, or 1.5 s after it
        // was asked for, on the route it gives. Until then the clicks are not played.
        if (!audio_started && (fe.bt_devices_known || now >= bluetooth_asked + 1500)) {
            fprintf(record, "bluetooth devices %s before audio ms=%llu\n", fe.bt_devices_known ? "known" : "unknown",
                    (unsigned long long)now);
            open_sound(&fe, &audio, record, now);
            audio_started = 1;
        }
        // GameSwitcher's picker runs over the screen (docs/mainui.md): nothing is drawn and no
        // user input acts until it ends. A quit ends the picker first.
        if (mainui_frontend_picker_active(&fe) || picker_quit) {
            SDL_Event event;
            while (SDL_PollEvent(&event)) {
                struct controller *c;
                switch (event.type) {
                case SDL_QUIT: picker_quit = 1; break;
                case SDL_JOYDEVICEADDED: open_controllers(controllers, record, repeat_epoch); break;
                case SDL_JOYDEVICEREMOVED:
                    c = controller_for(controllers, event.jdevice.which);
                    if (c) {
                        SDL_JoystickClose(c->joystick);
                        c->joystick = NULL;
                        mainui_input_reset(&c->input);
                    }
                    break;
                case SDL_KEYUP: wake_up_audio(&fe, audio_started ? &audio : NULL, &event, dry, record, now); break;
                default: break;
                }
            }
            if (picker_quit && !picker_cancelled) {
                mainui_frontend_picker_cancel(&fe, now);
                picker_cancelled = 1;
                picker_quit_ms = now;
            }
            // The Bluetooth answers still arrive, for the sound's route.
            if (now - last_tick >= 200 || (!audio_started && now - last_tick >= 20)) {
                mainui_frontend_tick(&fe, now);
                last_tick = now;
            }
            if (mainui_frontend_picker_poll(&fe, now)) {
                fprintf(record, "gameswitcher picker done ms=%llu\n", (unsigned long long)now);
                record_view(&fe, record, now);
                // What was held over the picker acts only once pressed again.
                for (int i = 0; i < CONTROLLERS; ++i) {
                    mainui_input_reset(&controllers[i].input);
                    controllers[i].stale_up = ~(Uint64)0;
                }
                dirty = 1;
                if (fe.exit_requested) { running = 0; reason = "handoff"; }
            }
            if (picker_quit && (!mainui_frontend_picker_active(&fe) || now - picker_quit_ms >= 5000)) {
                running = 0;
                reason = "quit";
            }
            if (running) SDL_Delay(16);
            continue;
        }
        read_input_gate(&input_gate, controllers, fe.fs.root, record, now);
        SDL_Event event;
        while (running && SDL_PollEvent(&event)) {
            read_input_gate(&input_gate, controllers, fe.fs.root, record, now);
            // Stock wait_event clicks before interpreting button-down/hat actions.
            input_sound(&fe, audio_started ? &audio : NULL, &event, input_gate.osd);
            struct controller *c;
            switch (event.type) {
            case SDL_QUIT: running = 0; reason = "quit"; break;
            case SDL_JOYDEVICEADDED: open_controllers(controllers, record, repeat_epoch); break;
            // A Bluetooth stream written before bluealsa has it ready fails for good in SDL
            // ("ALSA write failed (unrecoverable)"), and the official then stays silent until it
            // restarts. BrickOS opens the sound again a second later, up to five times in a row.
            case SDL_AUDIODEVICEREMOVED:
                if (!event.adevice.iscapture && audio_started && !dry) {
                    if (now - audio_lost_at > 30000) audio_retries = 0;
                    audio_lost_at = now;
                    if (audio_retries < 5) audio_retry_at = now + 1000;
                    fprintf(record, "audio device lost retry=%d ms=%llu\n", audio_retries, (unsigned long long)now);
                }
                break;
            case SDL_JOYDEVICEREMOVED:
                c = controller_for(controllers, event.jdevice.which);
                if (c) {
                    SDL_JoystickClose(c->joystick);
                    c->joystick = NULL;
                    mainui_input_reset(&c->input);
                }
                // The removed controller drops its own held state; another one's held keys
                // and the separate POWER path must keep working.
                fprintf(record, "controller removed id=%d ms=%llu\n", (int)event.jdevice.which, (unsigned long long)now);
                break;
            case SDL_JOYHATMOTION: {
                c = controller_for(controllers, event.jhat.which);
                if (!c || event.jhat.hat != 0) break;
                int x = (event.jhat.value & SDL_HAT_LEFT) ? -1 : (event.jhat.value & SDL_HAT_RIGHT) ? 1 : 0;
                int y = (event.jhat.value & SDL_HAT_UP) ? -1 : (event.jhat.value & SDL_HAT_DOWN) ? 1 : 0;
                mainui_input_hat(&c->input, x, y, now);
                break;
            }
            case SDL_JOYBUTTONDOWN:
                // Official wait_event resets the global repeat reader on every
                // button-down, even when it produces no logical button action.
                // Keep already-produced events and raw IO indicators intact.
                interrupt_repeats(controllers, &repeat_epoch, now);
                /* fall through */
            case SDL_JOYBUTTONUP:
                c = controller_for(controllers, event.jbutton.which);
                if (c && event.jbutton.button < 64) {
                    // The release of a button held over GameSwitcher's picker, such as the B that
                    // closed it, does nothing.
                    Uint64 bit = (Uint64)1 << event.jbutton.button;
                    if (event.type == SDL_JOYBUTTONDOWN) c->stale_up &= ~bit;
                    else if (c->stale_up & bit) {
                        c->stale_up &= ~bit;
                        break;
                    }
                }
                if (c && event.jbutton.button < c->count)
                    mainui_input_button(&c->input, c->codes[event.jbutton.button], event.type == SDL_JOYBUTTONDOWN, now);
                break;
            case SDL_JOYAXISMOTION:
                // L2 and R2 are ABS_Z and ABS_RZ; SDL gives 0..32767 for 0..255.
                c = controller_for(controllers, event.jaxis.which);
                if (c && (event.jaxis.axis == 2 || event.jaxis.axis == 5))
                    mainui_input_axis(&c->input, event.jaxis.axis == 2 ? MAINUI_CODE_ABS_Z : MAINUI_CODE_ABS_RZ,
                                      event.jaxis.value / 128, now);
                if (c && (event.jaxis.axis == 2 || event.jaxis.axis == 5))
                    mainui_input_test_axis(&c->input, event.jaxis.axis, event.jaxis.value);
                break;
            case SDL_KEYDOWN:
            case SDL_KEYUP:
                // KeyTestWindow2 receives arrows as SDL keyboard events, rather
                // than the ordinary joystick hat used by menu navigation.
                switch (event.key.keysym.sym) {
                case SDLK_UP: mainui_input_test_button(&input, MAINUI_CODE_UP, event.type == SDL_KEYDOWN); break;
                case SDLK_DOWN: mainui_input_test_button(&input, MAINUI_CODE_DOWN, event.type == SDL_KEYDOWN); break;
                case SDLK_LEFT: mainui_input_test_button(&input, MAINUI_CODE_LEFT, event.type == SDL_KEYDOWN); break;
                case SDLK_RIGHT: mainui_input_test_button(&input, MAINUI_CODE_RIGHT, event.type == SDL_KEYDOWN); break;
                default: break;
                }
                if (event.key.keysym.sym == SDLK_VOLUMEUP || event.key.keysym.sym == SDLK_VOLUMEDOWN)
                    mainui_input_test_button(&input, event.key.keysym.sym == SDLK_VOLUMEUP ? KEY_VOLUMEUP : KEY_VOLUMEDOWN,
                                             event.type == SDL_KEYDOWN);
                if (event.key.keysym.sym == SDLK_POWER) mainui_input_power(&input, event.type == SDL_KEYDOWN, now);
                if (!input_gate.osd) wake_up_audio(&fe, audio_started ? &audio : NULL, &event, dry, record, now);
                break;
            default: break;
            }
            // Consume actions from this event under the marker state observed when
            // it was polled, even if the OSD disappears before the next event.
            if (input_gate.osd) discard_osd_input(controllers, now);
            // Apply this event before polling the next: a tone/theme change belongs
            // to the following event, even when SDL has queued both already.
            if (running) dirty |= dispatch_inputs(&fe, audio_started ? &audio : NULL, controllers, &input_gate, record, now, &keys);
            // Apply held test controls per event too: a queued shoulder press/release
            // must retain its motor on/off operations even without an intervening frame.
            if (running) dirty |= update_io_controls(&fe, controllers, &input);
            if (fe.exit_requested) { running = 0; reason = "handoff"; }
        }
        if (running) {
            dirty |= dispatch_inputs(&fe, audio_started ? &audio : NULL, controllers, &input_gate, record, now, &keys);
            if (fe.exit_requested) { running = 0; reason = "handoff"; }
        }
        if (!running) break;
        dirty |= update_io_controls(&fe, controllers, &input);
        read_input_gate(&input_gate, controllers, fe.fs.root, record, now);
        if (running && !input_gate.osd && mainui_input_power_hold(&input, now))
            dirty |= mainui_frontend_event(&fe, MAINUI_EVENT_POWER_HOLD);
        // BGM Volume and the theme's sounds follow the settings, and a frontend switch.
        if (audio_started) {
            mainui_audio_volume(&audio, fe.settings.values[MAINUI_SET_BGMVOL]);
            mainui_audio_theme(&audio, &fe.fs, mainui_frontend_look_theme(&fe));
        }
        // keymon reports a volume button in its memory: the click follows 150 ms after the last
        // report, with Enable Keytone on or off, and the report is cleared (static: draw_nav_bar).
        int reported = 0;
        if (!dry && fe.ops.shm_get && fe.ops.shm_get(fe.ops.context, MAINUI_SHM_VOL_CHANGED, &reported) == 0 && reported > 0) {
            if (audio_started) mainui_audio_volume_report(&audio);
            if (fe.ops.shm_set) fe.ops.shm_set(fe.ops.context, MAINUI_SHM_VOL_CHANGED, 0);
        }
        int failures = audio.volume_failures;
        fe.effects_failed += failures - volume_errors;
        volume_errors = failures;
        // The volume buttons change keymon's values without the frontend.
        if (now - last_sync >= 500) {
            dirty |= mainui_frontend_sync(&fe);
            last_sync = now;
            // After a suspend keymon leaves this marker; the music volume is set again and the
            // marker removed (static: runWindows).
            if (access("/tmp/wakeup_from_suspend", F_OK) == 0) {
                if (!dry) unlink("/tmp/wakeup_from_suspend");
                fprintf(record, "audio restore after suspend ms=%llu\n", (unsigned long long)now);
                if (audio_started) {
                    audio.volume = -1;
                    mainui_audio_volume(&audio, fe.settings.values[MAINUI_SET_BGMVOL]);
                }
            }
        }
        if (now - last_status >= 10000) {
            read_status(&renderer.status);
            last_status = now;
            dirty = 1;
        }
        // The network adapters: Wi-Fi scans, Bluetooth answers, and the end of a wait. While the
        // sound waits for the Bluetooth device list, every 20 ms, as the official's wait did.
        if (running && (now - last_tick >= 200 || (!audio_started && now - last_tick >= 20))) {
            dirty |= mainui_frontend_tick(&fe, now);
            last_tick = now;
            if (fe.exit_requested) { running = 0; reason = "handoff"; }
            // A Bluetooth audio device that connects or goes away takes the sound or gives it
            // back (static: the official's user events 22 and 40 call reopen_audio).
            if (running && audio_started && strcmp(audio_route(&fe), audio.route) != 0) {
                fprintf(record, "audio route %s ms=%llu\n", audio_route(&fe)[0] ? "bluealsa" : "speaker", (unsigned long long)now);
                mainui_audio_reopen(&audio, audio_route(&fe), fe.settings.values[MAINUI_SET_VOL]);
            }
        }
        if (running && audio_retry_at && now >= audio_retry_at) {
            audio_retry_at = 0;
            ++audio_retries;
            fprintf(record, "audio reopen after loss route=%s ms=%llu\n", audio.route[0] ? "bluealsa" : "speaker",
                    (unsigned long long)now);
            mainui_audio_reopen(&audio, audio.route, fe.settings.values[MAINUI_SET_VOL]);
        }
        if (!running) break;
        // A progress message animates its dots, a frame every 100 ms (static: ProgressDialog).
        if (renderer.presentation != fe.model.presentation) {
            renderer.presentation = fe.model.presentation;
            dirty = 1;
        }
        if (mainui_model_top(&fe.model)->kind == MAINUI_WIN_PROGRESS && now - last_frame >= 100) dirty = 1;
        dirty |= mainui_frontend_motion(&fe, now);
        if (mainui_render_frame_due(&renderer, &fe, now)) dirty = 1;
        // A key or tick of this turn may have opened GameSwitcher's picker: it draws now.
        if (mainui_frontend_picker_active(&fe)) continue;
        if (dirty && running) {
            uint64_t render_start = NOW_MS();
            mainui_render_frame_at(&renderer, &fe, render_start);
            uint64_t present_start = NOW_MS();
            // A frame that decoded pictures for the first time (a system background, a cover)
            // holds the slides and fades it showed for that long: they go on after it instead of
            // ending at once. The drawing itself, slow as it may be, does not hold them.
            uint64_t decoded_ms = mainui_render_decode_ms(&renderer);
            if (decoded_ms > FRAME_STALL_MS) mainui_frontend_motion_stall(&fe, render_start, decoded_ms);
            SDL_RenderPresent(sdl);
            ++frames;
            fprintf(record, "frame number=%d ms=%llu render_ms=%llu present_ms=%llu\n", frames,
                    (unsigned long long)now, (unsigned long long)(present_start - render_start),
                    (unsigned long long)(NOW_MS() - present_start));
            dirty = 0;
            last_frame = now;
        }
        // While no slide or fade runs, a worker decodes the pictures the next step may need.
        (void)mainui_render_prefetch(&renderer, &fe, NOW_MS());
        // Vsync already paces animated frames. An additional sleep skips refreshes.
        if (!mainui_render_animating(&renderer, &fe)) SDL_Delay(16);
    }
    fprintf(record, "exit reason=%s ms=%llu frames=%d keys=%d effects=%d failed=%d clicks=%d\n", reason,
            (unsigned long long)NOW_MS(), frames, keys, fe.effects_done, fe.effects_failed, audio.clicks);
    result = fe.exit_requested ? fe.exit_status : 0;
    clean = 1;
cleanup:
    fe.bgm_volume = NULL;
    fe.bgm_context = NULL;
    fe.theme_audio = NULL;
    fe.theme_context = NULL;
    if (audio_started) mainui_audio_close(&audio);
    if (audio.volume_failures) fprintf(record, "audio volume timer failures=%d\n", (int)audio.volume_failures);
    for (int c = 0; c < CONTROLLERS; ++c)
        if (controllers[c].joystick) SDL_JoystickClose(controllers[c].joystick);
    mainui_render_early_take(NULL, &early);
    mainui_render_close(&renderer);
    if (sdl) SDL_DestroyRenderer(sdl);
    if (window) SDL_DestroyWindow(window);
    TTF_Quit();
    IMG_Quit();
    SDL_Quit();
    // The official writes the lists' positions as it exits, a launch handoff included (static:
    // runWindows).
    if (clean && mainui_frontend_save_positions(&fe) < 0) fprintf(record, "positions save failed\n");
    if (clean) mainui_frontend_finish(&fe);
    mainui_frontend_close(&fe);
    mainui_are_device_close(&are_device);
    if (!dry) mainui_device_close(&device);
    fprintf(record, "released fds_before=%d fds_after=%d\n", descriptors, open_descriptors());
    // Only a run that got here ended cleanly; a crash or kill never writes this marker.
    if (clean && mainui_marker_write(MAINUI_MARKER_DONE) < 0) fprintf(record, "marker done failed\n");
    if (record != stdout) fclose(record);
    return result;
}
