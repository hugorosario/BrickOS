#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L
// brickos-uninstall: the program of the Uninstall BrickOS app (/mnt/SDCARD/Apps/BrickOSUninstall,
// run by its launch.sh). An Apps launch runs with no frontend process, and the init starts
// MainUI again when this program exits. It shows the confirmation screen; cancelling exits with
// status 0 and changes nothing. Confirming stages the uninstall start script from uninstall.sh
// in this program's folder (stage.h), shows the restarting screen, and runs sync and reboot;
// the removal itself runs at the next boot. A staging error shows the error screen, where A or
// B returns to MainUI.
//
// Keys come from SDL's joystick, TRIMUI Player1 (the D-pad as its hat, A and B by their evdev
// codes), and from keyboard events, as MainUI reads them. BRICKOS_ROOT prefixes every device
// path and BRICKOS_UNINSTALL_REBOOT replaces the reboot command, for tests.
#include "model.h"
#include "render.h"
#include "stage.h"
#include "../common/version.h"
BRICKOS_DECLARE_COMPONENT("uninstall", BRICKOS_VERSION_UNINSTALL);

#include <SDL.h>
#include <SDL_ttf.h>
#include <fcntl.h>
#include <limits.h>
#include <linux/input.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#define CONTROLLERS 8
#define BUTTONS 64
// How long the restarting screen waits for the reboot to take the system down.
#define RESTART_WAIT_MS 60000

// A joystick and the evdev code of each SDL button.
struct controller {
    SDL_Joystick *joystick;
    SDL_JoystickID id;
    int codes[BUTTONS];
    int count;
};

// SDL numbers a joystick's buttons by evdev code, from BTN_JOYSTICK up, then the codes below it.
// Without the evdev node, TRIMUI Player1's layout: B A Y X L1 R1 SELECT START MENU F1 F2.
static void map_buttons(struct controller *c) {
    static const int layout[] = {BTN_SOUTH, BTN_EAST, BTN_NORTH, BTN_WEST, BTN_TL, BTN_TR,
                                 BTN_SELECT, BTN_START, BTN_MODE, BTN_THUMBL, BTN_THUMBR};
    c->count = (int)(sizeof(layout) / sizeof(layout[0]));
    memcpy(c->codes, layout, sizeof(layout));
#if SDL_VERSION_ATLEAST(2, 24, 0)
    const char *path = SDL_JoystickPath(c->joystick);
    int fd = path ? open(path, O_RDONLY | O_CLOEXEC) : -1;
    if (fd < 0) return;
    unsigned long bits[(KEY_MAX + 1) / (8 * sizeof(unsigned long)) + 1];
    memset(bits, 0, sizeof(bits));
    if (ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(bits)), bits) >= 0) {
        const int per = (int)(sizeof(unsigned long) * CHAR_BIT), total = (int)(sizeof(bits) / sizeof(bits[0])) * per;
        int n = 0;
        for (int pass = 0; pass < 2; ++pass)
            for (int code = pass ? 0 : BTN_JOYSTICK; code < (pass ? BTN_JOYSTICK : total) && n < BUTTONS; ++code)
                if ((bits[code / per] >> (code % per)) & 1UL) c->codes[n++] = code;
        if (n) c->count = n;
    }
    close(fd);
#endif
}

static void open_controllers(struct controller *controllers) {
    for (int i = 0; i < SDL_NumJoysticks(); ++i) {
        SDL_JoystickID id = SDL_JoystickGetDeviceInstanceID(i);
        int open = 0, free_slot = -1;
        for (int c = 0; c < CONTROLLERS; ++c) {
            if (controllers[c].joystick && controllers[c].id == id) open = 1;
            if (!controllers[c].joystick && free_slot < 0) free_slot = c;
        }
        if (open || free_slot < 0) continue;
        struct controller *c = &controllers[free_slot];
        c->joystick = SDL_JoystickOpen(i);
        if (!c->joystick) continue;
        c->id = SDL_JoystickInstanceID(c->joystick);
        map_buttons(c);
    }
}

