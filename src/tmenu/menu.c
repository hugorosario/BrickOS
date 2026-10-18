#include "menu.h"

#include <stdio.h>
#include <string.h>

#define KEYSHM_VOLUME 0
#define KEYSHM_BCONFIRM 22
#define VOLUME_MAX 20
#define SAVE_SETTLE_US 200000

static int swapped(struct tmenu_menu *m) {
    return m->ops.keyshm_get(m->ops.context, KEYSHM_BCONFIRM) != 0;
}

static void notify(struct tmenu_menu *m, const char *text) {
    snprintf(m->notify, sizeof(m->notify), "%s", text ? text : "");
    m->notify_ms = m->ops.now_ms(m->ops.context);
}

// Lists the saved slots again, keeping the Load selection on the same slot.
static void list_slots(struct tmenu_menu *m, int keep_slot) {
    int count = m->ops.saved_slots(m->ops.context, m->slot_count, m->saved);
    m->saved_count = count > 0 ? (count < m->slot_count ? count : m->slot_count) : 0;
    m->load_index = 0;
    memset(m->slot_saved, 0, sizeof(m->slot_saved));
    for (int i = 0; i < m->saved_count; ++i) {
        if (m->saved[i] == keep_slot && keep_slot != -1) m->load_index = i;
        if (m->saved[i] >= 0 && m->saved[i] < m->slot_count) m->slot_saved[m->saved[i]] = 1;
    }
}

void tmenu_menu_open(struct tmenu_menu *m, const struct tmenu_ops *ops, const struct tmenu_strings *strings,
                     int slot_count, int has_fullscreen, int fullscreen_on) {
    memset(m, 0, sizeof(*m));
    m->ops = *ops;
    m->strings = strings;
    m->slot_count = slot_count;
    m->rows = TMENU_ROWS;
    m->fullscreen_on = fullscreen_on;
    const int visible[TMENU_ITEMS] = {1, 1, 1, 0, 1, 1, has_fullscreen, 1};
    memcpy(m->visible, visible, sizeof(visible));
    list_slots(m, -1);
}

// The last item inside the seven-row window that starts at first.
static int window_last(const struct tmenu_menu *m) {
    int rows = 0, i = m->first;
    for (; i < TMENU_ITEMS && rows < m->rows; ++i) rows += m->visible[i] != 0;
    return i - 1;
}

static void keep_in_window(struct tmenu_menu *m, int down) {
    if (m->rows != TMENU_ROWS) {
        int visible[TMENU_ITEMS], count = 0, focus = 0, first = 0;
        for (int item = 0; item < TMENU_ITEMS; ++item) if (m->visible[item]) {
            if (item == m->focus) focus = count;
            if (item <= m->first) first = count;
            visible[count++] = item;
        }
        if (focus < first) first = focus;
        if (focus >= first + m->rows) first = focus - m->rows + 1;
        // Fill the viewport after wrapping from the first item to the last.
        if (first > count - m->rows) first = count > m->rows ? count - m->rows : 0;
        if (count) m->first = visible[first];
        return;
    }
    int last = window_last(m);
    if (m->focus >= m->first && m->focus <= last) return;
    if (down) m->first = m->focus == last + 1 ? m->first + 1 : m->focus;
    else m->first = m->focus == m->first - 1 ? m->first - 1 : (m->focus > m->rows - 1 ? m->focus - (m->rows - 1) : 0);
}

static enum tmenu_result move_focus(struct tmenu_menu *m, int down) {
    for (int steps = 0; steps < TMENU_ITEMS; ++steps) {
        m->focus = down ? (m->focus + 1 > TMENU_ITEMS - 1 ? 0 : m->focus + 1)
                        : (m->focus - 1 < 0 ? TMENU_ITEMS - 1 : m->focus - 1);
        if (m->rows == TMENU_ROWS) keep_in_window(m, down);
        if (m->visible[m->focus]) {
            if (m->rows != TMENU_ROWS) keep_in_window(m, down);
            break;
        }
    }
    return TMENU_RESULT_REDRAW;
}

static enum tmenu_result change_volume(struct tmenu_menu *m, int step) {
    int volume = m->ops.keyshm_get(m->ops.context, KEYSHM_VOLUME);
    if ((step > 0 && volume >= VOLUME_MAX) || (step < 0 && volume <= 0)) return TMENU_RESULT_REDRAW;
    volume += step;
    m->ops.keyshm_set(m->ops.context, KEYSHM_VOLUME, volume);
    m->ops.set_volume(m->ops.context, volume);
    return TMENU_RESULT_REDRAW;
}

static enum tmenu_result main_action(struct tmenu_menu *m) {
    switch (m->focus) {
    case TMENU_ITEM_CONTINUE: return TMENU_RESULT_CLOSE;
    case TMENU_ITEM_SAVE: m->screen = TMENU_SCREEN_SAVE; return TMENU_RESULT_REDRAW;
    case TMENU_ITEM_LOAD:
        if (m->saved_count > 0) m->screen = TMENU_SCREEN_LOAD;
        return TMENU_RESULT_REDRAW;
    case TMENU_ITEM_ADVANCED: return TMENU_RESULT_ADVANCED;
    case TMENU_ITEM_NETPLAY: return TMENU_RESULT_NETPLAY;
    case TMENU_ITEM_EXIT: m->ops.quit(m->ops.context); return TMENU_RESULT_CLOSE;
    default: return TMENU_RESULT_NONE;
    }
}

