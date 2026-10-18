#include "state.h"

#include <string.h>

int osdd_toast_state_add(struct osdd_toast_state *state, const struct osdd_toast *toast,
                         uint64_t now_ms) {
    if (state == NULL || toast == NULL || memchr(toast->id, 0, sizeof(toast->id)) == NULL)
        return -1;
    int index = -1;
    // A matching ID, including the empty ID of a freed slot, is replaced in place.
    for (int slot = 0; slot < OSDD_TOAST_SLOTS && index < 0; ++slot)
        if (strcmp(state->slots[slot].value.id, toast->id) == 0) index = slot;
    if (index < 0) {
        // The official daemon compares every slot with slot 0, not with a running
        // minimum: the last slot added before slot 0 is reused, otherwise slot 0.
        index = 0;
        for (int slot = 1; slot < OSDD_TOAST_SLOTS; ++slot)
            if (state->slots[slot].added_ms < state->slots[0].added_ms) index = slot;
    }
    struct osdd_toast_slot *target = &state->slots[index];
    target->value = *toast;
    target->remaining_ms = toast->duration;
    target->added_ms = now_ms == 0 ? 1 : now_ms;
    return index;
}

size_t osdd_toast_state_check_timeout(struct osdd_toast_state *state, uint64_t now_ms) {
    if (state == NULL) return 0;
    if (!state->has_last_check) {
        state->last_check_ms = now_ms;
        state->has_last_check = 1;
    }
    uint64_t difference = now_ms > state->last_check_ms ? now_ms - state->last_check_ms : 0;
    int elapsed = difference > 0x7fffffff ? 0x7fffffff : (int)difference;
    size_t freed = 0;
    int active = 0;
    for (int index = 0; index < OSDD_TOAST_SLOTS; ++index) {
        struct osdd_toast_slot *slot = &state->slots[index];
        if (slot->remaining_ms == 0) continue;
        if (slot->remaining_ms > 0) {
            slot->remaining_ms -= elapsed; // Both are non-negative ints, so this cannot overflow.
            if (slot->remaining_ms >= 0) {
                ++active;
                continue;
            }
        }
        memset(slot, 0, sizeof(*slot));
        ++freed;
    }
    state->last_check_ms = now_ms;
    state->count = active;
    return freed;
}

enum osdd_toast_show osdd_toast_state_check_show(struct osdd_toast_state *state) {
    if (state == NULL) return OSDD_TOAST_KEEP;
    enum osdd_toast_show action = OSDD_TOAST_KEEP;
    if (state->count != 0 && state->shown_count == 0) {
        action = OSDD_TOAST_SHOW;
        state->hide_pending = 0;
    } else if (state->count == 0 && state->shown_count != 0) {
        state->hide_pending = 1;
    } else if (state->hide_pending > 0 && --state->hide_pending == 0) {
        action = OSDD_TOAST_HIDE;
    }
    state->shown_count = state->count;
    return action;
}

int osdd_toast_slot_visible(const struct osdd_toast_slot *slot) {
    if (slot == NULL || slot->remaining_ms <= 0 || slot->value.w <= 7 || slot->value.h <= 7)
        return 0;
    // The official check is a message or a loaded background; sizes 0 to 3 have one.
    return slot->value.message[0] != 0 || slot->value.bg[0] != 0 ||
           (slot->value.size >= 0 && slot->value.size < 4);
}

int osdd_toast_state_active(const struct osdd_toast_state *state) {
    if (state == NULL) return 0;
    for (int index = 0; index < OSDD_TOAST_SLOTS; ++index)
        if (osdd_toast_slot_visible(&state->slots[index])) return 1;
    return 0;
}