static struct controller *controller_for(struct controller *controllers, SDL_JoystickID id) {
    for (int c = 0; c < CONTROLLERS; ++c)
        if (controllers[c].joystick && controllers[c].id == id) return &controllers[c];
    return NULL;
}

// The key an event gives, or -1. On the Brick, A is BTN_EAST and B is BTN_SOUTH.
static int key_of(struct controller *controllers, const SDL_Event *event) {
    struct controller *c;
    switch (event->type) {
    case SDL_JOYHATMOTION:
        if (event->jhat.hat != 0) return -1;
        if (event->jhat.value & SDL_HAT_LEFT) return UNINSTALL_KEY_LEFT;
        if (event->jhat.value & SDL_HAT_RIGHT) return UNINSTALL_KEY_RIGHT;
        return -1;
    case SDL_JOYBUTTONDOWN:
        c = controller_for(controllers, event->jbutton.which);
        if (!c || event->jbutton.button >= c->count) return -1;
        if (c->codes[event->jbutton.button] == BTN_EAST) return UNINSTALL_KEY_A;
        if (c->codes[event->jbutton.button] == BTN_SOUTH) return UNINSTALL_KEY_B;
        return -1;
    case SDL_KEYDOWN:
        if (event->key.repeat) return -1;
        switch (event->key.keysym.sym) {
        case SDLK_LEFT: return UNINSTALL_KEY_LEFT;
        case SDLK_RIGHT: return UNINSTALL_KEY_RIGHT;
        case SDLK_RETURN: case SDLK_KP_ENTER: return UNINSTALL_KEY_A;
        case SDLK_ESCAPE: case SDLK_BACKSPACE: return UNINSTALL_KEY_B;
        default: return -1;
        }
    default:
        return -1;
    }
}

struct display {
    SDL_Window *window;
    SDL_Renderer *renderer;
    SDL_Texture *texture;
    SDL_Surface *surface;
    struct uninstall_renderer look;
    struct uninstall_model *model;
};

static void present(struct display *d) {
    uninstall_render(&d->look, d->surface, d->model, NULL);
    SDL_UpdateTexture(d->texture, NULL, d->surface->pixels, d->surface->pitch);
    SDL_RenderClear(d->renderer);
    SDL_RenderCopy(d->renderer, d->texture, NULL, NULL);
    SDL_RenderPresent(d->renderer);
}

// Called once the script is staged, before the reboot command runs.
static void show_restarting(void *context) {
    struct display *d = context;
    uninstall_model_staged(d->model);
    present(d);
}

// The folder this program runs from, where the app keeps uninstall.sh.
static int program_folder(char *out, size_t size) {
    ssize_t n = readlink("/proc/self/exe", out, size - 1);
    if (n <= 0) return -1;
    out[n] = 0;
    char *slash = strrchr(out, '/');
    if (!slash) return -1;
    *slash = 0;
    return 0;
}