static enum tmenu_result main_key(struct tmenu_menu *m, enum tmenu_key key) {
    switch (key) {
    case TMENU_KEY_CONFIRM: return swapped(m) ? TMENU_RESULT_CLOSE : main_action(m);
    case TMENU_KEY_BACK: return swapped(m) ? main_action(m) : TMENU_RESULT_CLOSE;
    case TMENU_KEY_UP: return move_focus(m, 0);
    case TMENU_KEY_DOWN: return move_focus(m, 1);
    case TMENU_KEY_LEFT:
    case TMENU_KEY_RIGHT:
        if (m->focus == TMENU_ITEM_VOLUME) return change_volume(m, key == TMENU_KEY_RIGHT ? 1 : -1);
        if (m->focus == TMENU_ITEM_FULLSCREEN && m->ops.fullscreen) {
            m->fullscreen_on = !m->fullscreen_on;
            m->ops.fullscreen(m->ops.context);
        }
        return TMENU_RESULT_REDRAW;
    default: return TMENU_RESULT_NONE;
    }
}

static enum tmenu_result save_action(struct tmenu_menu *m) {
    if (m->ops.save_state(m->ops.context, m->save_index) < 0) {
        notify(m, m->strings->save_fail);
        return TMENU_RESULT_REDRAW;
    }
    if (m->close_after_save) {
        // PPSSPP queues the save for its next emulation frame. Close this modal
        // menu so that frame can run; the host reports completion asynchronously.
        return TMENU_RESULT_CLOSE;
    }
    // The main screen's focus stays on Save, so Back shows Save focused as it does without a save.
    // The official library moves it to Continue here; BrickOS does not (ingame-menu-experience).
    int keep = m->load_index >= 0 && m->load_index < m->saved_count ? m->saved[m->load_index] : -1;
    m->ops.sleep_us(m->ops.context, SAVE_SETTLE_US);
    list_slots(m, keep);
    m->ops.save_thumbnail(m->ops.context, m->save_index);
    char text[TMENU_NOTIFY_BYTES];
    snprintf(text, sizeof(text), m->strings->slot_saved ? m->strings->slot_saved : "", m->save_index);
    notify(m, text);
    m->ops.sync(m->ops.context);
    return TMENU_RESULT_REDRAW;
}

static enum tmenu_result load_action(struct tmenu_menu *m) {
    if (m->load_index < 0 || m->load_index >= m->saved_count ||
        m->ops.load_state(m->ops.context, m->saved[m->load_index]) < 0) {
        notify(m, m->strings->load_fail);
        return TMENU_RESULT_REDRAW;
    }
    return TMENU_RESULT_CLOSE;
}

static enum tmenu_result save_key(struct tmenu_menu *m, enum tmenu_key key) {
    switch (key) {
    case TMENU_KEY_CONFIRM:
        if (!swapped(m)) return save_action(m);
        m->screen = TMENU_SCREEN_MAIN;
        return TMENU_RESULT_REDRAW;
    case TMENU_KEY_BACK:
        if (swapped(m)) return save_action(m);
        m->screen = TMENU_SCREEN_MAIN;
        return TMENU_RESULT_REDRAW;
    case TMENU_KEY_RIGHT:
        m->save_index = m->save_index >= m->slot_count - 1 ? 0 : m->save_index + 1;
        return TMENU_RESULT_REDRAW;
    case TMENU_KEY_LEFT:
        m->save_index = m->save_index <= 0 ? (m->slot_count > 0 ? m->slot_count : 1) - 1 : m->save_index - 1;
        return TMENU_RESULT_REDRAW;
    default: return TMENU_RESULT_NONE;
    }
}

static enum tmenu_result load_key(struct tmenu_menu *m, enum tmenu_key key) {
    switch (key) {
    case TMENU_KEY_CONFIRM:
        if (!swapped(m)) return load_action(m);
        m->screen = TMENU_SCREEN_MAIN;
        return TMENU_RESULT_REDRAW;
    case TMENU_KEY_BACK:
        if (swapped(m)) return load_action(m);
        m->screen = TMENU_SCREEN_MAIN;
        return TMENU_RESULT_REDRAW;
    case TMENU_KEY_RIGHT:
        if (m->saved_count > 0)
            m->load_index = m->load_index >= m->saved_count - 1 ? 0 : m->load_index + 1;
        return TMENU_RESULT_REDRAW;
    case TMENU_KEY_LEFT:
        if (m->saved_count > 0)
            m->load_index = m->load_index <= 0 ? m->saved_count - 1 : m->load_index - 1;
        return TMENU_RESULT_REDRAW;
    default: return TMENU_RESULT_NONE;
    }
}

enum tmenu_result tmenu_menu_key(struct tmenu_menu *m, enum tmenu_key key) {
    if (key == TMENU_KEY_NONE) return TMENU_RESULT_NONE;
    switch (m->screen) {
    case TMENU_SCREEN_SAVE: return save_key(m, key);
    case TMENU_SCREEN_LOAD: return load_key(m, key);
    default: return main_key(m, key);
    }
}

int tmenu_menu_notification(struct tmenu_menu *m, char text[TMENU_NOTIFY_BYTES + 1]) {
    if (!m->notify[0]) return 0;
    memcpy(text, m->notify, TMENU_NOTIFY_BYTES + 1);
    if (m->ops.now_ms(m->ops.context) - m->notify_ms > TMENU_NOTIFY_MS) m->notify[0] = 0;
    return 1;
}

int tmenu_menu_return_value(enum tmenu_result result) {
    return result == TMENU_RESULT_ADVANCED || result == TMENU_RESULT_NETPLAY ? (int)result : 1;
}
