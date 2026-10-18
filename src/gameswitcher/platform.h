#ifndef BRICKOS_GAMESWITCHER_PLATFORM_H
#define BRICKOS_GAMESWITCHER_PLATFORM_H
#include "model.h"
#include <SDL.h>
// The panel is shown from the two slots of display channel 2, as the OSD's panel is: each frame is
// copied into the slot not shown, then the layer switches to it at the display's next refresh. A
// slot is written again only GS_SLOT_RETIRE_MS (one 60.8 Hz refresh) after the switch that hid it,
// so a frame is never changed while the display shows it.
#define GS_SLOT_RETIRE_MS 17
// The volume keys pressed on the relay node, as bits of relay_down.
#define GS_RELAY_UP 1u
#define GS_RELAY_DOWN 2u
struct gs_platform {
    int fb, disp, input;
    int relay;            // keymon's volume relay (relay.h), or -1: the volume keys go there
    unsigned relay_down;  // GS_RELAY_UP and GS_RELAY_DOWN, released at close
    int *keymon;          // keymon's shared memory (index 1: the brightness level), or NULL
    unsigned char *mapping, *backup[2]; // the slots' pixels before GameSwitcher, restored at close
    size_t map_bytes;
    unsigned char saved[256], shown[256];
    SDL_Surface *screen;
    int visible;
    uint64_t base;        // the framebuffer's physical start
    int front;            // the slot shown: 0 is panel slot 1 (shown first), 1 is panel slot 0
    uint64_t address;     // the panel slot the layer shows
    uint64_t switched_ms; // when the layer last switched slots
    unsigned char alpha;  // the layer's global alpha
};
// The framebuffer offset of slot 0 (panel slot 1) or 1 (panel slot 0).
size_t gs_platform_slot(int slot);
// The display ioctl the layer calls go through; tests replace it to record them.
extern int (*gs_platform_layer)(int fd, unsigned long request, unsigned char bytes[256]);
// The file that holds this process's pid while the layer is open, so the BrickOS OSD leaves
// channel 2 alone: written before the first frame and removed after the restore at close.
// OSDD_GAMESWITCHER_DISPLAY_MARKER; tests point it elsewhere.
extern const char *gs_platform_marker;
int gs_platform_open(struct gs_platform *platform, struct gs_model *model);
int gs_platform_present(struct gs_platform *platform);
int gs_platform_close(struct gs_platform *platform);
// Reads one input event, waiting up to timeout_ms for it: 1 with an event, 0 without, -1 when the
// controller is gone. The volume keys never come back: their presses and releases go to keymon
// through the relay, and the read goes on with the next event already waiting.
int gs_platform_input(struct gs_platform *platform, struct input_event *event, int timeout_ms);
// The keys held on the controller as GameSwitcher opens (held[code]) and the hat: the volume keys
// follow on the relay (pressed if held, released if not), since keymon saw the press before the
// grab and gets its release only from there; the other keys block the model until released.
void gs_platform_block(struct gs_platform *platform, struct gs_model *model, unsigned char held[KEY_MAX + 1], int x, int y);
// Sets the layer's global alpha, for the open and close fades; before the first present it only
// sets the alpha the layer will show with.
int gs_platform_alpha(struct gs_platform *platform, unsigned char alpha);
// Reads the battery level into the model.
void gs_platform_status(struct gs_model *model);
// keymon's brightness chord, which the controller's grab keeps from keymon: a step of +1 or -1
// within levels 0 to 10, as keymon does it. Nothing while the screen is off or without keymon's
// memory; at an end only the toast shows. Otherwise the level goes to keymon's memory, the LCD
// gets keymon's backlight for it, the toast script starts (gs_platform_reap collects it), and
// system.json keeps the level. A failure is logged and never stops GameSwitcher.
void gs_platform_brightness(struct gs_platform *platform, int step);
// Attaches keymon's shared memory without creating it; logs once when it is missing. Open calls it.
void gs_platform_attach_keymon(struct gs_platform *platform);
// Collects the toast scripts that have ended.
void gs_platform_reap(void);
// The step's device side, which tests replace: keymon's memory, the LCD brightness ioctl, the
// toast script (its pid, or -1), whether the screen is on (no /tmp/system_suspend and a backlight
// above 0), and the settings file.
extern int *(*gs_platform_keymon_attach)(void);
extern void (*gs_platform_keymon_detach)(int *memory);
extern int (*gs_platform_lcd)(int disp, int value);
extern int (*gs_platform_toast)(int level);
extern int (*gs_platform_screen_on)(void);
extern const char *gs_platform_settings;
#endif
