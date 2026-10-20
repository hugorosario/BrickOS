#include "model.h"
#include "history.h"
#include <stdio.h>
#include <string.h>

void gs_model_init(struct gs_model *m) {
    memset(m, 0, sizeof(*m)); m->battery = -1;
}
void gs_model_close(struct gs_model *m) { gs_index_close(&m->index); }
const struct gs_entry *gs_model_selected(const struct gs_model *m) {
    return m->selected < m->index.count ? &m->index.entries[m->selected] : NULL;
}
int gs_model_reload(struct gs_model *m, struct gs_store *s) {
    char id[GS_ID] = {0}; const struct gs_entry *previous = gs_model_selected(m);
    if (previous) memcpy(id, previous->id, GS_ID);
    struct gs_index next; struct gs_excluded excluded;
    // An unreadable list hides nothing.
    if (gs_excluded_load(s, &excluded)) excluded.count = 0;
    if (gs_store_load(s, &next)) {
        snprintf(m->message, sizeof(m->message), "Cannot read checkpoint history"); return -1;
    }
    // Played-only records are retained for time/history but have no card until saved; an excluded
    // system's cards keep theirs but do not show.
    size_t visible = 0;
    m->total_ms = 0;
    for (size_t i = 0; i < next.count; ++i) {
        uint64_t t = next.entries[i].play_ms;
        m->total_ms = UINT64_MAX - m->total_ms < t ? UINT64_MAX : m->total_ms + t;
        if (gs_history_shown(s, &excluded, &next.entries[i])) next.entries[visible++] = next.entries[i];
    }
    next.count = visible;
    gs_index_close(&m->index); m->index = next;
    if (m->selected >= visible) m->selected = visible ? visible - 1 : 0;
    for (size_t i = 0; i < visible; ++i) if (!strcmp(id, next.entries[i].id)) m->selected = i;
    return 0;
}
enum gs_action gs_model_repeat(struct gs_model *m, uint64_t now) {
    if (m->repeat_armed) { m->repeat_armed = 0; m->repeat_ms = now + GS_REPEAT_DELAY_MS; return GS_NONE; }
    if (!m->repeat_ms || !m->x || m->blocked || m->index.count < 2 || now < m->repeat_ms) return GS_NONE;
    m->repeat_ms = now - m->repeat_ms >= GS_REPEAT_INTERVAL_MS ? now + GS_REPEAT_INTERVAL_MS : m->repeat_ms + GS_REPEAT_INTERVAL_MS;
    if (m->x < 0) m->selected = m->selected ? m->selected - 1 : m->index.count - 1;
    else m->selected = (m->selected + 1) % m->index.count;
    m->message[0] = 0; return GS_CHANGED;
}
int gs_model_repeat_wait(const struct gs_model *m, uint64_t now) {
    if (m->repeat_armed) return 0;
    if (!m->repeat_ms || !m->x) return -1;
    return now >= m->repeat_ms ? 0 : (int)(m->repeat_ms - now);
}
uint64_t gs_model_total(const struct gs_model *m) {
    return m->total_ms;
}
static int neutral(const struct gs_model *m) {
    if (m->x || m->y) return 0;
    for (size_t i = 0; i < sizeof(m->held); ++i) if (m->held[i]) return 0;
    return 1;
}
void gs_model_block(struct gs_model *m, const unsigned char *held, int x, int y) {
    memcpy(m->held, held, sizeof(m->held)); m->x = x; m->y = y; m->blocked = !neutral(m);
}
enum gs_action gs_model_event(struct gs_model *m, const struct input_event *e) {
    int prior = 0;
    if (e->type == EV_KEY && e->code <= KEY_MAX) {
        prior = m->held[e->code]; m->held[e->code] = e->value != 0;
    } else if (e->type == EV_ABS && (e->code == ABS_HAT0X || e->code == ABS_HAT0Y)) {
        int *hat = e->code == ABS_HAT0X ? &m->x : &m->y;
        prior = *hat; *hat = e->value;
        // Any change of LEFT or RIGHT stops the repeat; a new step below starts it again.
        if (e->code == ABS_HAT0X && e->value != prior) { m->repeat_armed = 0; m->repeat_ms = 0; }
    } else return GS_NONE;
    if (m->blocked) { if (neutral(m)) m->blocked = 0; return GS_NONE; }
    if (!e->value || prior || (e->type == EV_KEY && e->value != 1)) return GS_NONE;
    if (e->type == EV_ABS && e->code == ABS_HAT0X) {
        if (m->index.count < 2) return GS_NONE;
        if (e->value < 0) m->selected = m->selected ? m->selected - 1 : m->index.count - 1;
        else m->selected = (m->selected + 1) % m->index.count;
        m->message[0] = 0; m->repeat_armed = 1; return GS_CHANGED;
    }
    if (e->type == EV_ABS && e->code == ABS_HAT0Y) return GS_NONE; // UP and DOWN have no action.
    // The printed buttons as TRIMUI Player1 sends them (measured on the Brick): A is BTN_EAST (305),
    // B BTN_SOUTH (304), X BTN_WEST (308) and Y BTN_NORTH (307).
    switch (e->code) {
    case BTN_EAST: case BTN_MODE: return gs_model_selected(m) ? GS_RESUME : GS_NONE;
    case BTN_SOUTH: return GS_QUIT;
    case BTN_WEST: return gs_model_selected(m) ? GS_REMOVE : GS_NONE;
    case BTN_NORTH: m->fullscreen = !m->fullscreen; return GS_CHANGED;
    case BTN_SELECT: m->show_time = !m->show_time; return GS_CHANGED;
    // keymon's brightness chord, which its grab of the controller keeps from keymon.
    case BTN_TR: return m->held[BTN_START] && !m->held[BTN_SELECT] ? GS_BRIGHTER : GS_NONE;
    case BTN_TL: return m->held[BTN_START] && !m->held[BTN_SELECT] ? GS_DARKER : GS_NONE;
    default: return GS_NONE; // START intentionally has no action.
    }
}
