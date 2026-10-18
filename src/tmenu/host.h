#ifndef BRICKOS_TMENU_HOST_H
#define BRICKOS_TMENU_HOST_H

#include <stddef.h>
#include <stdint.h>

#define TMENU_ASYNC_SAVESTATE_MARKER "BrickOS PPSSPP async state v1"

// The host table RetroArch passes to ShowMenu (docs/tmenu-abi.md).
struct tmenu_host {
    int (*saved_slots)(int max, int *slots);
    char *(*thumb_path)(int slot);
    int (*save_state)(int slot);
    int (*load_state)(int slot);
    void (*quit)(void);
    void (*fullscreen_toggle)(void);
    int (*fullscreen_state)(void);
    int slot_count;
    int64_t reserved;       // 0x40, retained from the observed ABI
    char rom_name[256];     // 0x48; PPSSPP uses a private marker here
};

struct SDL_Surface;
typedef void (*tmenu_present_fn)(struct SDL_Surface *frame);

// What the check needs of an SDL surface, so it stays independent of SDL.
struct tmenu_frame_info {
    int present;  // 0 for a NULL surface
    int w, h, pitch;
    uint32_t format;
    int has_pixels;
};

#define TMENU_FRAME_W 1024
#define TMENU_FRAME_H 768
#define TMENU_FRAME_PITCH 4096
#define TMENU_FRAME_FORMAT 0x16762004u  // SDL_PIXELFORMAT_ABGR8888
#define TMENU_SLOTS_MIN 1
#define TMENU_SLOTS_MAX 100

enum tmenu_call_check {
    TMENU_CALL_OK,
    TMENU_CALL_BAD_ARGUMENTS,  // what the official library also rejects: return 0
    TMENU_CALL_UNKNOWN_HOST,   // not the observed RetroArch call: delegate
};

// Checks the arguments against the call RetroArch makes on the stock firmware: the
// host table, its callbacks, the caller name, and the two surfaces.
enum tmenu_call_check tmenu_call_check(const struct tmenu_host *host, const struct tmenu_frame_info *screen,
                                       const struct tmenu_frame_info *background, const char *name,
                                       tmenu_present_fn present, char *reason, size_t reason_size);

#endif
