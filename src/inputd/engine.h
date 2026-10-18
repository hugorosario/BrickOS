#ifndef BRICKOS_INPUTD_ENGINE_H
#define BRICKOS_INPUTD_ENGINE_H

#include <stdint.h>

#include "config.h"
#include "profile.h"

// The pure input state engine: physical snapshots and marker states in, the stock
// controller's events out, in the stock order. It opens no device.
//
// Without a profile it reproduces the stock trimui_inputd scan thread exactly,
// including its repeated zero writes. With a profile, sources that have an explicit binding
// leave the stock pass; their destinations, and any stock output to the same key or hat,
// go through reference counts, so a destination held by two sources stays held
// until the last one is released.

#define INPUTD_MAX_EVENTS 128
#define INPUTD_KEY_CODES 512
#define INPUTD_ABS_CODES 64

struct inputd_event {
    int type, code, value;
};

struct inputd_events {
    int count;
    struct inputd_event ev[INPUTD_MAX_EVENTS];
};

struct inputd_markers {
    uint32_t turbo; // bits whose turbo marker exists
    uint32_t alt;   // bits named by a dpad2axis_hold marker
    int no_dpad;    // input_no_dpad exists
    int dpad2axis;  // input_dpad_to_joystick exists
    int rotate;     // rotate_270 exists
};

struct inputd_engine {
    struct inputd_markers markers;
    uint32_t last;      // stock s_last_state: the previous pass, after the turbo mask
    int toggle;         // stock s_toggle: turbo phase
    int mode_last;      // stock d2a_last: 0 normal, 1 dpad_to_joystick, 2 held ALT
    uint32_t disarmed;  // held at a profile reload; ignored until released
    // Profile layer.
    int profile;
    uint32_t bound;     // bits with an explicit binding
    struct inputd_dest bit_dest[INPUTD_BITS];
    uint32_t bound_last;
    int fn_bound;
    struct inputd_dest fn_dest;
    int fn_held;
    unsigned char managed_key[INPUTD_KEY_CODES];
    int key_count[INPUTD_KEY_CODES];
    int managed_hat[2];           // HAT0X, HAT0Y
    int hat_pos[2], hat_neg[2], hat_cur[2];
    // What consumers were last told, for a neutral stop.
    unsigned char key_out[INPUTD_KEY_CODES];
    int abs_out[INPUTD_ABS_CODES];
};

// config may be NULL for the stock mapping.
void inputd_engine_init(struct inputd_engine *e, const struct inputd_config *config);
// One scan pass. pressed has a 1 for each held control (pins read 0). suspended is true while
// /tmp/system_suspend exists: nothing is sent and no state changes.
void inputd_engine_scan(struct inputd_engine *e, uint32_t pressed, int suspended, struct inputd_events *out);
// The stock reset when the grab marker appears or disappears.
void inputd_engine_grab(struct inputd_engine *e, struct inputd_events *out);
// The FN switch value read once a second; value is 0 or 1.
void inputd_engine_fn(struct inputd_engine *e, int value, struct inputd_events *out);
// Swaps in a validated profile (NULL for stock): releases every output and ignores the
// controls held now until each is released.
void inputd_engine_reload(struct inputd_engine *e, const struct inputd_config *config, uint32_t pressed,
                          struct inputd_events *out);
// Releases every held key and centers every axis, for a controlled stop.
void inputd_engine_neutral(struct inputd_engine *e, struct inputd_events *out);

#endif
