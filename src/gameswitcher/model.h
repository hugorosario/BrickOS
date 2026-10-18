#ifndef BRICKOS_GAMESWITCHER_MODEL_H
#define BRICKOS_GAMESWITCHER_MODEL_H
#include "store.h"
#include <linux/input.h>

// GS_BRIGHTER and GS_DARKER are keymon's brightness chord: R1 or L1 pressed while START is held
// and SELECT is not.
enum gs_action { GS_NONE, GS_CHANGED, GS_RESUME, GS_QUIT, GS_REMOVE, GS_BRIGHTER, GS_DARKER };
// A held LEFT or RIGHT repeats as BrickOS's carousel does: first 500 ms after the press, then every
// 100 ms.
#define GS_REPEAT_DELAY_MS 500
#define GS_REPEAT_INTERVAL_MS 100
struct gs_model {
    struct gs_index index;
    size_t selected;
    uint64_t total_ms;
    int fullscreen, show_time, battery, x, y, blocked;
    // A LEFT or RIGHT step waits for its repeat clock (armed), then repeats at repeat_ms.
    int repeat_armed;
    uint64_t repeat_ms;
    unsigned char held[KEY_MAX + 1];
    char message[160];
};
void gs_model_init(struct gs_model *model);
void gs_model_close(struct gs_model *model);
int gs_model_reload(struct gs_model *model, struct gs_store *store);
const struct gs_entry *gs_model_selected(const struct gs_model *model);
enum gs_action gs_model_event(struct gs_model *model, const struct input_event *event);
// Once per loop turn, after its input events: starts the repeat clock of a new LEFT or RIGHT step,
// and gives GS_CHANGED, with the selection moved, when a repeat is due. A late turn gives one step
// and restarts the interval, never a burst.
enum gs_action gs_model_repeat(struct gs_model *model, uint64_t now_ms);
// Milliseconds until the next repeat is due, or -1 when nothing repeats.
int gs_model_repeat_wait(const struct gs_model *model, uint64_t now_ms);
// Ignore held controls on entry until every tracked button and hat is released.
void gs_model_block(struct gs_model *model, const unsigned char *held, int x, int y);
uint64_t gs_model_total(const struct gs_model *model);
#endif
