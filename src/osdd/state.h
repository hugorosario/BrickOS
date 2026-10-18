#ifndef BRICKOS_OSDD_STATE_H
#define BRICKOS_OSDD_STATE_H

#include "toast.h"

#include <stddef.h>
#include <stdint.h>

// The official daemon's toast slots and loop rules, as measured in bounded traces of
// its toast handling.
#define OSDD_TOAST_SLOTS 16

struct osdd_toast_slot {
    struct osdd_toast value;
    int remaining_ms; // 0 is an occupied but inactive slot; negative expires on the next check.
    uint64_t added_ms; // 0 for a free slot.
};

struct osdd_toast_state {
    struct osdd_toast_slot slots[OSDD_TOAST_SLOTS];
    uint64_t last_check_ms;
    int has_last_check;
    int count;        // Slots still counting down after the last check.
    int shown_count;  // The count seen by the previous show decision.
    int hide_pending;
};

enum osdd_toast_show {
    OSDD_TOAST_KEEP,
    OSDD_TOAST_SHOW,
    OSDD_TOAST_HIDE
};

// Copies a record into a slot as osd_msg_add does and returns the slot index.
int osdd_toast_state_add(struct osdd_toast_state *state, const struct osdd_toast *toast,
                         uint64_t now_ms);
// Counts every slot down by the time since the previous check and frees expired
// slots. Returns the number of slots freed.
size_t osdd_toast_state_check_timeout(struct osdd_toast_state *state, uint64_t now_ms);
// Decides the layer state after a check; a hide takes effect one check later.
enum osdd_toast_show osdd_toast_state_check_show(struct osdd_toast_state *state);
// Whether a slot is drawn. Visible slots are drawn in index order.
int osdd_toast_slot_visible(const struct osdd_toast_slot *slot);
// Whether any slot keeps the loop at its 33 ms active cadence.
int osdd_toast_state_active(const struct osdd_toast_state *state);

#endif