int main(int argc, char **argv) {
    BRICKOS_HANDLE_VERSION(argc, argv);
    const char *root = getenv("BRICKOS_ROOT");
    const char *reboot_command = getenv("BRICKOS_UNINSTALL_REBOOT");
    if (!root) root = "";
    if (!reboot_command || !reboot_command[0]) reboot_command = UNINSTALL_DEFAULT_REBOOT;
    struct uninstall_model model;
    uninstall_model_load(&model, root);
    char language[64], folder[PATH_MAX], font[PATH_MAX];
    uninstall_read_language(root, language, sizeof(language));
    if (program_folder(folder, sizeof(folder)) < 0) {
        fprintf(stderr, "brickos-uninstall: cannot find its own folder\n");
        return 0;
    }
    if (uninstall_font(root, language, font, sizeof(font)) < 0) {
        fprintf(stderr, "brickos-uninstall: no Default theme font\n");
        return 0;
    }
    // Every failure to show the screen exits as a cancel: nothing has changed.
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_JOYSTICK | SDL_INIT_EVENTS) != 0) {
        fprintf(stderr, "brickos-uninstall: SDL_Init: %s\n", SDL_GetError());
        return 0;
    }
    struct display d = {.model = &model};
    struct controller controllers[CONTROLLERS];
    memset(controllers, 0, sizeof(controllers));
    if (TTF_Init() < 0) {
        fprintf(stderr, "brickos-uninstall: TTF_Init: %s\n", TTF_GetError());
        SDL_Quit();
        return 0;
    }
    if (uninstall_renderer_open(&d.look, font) < 0) {
        fprintf(stderr, "brickos-uninstall: cannot open %s\n", font);
        goto done;
    }
    d.window = SDL_CreateWindow("Uninstall BrickOS", 0, 0, UNINSTALL_SCREEN_W, UNINSTALL_SCREEN_H, SDL_WINDOW_SHOWN);
    d.renderer = d.window ? SDL_CreateRenderer(d.window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC) : NULL;
    if (d.window && !d.renderer) d.renderer = SDL_CreateRenderer(d.window, -1, SDL_RENDERER_SOFTWARE);
    d.texture = d.renderer ? SDL_CreateTexture(d.renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING,
                                               UNINSTALL_SCREEN_W, UNINSTALL_SCREEN_H)
                           : NULL;
    d.surface = SDL_CreateRGBSurfaceWithFormat(0, UNINSTALL_SCREEN_W, UNINSTALL_SCREEN_H, 32, SDL_PIXELFORMAT_ARGB8888);
    if (!d.texture || !d.surface) {
        fprintf(stderr, "brickos-uninstall: video: %s\n", SDL_GetError());
        goto done;
    }
    open_controllers(controllers);
    present(&d);
    int running = 1, restarted = 0;
    while (running && !restarted) {
        SDL_Event event;
        if (!SDL_WaitEventTimeout(&event, 100)) continue;
        int dirty = 0;
        do {
            if (event.type == SDL_QUIT) running = 0;
            if (event.type == SDL_JOYDEVICEADDED) open_controllers(controllers);
            if (event.type == SDL_JOYDEVICEREMOVED) {
                struct controller *c = controller_for(controllers, event.jdevice.which);
                if (c) {
                    SDL_JoystickClose(c->joystick);
                    memset(c, 0, sizeof(*c));
                }
            }
            int key = key_of(controllers, &event);
            if (key < 0) continue;
            switch (uninstall_model_key(&model, (enum uninstall_key)key)) {
            case UNINSTALL_ACTION_NONE: dirty = 1; break;
            case UNINSTALL_ACTION_EXIT: running = 0; break;
            case UNINSTALL_ACTION_UNINSTALL: {
                enum uninstall_error error = uninstall_confirm(root, folder, reboot_command, show_restarting, &d);
                if (error == UNINSTALL_ERROR_NONE) restarted = 1;
                else uninstall_model_failed(&model, error);
                dirty = 1;
                break;
            }
            }
        } while (running && !restarted && SDL_PollEvent(&event));
        if (running && !restarted && dirty) present(&d);
    }
    // The reboot takes the system down; should it not, the next start runs the removal anyway.
    if (restarted) {
        Uint32 deadline = SDL_GetTicks() + RESTART_WAIT_MS;
        SDL_Event event;
        while (!SDL_TICKS_PASSED(SDL_GetTicks(), deadline))
            if (SDL_WaitEventTimeout(&event, 500) && event.type == SDL_QUIT) break;
    }
done:
    for (int c = 0; c < CONTROLLERS; ++c)
        if (controllers[c].joystick) SDL_JoystickClose(controllers[c].joystick);
    if (d.surface) SDL_FreeSurface(d.surface);
    if (d.texture) SDL_DestroyTexture(d.texture);
    if (d.renderer) SDL_DestroyRenderer(d.renderer);
    if (d.window) SDL_DestroyWindow(d.window);
    uninstall_renderer_close(&d.look);
    TTF_Quit();
    SDL_Quit();
    return 0;
}
