#include "model.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const int mainui_picture_sizes[3] = {150, 50, 100};
const int mainui_dim_times[6] = {120, 300, 600, 900, 1800, -1};

static const int keymaps[2] = {0, 1};
static const int dpad_keys[7] = {0, 1, 2, 3, 4, 5, 6};  // Off, F1, F2, R2, L2, SELECT, START
static const int two_values[2] = {0, 1};
// Keep persisted firmware indexes; index 0 is a legacy preference, not a choice.
static const int osd_shortcuts[MAINUI_OSD_SHORTCUTS - 1] = {
    1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24,
    25, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40, 41, 42, 43, 44, 45, 46, 47, 48, 49,
};
const char *const mainui_osd_shortcut_names[MAINUI_OSD_SHORTCUTS] = {
    "MENU+SELECT", "MENU+START", "MENU+MENU", "MENU+L2", "MENU+R2", "MENU+L", "MENU+R", "MENU+A", "MENU+B",
    "MENU+X", "MENU+Y", "SELECT+START", "SELECT+L2", "SELECT+R2", "SELECT+L", "SELECT+R", "START+L2",
    "START+R2", "START+L", "START+R", "MENU+L2+R2", "MENU+F1+F2", "F1", "F2", "F1+F2", "F1+F1", "F2+F2",
    "F1+MENU", "F1+SELECT", "F1+START", "F1+L", "F1+L2", "F1+R", "F1+R2", "F1+A", "F1+B", "F1+X", "F1+Y",
    "F2+MENU", "F2+SELECT", "F2+START", "F2+L", "F2+L2", "F2+R", "F2+R2", "F2+A", "F2+B", "F2+X", "F2+Y", "OFF",
};
static const int usb_modes[3] = {0, 1, 2};              // Host, Dock, Data

// Button Mapping: the layouts, and each key row's destinations, in the order of enum
// mainui_map_dest without the row's own key.
static const int map_layouts[2] = {MAINUI_MAP_NINTENDO, MAINUI_MAP_XBOX};
#define D(name) MAINUI_MAP_##name
static const int map_select[MAINUI_MAP_DESTS - 1] = {
    D(DEFAULT), D(A), D(B), D(X), D(Y), D(L1), D(R1), D(START), D(MENU), D(F1), D(F2), D(VOLDOWN), D(VOLUP), D(DISABLED),
};
static const int map_start[MAINUI_MAP_DESTS - 1] = {
    D(DEFAULT), D(A), D(B), D(X), D(Y), D(L1), D(R1), D(SELECT), D(MENU), D(F1), D(F2), D(VOLDOWN), D(VOLUP), D(DISABLED),
};
static const int map_f1[MAINUI_MAP_DESTS - 1] = {
    D(DEFAULT), D(A), D(B), D(X), D(Y), D(L1), D(R1), D(START), D(SELECT), D(MENU), D(F2), D(VOLDOWN), D(VOLUP), D(DISABLED),
};
static const int map_f2[MAINUI_MAP_DESTS - 1] = {
    D(DEFAULT), D(A), D(B), D(X), D(Y), D(L1), D(R1), D(START), D(SELECT), D(MENU), D(F1), D(VOLDOWN), D(VOLUP), D(DISABLED),
};
static const int map_volup[MAINUI_MAP_DESTS - 1] = {
    D(DEFAULT), D(A), D(B), D(X), D(Y), D(L1), D(R1), D(START), D(SELECT), D(MENU), D(F1), D(F2), D(VOLDOWN), D(DISABLED),
};
static const int map_voldown[MAINUI_MAP_DESTS - 1] = {
    D(DEFAULT), D(A), D(B), D(X), D(Y), D(L1), D(R1), D(START), D(SELECT), D(MENU), D(F1), D(F2), D(VOLUP), D(DISABLED),
};
#undef D
const char *const mainui_map_dest_names[MAINUI_MAP_DESTS] = {
    "Default", "A", "B", "X", "Y", "L1", "R1", "Start", "Select", "Menu", "F1", "F2", "Vol-", "Vol+", "Disabled",
};

// LED Policy's options in order, and the Custom rows' values.
static const int led_options[MAINUI_LED_OPTIONS] = {
    MAINUI_LED_DEFAULT, MAINUI_LED_FOLLOW_THEME, MAINUI_LED_BATTERY, MAINUI_LED_CPU, MAINUI_LED_TEMPERATURE,
    MAINUI_LED_CHARGING, MAINUI_LED_CUSTOM,
};
static const int led_effects[MAINUI_LED_EFFECTS] = {0, 1, 2, 3, 4, 5, 6, 7};
// BrickOS Appearance's options, in the order their rows step through them.
static const int appear_accents[MAINUI_ACCENTS] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9};
static const int appear_backgrounds[MAINUI_BACKGROUNDS] = {0, 1, 2};
static const int appear_text_sizes[MAINUI_TEXT_SIZES] = {
    MAINUI_TEXT_SMALL, MAINUI_TEXT_NORMAL, MAINUI_TEXT_LARGE, MAINUI_TEXT_EXTRA_LARGE,
};
static const int appear_shapes[MAINUI_SHAPES] = {0, 1, 2};
static const int appear_dimmings[MAINUI_DIMMINGS] = {MAINUI_DIM_NONE, MAINUI_DIM_SOFT, MAINUI_DIM_STRONG};
static const int appear_halos[MAINUI_HALOS] = {0, 1, 2};
static const int appear_motions[MAINUI_MOTIONS] = {0, 1, 2};
static const int appear_views[MAINUI_VIEWS] = {MAINUI_VIEW_LIST, MAINUI_VIEW_GRID, MAINUI_VIEW_CAROUSEL};
static const int led_colours[MAINUI_LED_COLOURS] = {0, 1, 2, 3, 4, 5, 6, 7};

int mainui_model_led_setting(int setting) {
    return setting >= MAINUI_SET_LED_OPTION && setting <= MAINUI_SET_LED_TIME;
}

int mainui_model_appearance_setting(int setting) {
    return setting >= MAINUI_SET_APPEAR_ACCENT && setting <= MAINUI_SET_APPEAR_LAST;
}

int mainui_model_mapping_setting(int setting) {
    return setting >= MAINUI_SET_MAP_LAYOUT && setting <= MAINUI_SET_MAP_VOLDOWN;
}

int mainui_model_hold_remapped(const struct mainui_model *m) {
    // Dpad to Analog Key(hold): Off, F1, F2, R2, L2, SELECT, START. Any binding, Custom
    // included, takes the key out of the daemon's stock path and so out of its hold role.
    static const int keys[7] = {-1, MAINUI_SET_MAP_F1, MAINUI_SET_MAP_F2, -1, -1, MAINUI_SET_MAP_SELECT, MAINUI_SET_MAP_START};
    int hold = m->settings[MAINUI_SET_DPAD2AXIS];
    return hold > 0 && hold < 7 && keys[hold] >= 0 && m->settings[keys[hold]] != MAINUI_MAP_DEFAULT;
}

void mainui_settings_defaults(int settings[MAINUI_SET_COUNT]) {
    static const int values[MAINUI_SET_COUNT] = {
        [MAINUI_SET_VOL] = 2, [MAINUI_SET_BRIGHTNESS] = 5, [MAINUI_SET_WIFI] = 1,
        [MAINUI_SET_OSDSHORTCUT] = 1,
        [MAINUI_SET_TOPLED] = 4, [MAINUI_SET_SHOULDERLED] = 4, [MAINUI_SET_F1F2LED] = 4,
        [MAINUI_SET_LEDPOLICY] = 1, [MAINUI_SET_MENUSTYLEL1] = 1, [MAINUI_SET_USENETWORKTIME] = 1,
        [MAINUI_SET_24HOURCLOCK] = 1, [MAINUI_SET_DIMTIME] = 300, [MAINUI_SET_COLORBRIGHTNESS] = 5,
        [MAINUI_SET_COLORCONTRAST] = 5, [MAINUI_SET_COLORTEMPERATURE] = 10,
        [MAINUI_SET_COLORSATURATION] = 5, [MAINUI_SET_USBMODE] = 2, [MAINUI_SET_ENABLEHOME] = 1,
        [MAINUI_SET_FOCUSONTAB] = 1, [MAINUI_SET_PICTURESIZE] = 150, [MAINUI_SET_FAVORITETAB] = 1,
        [MAINUI_SET_RECENTPLAYEDTAB] = 1, [MAINUI_SET_BESTSTAB] = 1, [MAINUI_SET_RETROGAMETAB] = 1,
        [MAINUI_SET_PORTSTAB] = 1, [MAINUI_SET_APPSTAB] = 1, [MAINUI_SET_NETPLAYTAB] = 1,
        [MAINUI_SET_ENABLEKEYTONE] = 1,
        // ledpolicy 1 reads Follow Theme; Custom starts on BrickTools' linear red, 1 second.
        [MAINUI_SET_LED_OPTION] = MAINUI_LED_FOLLOW_THEME, [MAINUI_SET_LED_EFFECT] = 1, [MAINUI_SET_LED_COLOUR] = 1,
        [MAINUI_SET_LED_TIME] = 1,
    };
    memcpy(settings, values, sizeof(values));
}

// ---- Effects and helpers -------------------------------------------------------------------

static struct mainui_effect *effect(struct mainui_model *m, enum mainui_effect_kind kind) {
    static struct mainui_effect overflow;
    if (m->effect_count == MAINUI_EFFECTS_MAX) {
        ++m->effects_dropped;
        memset(&overflow, 0, sizeof(overflow));
        return &overflow;
    }
    struct mainui_effect *e = &m->effects[(m->effect_head + m->effect_count++) % MAINUI_EFFECTS_MAX];
    memset(e, 0, sizeof(*e));
    e->kind = kind;
    e->list = e->system = e->index = e->launcher = e->setting = -1;
    return e;
}

int mainui_model_next_effect(struct mainui_model *m, struct mainui_effect *out) {
    if (!m->effect_count) return 0;
    *out = m->effects[m->effect_head];
    m->effect_head = (m->effect_head + 1) % MAINUI_EFFECTS_MAX;
    --m->effect_count;
    return 1;
}

static void set_setting(struct mainui_model *m, enum mainui_setting setting, int value) {
    m->settings[setting] = value;
    if (setting == MAINUI_SET_VOL) m->settings[MAINUI_SET_MUTE] = 0;
    struct mainui_effect *e = effect(m, MAINUI_EFFECT_SET);
    e->setting = (int)setting;
    e->value = value;
}

static void persist(struct mainui_model *m) { effect(m, MAINUI_EFFECT_PERSIST); }

static int count(const struct mainui_model *m, enum mainui_list list, int arg) {
    int n = m->catalog.count ? m->catalog.count(m->catalog.context, list, arg) : 0;
    return n > 0 ? n : 0;
}

static int entry_flags(const struct mainui_model *m, enum mainui_list list, int arg, int index) {
    return m->catalog.entry_flags ? m->catalog.entry_flags(m->catalog.context, list, arg, index) : 0;
}

static struct mainui_window *top(struct mainui_model *m) { return &m->stack[m->depth - 1]; }
const struct mainui_window *mainui_model_top(const struct mainui_model *m) { return &m->stack[m->depth - 1]; }

static struct mainui_window *push(struct mainui_model *m, enum mainui_window_kind kind) {
    if (m->depth == MAINUI_STACK_MAX) return NULL;
    struct mainui_window *w = &m->stack[m->depth++];
    memset(w, 0, sizeof(*w));
    w->kind = kind;
    w->focus = 0;
    w->system = w->list = w->index = -1;
    return w;
}

static void pop(struct mainui_model *m) {
    if (m->depth > 1) --m->depth;
}

// Keeps focus within [first, first + rows) with the least scrolling.
static void keep_visible(int *first, int focus, int rows, int n) {
    if (focus < *first) *first = focus;
    if (focus >= *first + rows) *first = focus - rows + 1;
    int last_first = n > rows ? n - rows : 0;
    if (*first > last_first) *first = last_first;
    if (*first < 0) *first = 0;
}

// ---- Tabs ----------------------------------------------------------------------------------

static int tab_visible(const struct mainui_model *m, enum mainui_tab tab) {
    const int *s = m->settings;
    int content = m->catalog.has_content && m->catalog.has_content(m->catalog.context, tab);
    switch (tab) {
    case MAINUI_TAB_FAVORITES: return s[MAINUI_SET_FAVORITETAB] != 0;
    case MAINUI_TAB_RECENT: return s[MAINUI_SET_RECENTPLAYEDTAB] != 0;
    case MAINUI_TAB_BESTS: return s[MAINUI_SET_BESTSTAB] && content;
    case MAINUI_TAB_GAMES: return s[MAINUI_SET_RETROGAMETAB] != 0;
    case MAINUI_TAB_PORTS: return s[MAINUI_SET_PORTSTAB] && content;
    case MAINUI_TAB_APPS: return s[MAINUI_SET_APPSTAB] != 0;
    // The Netplay switch does not hide the tab; it shows with netplay 0 and 1 (k2c2).
    case MAINUI_TAB_NETPLAY: return s[MAINUI_SET_NETPLAYTAB] != 0;
    case MAINUI_TAB_SETTINGS: return 1;
    default: return 0;
    }
}

int mainui_model_visible_tabs(const struct mainui_model *m, enum mainui_tab tabs[MAINUI_TAB_COUNT]) {
    int n = 0;
    for (int tab = 0; tab < MAINUI_TAB_COUNT; ++tab)
        if (tab_visible(m, (enum mainui_tab)tab)) tabs[n++] = (enum mainui_tab)tab;
    return n;
}

// GameSwitcher's card is a BrickOS desktop destination while its app is installed.
static int gameswitcher_installed(const struct mainui_model *m) {
    return m->presentation == MAINUI_PRESENTATION_MATERIAL_YOU && m->catalog.gameswitcher &&
           m->catalog.gameswitcher(m->catalog.context);
}

int mainui_model_home_items(const struct mainui_model *m, int items[MAINUI_HOME_COUNT]) {
    // GameSwitcher sits where Recent is, after it, or in its place when Recent is hidden.
    static const int material_order[MAINUI_HOME_COUNT] = {
        MAINUI_TAB_GAMES, MAINUI_TAB_BESTS, MAINUI_TAB_FAVORITES, MAINUI_TAB_RECENT, MAINUI_HOME_GAMESWITCHER,
        MAINUI_TAB_APPS, MAINUI_TAB_PORTS, MAINUI_TAB_NETPLAY, MAINUI_TAB_SETTINGS,
    };
    int n = 0;
    if (m->presentation != MAINUI_PRESENTATION_MATERIAL_YOU) {
        enum mainui_tab tabs[MAINUI_TAB_COUNT];
        n = mainui_model_visible_tabs(m, tabs);
        for (int i = 0; i < n; ++i) items[i] = tabs[i];
        return n;
    }
    for (int i = 0; i < MAINUI_HOME_COUNT; ++i) {
        int item = material_order[i];
        if (item == MAINUI_HOME_GAMESWITCHER ? gameswitcher_installed(m) : tab_visible(m, (enum mainui_tab)item))
            items[n++] = item;
    }
    return n;
}

int mainui_desktop_position(int index, int count, int *column, int *row) {
    if (index < 0 || index >= count || !column || !row || count > MAINUI_DESKTOP_COLUMNS * MAINUI_DESKTOP_ROWS) return -1;
    *column = index % MAINUI_DESKTOP_COLUMNS;
    *row = index / MAINUI_DESKTOP_COLUMNS;
    return 0;
}

int mainui_desktop_move(int index, int count, enum mainui_key key) {
    int column, row;
    if (mainui_desktop_position(index, count, &column, &row) < 0) return -1;
    switch (key) {
    case MAINUI_KEY_LEFT: return column > 0 ? index - 1 : index;
    case MAINUI_KEY_RIGHT: return column + 1 < MAINUI_DESKTOP_COLUMNS && index + 1 < count ? index + 1 : index;
    case MAINUI_KEY_UP: return row > 0 ? index - MAINUI_DESKTOP_COLUMNS : index;
    case MAINUI_KEY_DOWN:
        if (count <= MAINUI_DESKTOP_COLUMNS || row + 1 >= MAINUI_DESKTOP_ROWS) return index;
        if (index + MAINUI_DESKTOP_COLUMNS < count) return index + MAINUI_DESKTOP_COLUMNS;
        // On a partial second row, move to the nearest existing card in that row.
        return MAINUI_DESKTOP_COLUMNS +
               (column < count - MAINUI_DESKTOP_COLUMNS ? column : count - MAINUI_DESKTOP_COLUMNS - 1);
    default: return index;
    }
}

// The Settings tab keeps the paged grid in every layout (h4, h5; static: TabMenuWindow::initTab
// makes it a fixed 2x3 IconMenu).
int mainui_model_tab_layout(const struct mainui_model *m, enum mainui_tab tab) {
    return tab == MAINUI_TAB_SETTINGS ? 1 : tab == MAINUI_TAB_BESTS ? 0 : m->settings[MAINUI_SET_MENUSTYLEL1];
}

static int tab_position(const struct mainui_model *m, enum mainui_tab tab, enum mainui_tab tabs[MAINUI_TAB_COUNT], int *n) {
    *n = mainui_model_visible_tabs(m, tabs);
    for (int i = 0; i < *n; ++i)
        if (tabs[i] == tab) return i;
    return 0;
}

// Ports is a page of tiles like Apps, in the level-1 layout (p1, p2).
static int list_tab(enum mainui_tab tab) {
    return tab == MAINUI_TAB_FAVORITES || tab == MAINUI_TAB_RECENT || tab == MAINUI_TAB_NETPLAY;
}

static enum mainui_list tab_list(enum mainui_tab tab) {
    static const enum mainui_list lists[MAINUI_TAB_COUNT] = {
        MAINUI_LIST_FAVORITES, MAINUI_LIST_RECENT, MAINUI_LIST_BESTS, MAINUI_LIST_SYSTEMS,
        MAINUI_LIST_PORTS, MAINUI_LIST_APPS, MAINUI_LIST_NETPLAY, MAINUI_LIST_COUNT,
    };
    return lists[tab];
}

static int extension_tiles(const struct mainui_model *m) {
    const struct mainui_extensions *x = &m->extensions;
    int n = x->tiles ? x->tiles(x->context) : 0;
    return n > 0 ? n : 0;
}

int mainui_model_settings_tiles(const struct mainui_model *m) {
    return MAINUI_SETTINGS_TILES + extension_tiles(m);
}

void mainui_model_set_extensions(struct mainui_model *m, const struct mainui_extensions *extensions) {
    if (extensions) m->extensions = *extensions;
    else memset(&m->extensions, 0, sizeof(m->extensions));
}

static int tab_count(const struct mainui_model *m, enum mainui_tab tab) {
    if (tab == MAINUI_TAB_SETTINGS) return mainui_model_settings_tiles(m);
    return count(m, tab_list(tab), 0);
}

static void open_tabs(struct mainui_model *m, enum mainui_tab tab, int bar_focused) {
    m->depth = 1;
    m->stack[0].kind = MAINUI_WIN_TABS;
    m->tab = tab;
    m->bar_focused = bar_focused;
}

// The home row shows four shortcuts and keeps one beyond the focus on each side while it can:
// moving right the focus stays in the third slot, moving left in the second (p5).
static void home_fit(struct mainui_model *m) {
    int items[MAINUI_HOME_COUNT];
    int n = mainui_model_home_items(m, items), pos = 0;
    while (pos < n && items[pos] != m->home_focus) ++pos;
    if (pos >= n) return;
    if (m->presentation == MAINUI_PRESENTATION_MATERIAL_YOU) {
        if (n <= MAINUI_MATERIAL_HOME_VISIBLE || pos <= 1) m->home_first = 0;
        else if (pos >= n - 2) m->home_first = n - MAINUI_MATERIAL_HOME_VISIBLE;
        else m->home_first = pos - 1;
        if (m->home_first < 0) m->home_first = 0;
        return;
    }
    if (pos < m->home_first + 1) m->home_first = pos - 1;
    if (pos > m->home_first + 2) m->home_first = pos - 2;
    if (m->home_first > n - 4) m->home_first = n - 4;
    if (m->home_first < 0) m->home_first = 0;
}

static void open_home(struct mainui_model *m) {
    home_fit(m);
    m->depth = 1;
    memset(&m->stack[0], 0, sizeof(m->stack[0]));
    m->stack[0].kind = MAINUI_WIN_HOME;
    m->home_search = 0;
}

static int material_home_move(int index, int count, enum mainui_key key) {
    if (count <= 0) return -1;
    if (key == MAINUI_KEY_LEFT) return index > 0 ? index - 1 : 0;
    if (key == MAINUI_KEY_RIGHT) return index + 1 < count ? index + 1 : count - 1;
    return index;
}

void mainui_model_init(struct mainui_model *m, const int settings[MAINUI_SET_COUNT],
                       const struct mainui_catalog *catalog) {
    memset(m, 0, sizeof(*m));
    memcpy(m->settings, settings, sizeof(m->settings));
    // ledpolicy 1 always reads Follow Theme, and 0 never does.
    int *option = &m->settings[MAINUI_SET_LED_OPTION];
    if (m->settings[MAINUI_SET_LEDPOLICY] == 1) *option = MAINUI_LED_FOLLOW_THEME;
    else if (*option == MAINUI_LED_FOLLOW_THEME || *option < 0 || *option >= MAINUI_LED_OPTIONS) *option = MAINUI_LED_DEFAULT;
    if (catalog) m->catalog = *catalog;
    enum mainui_tab tabs[MAINUI_TAB_COUNT];
    int n = mainui_model_visible_tabs(m, tabs);
    enum mainui_tab start = tabs[0];
    for (int i = 0; i < n; ++i)
        if (tabs[i] == MAINUI_TAB_GAMES) start = MAINUI_TAB_GAMES;
    // The home row opens on Recent.
    m->home_focus = tab_visible(m, MAINUI_TAB_RECENT) ? MAINUI_TAB_RECENT : tabs[0];
    open_tabs(m, start, 0);
}

static int same_row(const struct mainui_item *a, const struct mainui_item *b) {
    return a->kind == b->kind && a->screen == b->screen && a->setting == b->setting && a->action == b->action &&
           strcmp(a->label ? a->label : "", b->label ? b->label : "") == 0;
}

static struct mainui_item *window_rows(const struct mainui_model *m, const struct mainui_window *w, int *n) {
    int capacity = mainui_model_screen_item_capacity(m, w->screen);
    struct mainui_item *items = calloc((size_t)(capacity > 0 ? capacity : 1), sizeof(*items));
    *n = items ? mainui_model_screen_items(m, w->screen, items, capacity) : 0;
    return items;
}

// A frontend switch adds or removes the rows only one presentation shows (Themes' Theme row,
// Homepage's Default destination). Each Settings window keeps the row it had focused, found
// again as the same nth row of its kind; a row that is gone leaves the focus clamped.
static void keep_focused_rows(struct mainui_model *m, enum mainui_presentation presentation) {
    enum mainui_presentation old = m->presentation;
    for (int i = 0; i < m->depth; ++i) {
        struct mainui_window *w = &m->stack[i];
        // Extension menus have the same rows in both presentations.
        if (w->kind != MAINUI_WIN_SETTINGS || w->screen == MAINUI_SCREEN_EXTENSION) continue;
        int before_n, after_n;
        struct mainui_item *before = window_rows(m, w, &before_n);
        m->presentation = presentation;
        struct mainui_item *after = window_rows(m, w, &after_n);
        m->presentation = old;
        if (before && after && w->focus >= 0 && w->focus < before_n && after_n > 0) {
            int nth = 0, focus = w->focus < after_n ? w->focus : after_n - 1;
            for (int k = 0; k < w->focus; ++k) nth += same_row(&before[k], &before[w->focus]);
            for (int k = 0, seen = 0; k < after_n; ++k)
                if (same_row(&after[k], &before[w->focus]) && seen++ == nth) { focus = k; break; }
            if (focus != w->focus) {
                w->focus = focus;
                keep_visible(&w->first, focus, 6, after_n);
            }
        }
        free(before);
        free(after);
    }
}

void mainui_model_set_presentation(struct mainui_model *m, enum mainui_presentation presentation,
                                   int valid_return) {
    if (!m || (presentation != MAINUI_PRESENTATION_STOCK && presentation != MAINUI_PRESENTATION_MATERIAL_YOU)) return;
    keep_focused_rows(m, presentation);
    m->presentation = presentation;
    // Stock's home row has only tabs: the GameSwitcher card's focus goes to Recent's place.
    if (presentation == MAINUI_PRESENTATION_STOCK && m->home_focus >= MAINUI_TAB_COUNT)
        m->home_focus = MAINUI_TAB_RECENT;
    // Preserve a validated launch-return stack. A fresh BrickOS cycle starts on its desktop;
    // switching back to stock leaves the stock stack as it was.
    if (presentation == MAINUI_PRESENTATION_MATERIAL_YOU && !valid_return && m->depth == 1 &&
        m->stack[0].kind == MAINUI_WIN_TABS) {
        int items[MAINUI_HOME_COUNT];
        int n = mainui_model_home_items(m, items);
        m->home_focus = MAINUI_TAB_GAMES;
        for (int i = 0; i < n; ++i)
            if (items[i] == MAINUI_TAB_GAMES) { m->home_focus = MAINUI_TAB_GAMES; break; }
            else if (i == n - 1) m->home_focus = items[0];
        m->home_search = 0;
        m->home_first = 0;
        open_home(m);
    }
}

void mainui_model_apply_brickos_initial_view(struct mainui_model *m, enum mainui_brickos_initial_view view) {
    if (!m) return;
    if ((unsigned)view >= MAINUI_BRICKOS_VIEW_COUNT) view = MAINUI_BRICKOS_VIEW_DESKTOP;
    m->brickos_initial_view = view;
    if (m->presentation != MAINUI_PRESENTATION_MATERIAL_YOU || view == MAINUI_BRICKOS_VIEW_LAST_USED) return;
    if (view == MAINUI_BRICKOS_VIEW_DESKTOP) {
        open_home(m);
        return;
    }
    // GameSwitcher opens at once over the desktop, whose card is focused for the return; without
    // its app the desktop opens and the choice stays. A start does not add it to Recent.
    if (view == MAINUI_BRICKOS_VIEW_GAMESWITCHER) {
        if (gameswitcher_installed(m)) {
            m->home_focus = MAINUI_HOME_GAMESWITCHER;
            effect(m, MAINUI_EFFECT_LAUNCH_GAMESWITCHER)->value = 0;
        }
        open_home(m);
        return;
    }
    enum mainui_tab tab = view == MAINUI_BRICKOS_VIEW_GAMES ? MAINUI_TAB_GAMES :
                          view == MAINUI_BRICKOS_VIEW_FAVORITES ? MAINUI_TAB_FAVORITES :
                          view == MAINUI_BRICKOS_VIEW_RECENT ? MAINUI_TAB_RECENT : MAINUI_TAB_APPS;
    if (!tab_visible(m, tab)) {
        open_home(m);
        return;
    }
    open_tabs(m, tab, 0);
}

// ---- Resume after a launch -----------------------------------------------------------------

int mainui_model_resume_state(const struct mainui_model *m, struct mainui_resume *r) {
    enum mainui_tab tabs[MAINUI_TAB_COUNT];
    memset(r, 0, sizeof(*r));
    r->system = r->search_system = -1;
    int end = m->depth;
    while (end > 1 && m->stack[end - 1].kind == MAINUI_WIN_FILES) {
        ++r->files;
        --end;
    }
    if (end > 1 && m->stack[end - 1].kind == MAINUI_WIN_SEARCH) {
        const struct mainui_window *w = &m->stack[--end];
        r->search = 1;
        r->search_system = w->system;
        r->search_focus = w->focus;
        r->search_first = w->first;
        memcpy(r->query, w->text, sizeof(r->query));
    }
    if (end > 1 && m->stack[end - 1].kind == MAINUI_WIN_KEYBOARD &&
        m->stack[end - 1].purpose == MAINUI_TYPE_SEARCH) {
        r->keyboard = 1;
        if (!r->search) r->search_system = m->stack[end - 1].system;
        --end;
    }
    r->home = m->stack[0].kind == MAINUI_WIN_HOME;
    // The BrickOS desktop is itself a complete last-used view. Preserve stock's
    // existing rule that a plain Home screen is not a launch-return state.
    if (r->home ? (end != 1 || (m->presentation != MAINUI_PRESENTATION_MATERIAL_YOU &&
                                !r->keyboard && !r->search)) : m->stack[0].kind != MAINUI_WIN_TABS) return -1;
    for (int i = 1; i < end; ++i) {
        const struct mainui_window *w = &m->stack[i];
        if (w->kind != MAINUI_WIN_ROMS || r->rom_count == MAINUI_STACK_MAX - 1) return -1;
        int at = r->rom_count++;
        r->roms[at].system = w->system; r->roms[at].focus = w->focus; r->roms[at].first = w->first;
        memcpy(r->roms[at].path, w->rompath, sizeof(w->rompath));
    }
    r->tab_count = mainui_model_visible_tabs(m, tabs);
    // The GameSwitcher card is kept as a flag beside the tab before it on the desktop, so the
    // stock state still names a tab.
    int home_tab = m->home_focus;
    if (r->home && m->home_focus == MAINUI_HOME_GAMESWITCHER) {
        int items[MAINUI_HOME_COUNT], count = mainui_model_home_items(m, items);
        r->home_gameswitcher = 1;
        home_tab = -1;
        for (int i = 0; i < count && items[i] != MAINUI_HOME_GAMESWITCHER; ++i) home_tab = items[i];
        if (home_tab < 0) for (int i = 0; i < count; ++i) if (items[i] != MAINUI_HOME_GAMESWITCHER) { home_tab = items[i]; break; }
    }
    for (int i = 0; i < r->tab_count; ++i) {
        r->tabs[i] = m->tabs[tabs[i]];
        if (tabs[i] == m->tab) r->tab_index = i;
        if ((int)tabs[i] == home_tab) r->home_focus = i;
    }
    r->home_first = m->home_first;
    if (r->rom_count) {
        r->system = m->stack[1].system;
        r->focus = m->stack[1].focus;
        r->first = m->stack[1].first;
    }
    return 0;
}

void mainui_model_resume(struct mainui_model *m, const struct mainui_resume *r) {
    enum mainui_tab tabs[MAINUI_TAB_COUNT];
    int n = mainui_model_visible_tabs(m, tabs);
    if (r->tab_index < 0 || r->tab_index >= n) return;
    open_tabs(m, tabs[r->tab_index], 0);
    if (r->home) {
        int items[MAINUI_HOME_COUNT], count = mainui_model_home_items(m, items);
        m->stack[0].kind = MAINUI_WIN_HOME;
        m->home_focus = tabs[r->home_focus >= 0 && r->home_focus < n ? r->home_focus : 0];
        if (r->home_gameswitcher && gameswitcher_installed(m)) m->home_focus = MAINUI_HOME_GAMESWITCHER;
        m->home_first = r->home_first >= 0 && r->home_first < count ? r->home_first : 0;
        m->home_search = 0;
        home_fit(m);
    }
    for (int i = 0; i < n && i < r->tab_count; ++i) {
        struct mainui_tab_state *t = &m->tabs[tabs[i]];
        int items = tab_count(m, tabs[i]);
        t->focus = r->tabs[i].focus >= 0 && r->tabs[i].focus < items ? r->tabs[i].focus : 0;
        t->first = r->tabs[i].first > 0 ? r->tabs[i].first : 0;
        if (list_tab(tabs[i])) keep_visible(&t->first, t->focus, MAINUI_TAB_LIST_ROWS, items);
        else t->first = 0;
    }
    m->refresh_tab_count = r->tab_count > 0 ?
        (r->tab_count <= MAINUI_TAB_COUNT ? r->tab_count : MAINUI_TAB_COUNT + 1) : 0;
    for (int i = 0; i < n && i < r->tab_count; ++i)
        m->refresh_tabs[i] = m->tabs[tabs[i]];
    int base = m->tab == MAINUI_TAB_BESTS ? MAINUI_BEST_SYSTEM_BASE : 0;
    int roms = r->rom_count > 0 && r->rom_count < MAINUI_STACK_MAX ? r->rom_count : r->system >= 0 ? 1 : 0;
    for (int i = 0; !r->home && i < roms && (m->tab == MAINUI_TAB_GAMES || m->tab == MAINUI_TAB_BESTS); ++i) {
        int system = i ? r->roms[i].system : r->system;
        if (system < base || system >= base + tab_count(m, m->tab)) break;
        struct mainui_window *w = push(m, MAINUI_WIN_ROMS);
        if (w) {
            w->system = system;
            if (r->rom_count) memcpy(w->rompath, r->roms[i].path, sizeof(w->rompath));
            int games = count(m, MAINUI_LIST_ROMS, system);
            w->resumed = 1;
            int focus = i ? r->roms[i].focus : r->focus;
            int first = i ? r->roms[i].first : r->first;
            w->focus = focus >= 0 && focus < games ? focus : 0;
            w->first = first > 0 ? first : 0;
            keep_visible(&w->first, w->focus, MAINUI_ROM_LIST_ROWS, games);
        }
    }
    if (r->keyboard) {
        struct mainui_window *w = push(m, MAINUI_WIN_KEYBOARD);
        if (w) { w->purpose = MAINUI_TYPE_SEARCH; w->system = r->search_system; }
    }
    if (r->search) {
        struct mainui_window *w = push(m, MAINUI_WIN_SEARCH);
        int hits = count(m, MAINUI_LIST_SEARCH, 0);
        if (w) {
            w->system = r->search_system;
            memcpy(w->text, r->query, sizeof(w->text));
            w->focus = r->search_focus >= 0 && r->search_focus < hits ? r->search_focus : 0;
            w->first = r->search_first > 0 ? r->search_first : 0;
            keep_visible(&w->first, w->focus, mainui_search_rows(w->system), hits);
        }
    }
}

// ---- Grids ---------------------------------------------------------------------------------

// The paged grid (HallMenu): 3 columns, 2 rows per page. Right past a row's end goes to the
// same row of the next page (the first page after the last); left past column 0 goes to the
// same row of the previous page, except that on the first page it goes to the last item. Down
// moves between the two rows of the page; up from the top row leaves the grid (returns -1).
static int tiled_move(int index, int n, enum mainui_key key, int columns, int rows) {
    const int per_page = columns * rows;
    if (n <= 0) return index;
    int page = index / per_page, pos = index % per_page, row = pos / columns, col = pos % columns;
    int pages = (n + per_page - 1) / per_page;
    int target = index;
    switch (key) {
    case MAINUI_KEY_RIGHT:
        if (col < columns - 1 && index + 1 < n) return index + 1;
        page = (page + 1) % pages;
        // The stock endpoint calculation uses the preceding column to choose
        // the row. A final item at column zero wraps to the preceding row.
        if (page == 0) row = pos > 0 ? (pos - 1) / columns : 0;
        target = page * per_page + row * columns;
        if (target >= n) target = page * per_page + ((n - page * per_page - 1) / columns) * columns;
        break;
    case MAINUI_KEY_LEFT:
        if (col > 0) return index - 1;
        if (page == 0) return n - 1;
        target = (page - 1) * per_page + row * columns + columns - 1;
        break;
    case MAINUI_KEY_DOWN:
        target = page * per_page + ((row + 1) % rows) * columns + col;
        break;
    case MAINUI_KEY_UP:
        if (row == 0) return -1;
        return index - columns;
    default:
        return index;
    }
    // A missing item on a partial page falls back to the last item.
    return target < n ? target : n - 1;
}

static int grid_move(int index, int n, enum mainui_key key) {
    return tiled_move(index, n, key, MAINUI_GRID_COLUMNS, MAINUI_GRID_ROWS);
}

// IconMenu's compact layout has five rows of eight columns per page. Its
// horizontal page transitions preserve rows, including partial-row endpoints.
static int compact_move(int index, int n, enum mainui_key key) {
    return tiled_move(index, n, key, MAINUI_COMPACT_COLUMNS, MAINUI_COMPACT_ROWS);
}

// The one-row layout: left and right move by one and wrap at both ends (r9); up leaves the page
// for the tab bar and down does nothing (r10).
static int row_move(int index, int n, enum mainui_key key) {
    if (n <= 0) return index;
    switch (key) {
    case MAINUI_KEY_RIGHT: return index + 1 < n ? index + 1 : 0;
    case MAINUI_KEY_LEFT: return index > 0 ? index - 1 : n - 1;
    case MAINUI_KEY_UP: return -1;
    default: return index;
    }
}

// ---- Popups and dialogs ---------------------------------------------------------------------

static void open_popup(struct mainui_model *m, enum mainui_popup popup, int list, int system, int index) {
    struct mainui_window *w = push(m, MAINUI_WIN_POPUP);
    if (!w) return;
    w->popup = popup;
    w->list = list;
    w->system = system;
    w->index = index;
    int *items = w->items, n = 0;
    switch (popup) {
    case MAINUI_POPUP_TAB:
        if (m->tab == MAINUI_TAB_FAVORITES) {
            items[n++] = MAINUI_POP_REMOVE_FAVORITE;
            items[n++] = MAINUI_POP_CLEAR_FAVORITE;
        } else if (m->tab == MAINUI_TAB_RECENT) {
            // Add Favorite even on an entry that is already a favorite (k2a: rows with the star
            // showed it too); adding one again changes nothing.
            items[n++] = MAINUI_POP_ADD_FAVORITE;
            items[n++] = MAINUI_POP_CLEAR_LIST;
        } else if (m->tab == MAINUI_TAB_GAMES || m->tab == MAINUI_TAB_APPS) {
            items[n++] = MAINUI_POP_EDIT;
        }
        items[n++] = MAINUI_POP_SEARCH_ALL;
        items[n++] = MAINUI_POP_REFRESH_ROMS;
        break;
    case MAINUI_POPUP_ROMS:
        items[n++] = MAINUI_POP_REFRESH_ROMS;
        items[n++] = MAINUI_POP_OPEN;
        if (entry_flags(m, MAINUI_LIST_ROMS, system, index) & MAINUI_ENTRY_FOLDER) {
            items[n++] = MAINUI_POP_SEARCH;
            break;
        }
        items[n++] = MAINUI_POP_ADD_FAVORITE;  // also on a favorite (k2b: GBA rows with the star)
        items[n++] = MAINUI_POP_DELETE;
        items[n++] = MAINUI_POP_SEARCH;
        items[n++] = MAINUI_POP_PICTURE_SIZE;
        break;
    case MAINUI_POPUP_LAUNCHERS: {
        int launchers = count(m, MAINUI_LIST_LAUNCHERS, system);
        for (int i = 0; i < launchers && n < MAINUI_POPUP_ITEMS_MAX; ++i) items[n++] = MAINUI_POP_LAUNCHER + i;
        break;
    }
    case MAINUI_POPUP_SEARCH:
        items[n++] = MAINUI_POP_OPEN;
        items[n++] = MAINUI_POP_ADD_FAVORITE;
        break;
    case MAINUI_POPUP_BT_DEVICE:
        items[n++] = MAINUI_POP_REMOVE_DEVICE;
        break;
    case MAINUI_POPUP_WIFI:
        items[n++] = MAINUI_POP_CONNECT;
        items[n++] = MAINUI_POP_CHANGE_PASSWORD;
        items[n++] = MAINUI_POP_FORGET;
        break;
    }
    w->item_count = n;
}

static void open_dialog(struct mainui_model *m, enum mainui_dialog dialog, int list, int system, int index) {
    struct mainui_window *w = push(m, MAINUI_WIN_DIALOG);
    if (!w) return;
    w->dialog = dialog;
    w->list = list;
    w->system = system;
    w->index = index;
    w->focus = MAINUI_DIALOG_OK;
}

static void open_keyboard(struct mainui_model *m, int system, enum mainui_keyboard_purpose purpose, int index) {
    struct mainui_window *w = push(m, MAINUI_WIN_KEYBOARD);
    if (!w) return;
    w->system = system;
    w->purpose = purpose;
    w->index = index;
}

// Refresh Roms first confirms; its adapter reports completion after clearing caches,
// keymaps and positions under ProgressDialog(53), "Refresh".
// Wi-Fi work runs under its progress window, which the frontend closes when it ends.
static struct mainui_effect *wifi_work(struct mainui_model *m, enum mainui_effect_kind kind, enum mainui_progress progress, int index) {
    struct mainui_effect *e = effect(m, kind);
    e->index = index;
    struct mainui_window *w = push(m, MAINUI_WIN_PROGRESS);
    if (w) w->progress = progress;
    return e;
}

static void refresh_roms(struct mainui_model *m) {
    open_dialog(m, MAINUI_DIALOG_REFRESH_ROMS, -1, -1, -1);
}

static void open_edit(struct mainui_model *m, enum mainui_tab tab) {
    struct mainui_window *w = push(m, MAINUI_WIN_EDIT);
    if (w) w->system = (int)tab;
}

static void launch(struct mainui_model *m, enum mainui_list list, int system, int index, int launcher) {
    struct mainui_effect *e = effect(m, MAINUI_EFFECT_LAUNCH);
    e->list = (int)list;
    e->system = system;
    e->index = index;
    e->launcher = launcher;
}

static void list_effect(struct mainui_model *m, enum mainui_effect_kind kind, int list, int system, int index) {
    struct mainui_effect *e = effect(m, kind);
    e->list = list;
    e->system = system;
    e->index = index;
}

static void popup_choose(struct mainui_model *m) {
    struct mainui_window w = *top(m);
    if (w.item_count == 0) return;
    int item = w.items[w.focus];
    // A row that asks first leaves the popup drawn under its dialog (n04); the dialog closes
    // both.
    if (item != MAINUI_POP_REMOVE_FAVORITE && item != MAINUI_POP_CLEAR_LIST && item != MAINUI_POP_CLEAR_FAVORITE &&
        item != MAINUI_POP_DELETE && item != MAINUI_POP_REMOVE_DEVICE && item != MAINUI_POP_REFRESH_ROMS)
        pop(m);
    if (item >= MAINUI_POP_LAUNCHER) {
        launch(m, MAINUI_LIST_ROMS, w.system, w.index, item - MAINUI_POP_LAUNCHER);
        return;
    }
    switch ((enum mainui_pop)item) {
    case MAINUI_POP_EDIT: open_edit(m, m->tab); break;
    case MAINUI_POP_SEARCH_ALL: open_keyboard(m, -1, MAINUI_TYPE_SEARCH, -1); break;
    case MAINUI_POP_SEARCH: open_keyboard(m, w.system, MAINUI_TYPE_SEARCH, -1); break;
    case MAINUI_POP_REFRESH_ROMS: refresh_roms(m); break;
    case MAINUI_POP_ADD_FAVORITE:
        if (w.index >= 0) list_effect(m, MAINUI_EFFECT_ADD_FAVORITE, w.list, w.system, w.index);
        break;
    case MAINUI_POP_REMOVE_FAVORITE:
        if (w.index >= 0) open_dialog(m, MAINUI_DIALOG_REMOVE_FAVORITE, w.list, w.system, w.index);
        else pop(m);
        break;
    case MAINUI_POP_CLEAR_LIST: open_dialog(m, MAINUI_DIALOG_CLEAR_RECENT, w.list, -1, -1); break;
    case MAINUI_POP_CLEAR_FAVORITE: open_dialog(m, MAINUI_DIALOG_CLEAR_FAVORITE, w.list, -1, -1); break;
    case MAINUI_POP_OPEN:
        if (w.index >= 0) launch(m, (enum mainui_list)w.list, w.system, w.index, -1);
        break;
    case MAINUI_POP_DELETE:
        if (w.index >= 0) open_dialog(m, MAINUI_DIALOG_DELETE_ROM, w.list, w.system, w.index);
        else pop(m);
        break;
    case MAINUI_POP_REMOVE_DEVICE:
        // Remove asks first, over the popup (d1).
        open_dialog(m, MAINUI_DIALOG_REMOVE_DEVICE, w.list, -1, w.index);
        break;
    // A saved network's popup (static: WifiItemAction). Connect selects it again and Forget
    // removes it, each at once under its progress window; Change Password asks the password
    // again and connects with it.
    case MAINUI_POP_CONNECT: wifi_work(m, MAINUI_EFFECT_WIFI_RECONNECT, MAINUI_PROGRESS_WIFI_CONNECT, w.index); break;
    case MAINUI_POP_CHANGE_PASSWORD: open_keyboard(m, -1, MAINUI_TYPE_WIFI_PASSWORD, w.index); break;
    case MAINUI_POP_FORGET: wifi_work(m, MAINUI_EFFECT_WIFI_FORGET, MAINUI_PROGRESS_WIFI_FORGET, w.index); break;
    case MAINUI_POP_PICTURE_SIZE: {
        // The popup stays open and its row shows the new size.
        int next = 0;
        for (int i = 0; i < 3; ++i)
            if (mainui_picture_sizes[i] == m->settings[MAINUI_SET_PICTURESIZE]) next = (i + 1) % 3;
        set_setting(m, MAINUI_SET_PICTURESIZE, mainui_picture_sizes[next]);
        persist(m);
        if (m->depth < MAINUI_STACK_MAX) m->stack[m->depth++] = w;
        break;
    }
    default: break;
    }
}

static void dialog_confirm(struct mainui_model *m) {
    struct mainui_window w = *top(m);
    pop(m);
    if (w.dialog == MAINUI_DIALOG_EXTENSION_CONFIRM) {
        struct mainui_effect *e = effect(m, MAINUI_EFFECT_EXTENSION_ANSWER);
        e->index = w.index;
        e->value = w.focus == MAINUI_DIALOG_OK ? 'A' : 'B';
        return;
    }
    if (top(m)->kind == MAINUI_WIN_POPUP) pop(m);
    if (w.dialog == MAINUI_DIALOG_POWER_OFF && w.focus == MAINUI_DIALOG_REBOOT) {
        // As Power off, with the reboot command and its own progress.
        struct mainui_effect *e = effect(m, MAINUI_EFFECT_REBOOT);
        e->value = w.index == 1;
        struct mainui_window *progress = push(m, MAINUI_WIN_PROGRESS);
        if (progress) {
            progress->progress = MAINUI_PROGRESS_REBOOT;
            progress->index = e->value;
        }
        return;
    }
    if (w.focus != MAINUI_DIALOG_OK) {
        // Stock's editor destructor saves even on Cancel. Discard those edits.
        if (w.dialog == MAINUI_DIALOG_SAVE_EDIT) {
            effect(m, MAINUI_EFFECT_CANCEL_EDIT);
            pop(m);
        }
        return;
    }
    switch (w.dialog) {
    case MAINUI_DIALOG_SAVE_EDIT:
        list_effect(m, MAINUI_EFFECT_SAVE_EDIT, -1, top(m)->system, -1);
        pop(m);
        break;
    case MAINUI_DIALOG_CLEAR_RECENT: effect(m, MAINUI_EFFECT_CLEAR_RECENT); break;
    case MAINUI_DIALOG_REMOVE_FAVORITE: list_effect(m, MAINUI_EFFECT_REMOVE_FAVORITE, w.list, w.system, w.index); break;
    case MAINUI_DIALOG_CLEAR_FAVORITE: effect(m, MAINUI_EFFECT_CLEAR_FAVORITES); break;
    case MAINUI_DIALOG_DELETE_ROM: list_effect(m, MAINUI_EFFECT_DELETE_ROM, w.list, w.system, w.index); break;
    case MAINUI_DIALOG_FACTORY_RESET: effect(m, MAINUI_EFFECT_FACTORY_RESET); break;
    case MAINUI_DIALOG_REFRESH_ROMS: {
        effect(m, MAINUI_EFFECT_REFRESH_ROMS);
        struct mainui_window *progress = push(m, MAINUI_WIN_PROGRESS);
        if (progress) progress->progress = MAINUI_PROGRESS_REFRESH;
        break;
    }
    case MAINUI_DIALOG_POWER_OFF:
    case MAINUI_DIALOG_FLASH_MODE: {
        // Both run under their progress window until the device goes down (static).
        int off = w.dialog == MAINUI_DIALOG_POWER_OFF;
        struct mainui_effect *e = effect(m, off ? MAINUI_EFFECT_POWER_OFF : MAINUI_EFFECT_FLASH_MODE);
        if (off) e->value = w.index == 1;
        struct mainui_window *progress = push(m, MAINUI_WIN_PROGRESS);
        if (progress) {
            progress->progress = off ? MAINUI_PROGRESS_POWER_OFF : MAINUI_PROGRESS_FLASH_MODE;
            if (off) progress->index = e->value;
        }
        break;
    }
    case MAINUI_DIALOG_UPGRADE: effect(m, MAINUI_EFFECT_UPGRADE); break;
    case MAINUI_DIALOG_REMOVE_DEVICE: list_effect(m, MAINUI_EFFECT_REMOVE_DEVICE, w.list, -1, w.index); break;
    case MAINUI_DIALOG_EXTENSION_CONFIRM: break;
    case MAINUI_DIALOG_RESET_MAPPING:
        // The rows read their defaults at once; the frontend writes the profile and reads it back.
        for (int setting = MAINUI_SET_MAP_LAYOUT; setting <= MAINUI_SET_MAP_VOLDOWN; ++setting) m->settings[setting] = 0;
        effect(m, MAINUI_EFFECT_RESET_MAPPING);
        break;
    case MAINUI_DIALOG_RESET_APPEARANCE: {
        // The rows read their defaults at once; the frontend applies them, with the Default font,
        // and writes appearance.json.
        int defaults[MAINUI_SET_COUNT];
        mainui_settings_defaults(defaults);
        for (int setting = MAINUI_SET_APPEAR_ACCENT; setting <= MAINUI_SET_APPEAR_LAST; ++setting)
            m->settings[setting] = defaults[setting];
        effect(m, MAINUI_EFFECT_RESET_APPEARANCE);
        break;
    }
    }
}

// ---- Settings screens ----------------------------------------------------------------------

#define RANGE(text, key, lo, hi) { .label = text, .kind = MAINUI_ITEM_RANGE, .setting = key, .min = lo, .max = hi }
#define CYCLE(text, key, list, wraps) { .label = text, .kind = MAINUI_ITEM_CYCLE, .setting = key, \
    .values = list, .value_count = (int)(sizeof(list) / sizeof(list[0])), .wrap = wraps }
#define SWITCH(text, key) { .label = text, .kind = MAINUI_ITEM_SWITCH, .setting = key }
#define SUBMENU(text, target) { .label = text, .kind = MAINUI_ITEM_SUBMENU, .screen = target }
#define ACTION(text, what) { .label = text, .kind = MAINUI_ITEM_ACTION, .action = what }
#define INFO(text) { .label = text, .kind = MAINUI_ITEM_INFO }
#define HEADER(text) { .label = text, .kind = MAINUI_ITEM_HEADER }

// Display: the stock rows, with LED Policy's Custom rows right after it while Custom is chosen.
static const struct mainui_item display_items[] = {
    RANGE("Brightness", MAINUI_SET_BRIGHTNESS, 0, 10),
    SWITCH("LED", MAINUI_SET_LEDSWITCH),
    RANGE("Top LED Scale", MAINUI_SET_TOPLED, 0, 10),
    RANGE("Taillight LED Scale", MAINUI_SET_SHOULDERLED, 0, 10),
    RANGE("F1/F2 LED Scale", MAINUI_SET_F1F2LED, 0, 10),
    CYCLE("LED Policy", MAINUI_SET_LED_OPTION, led_options, 1),
};
static const struct mainui_item led_custom_items[] = {
    CYCLE("Effect", MAINUI_SET_LED_EFFECT, led_effects, 1),
    CYCLE("Colour", MAINUI_SET_LED_COLOUR, led_colours, 1),
    RANGE("Time", MAINUI_SET_LED_TIME, 1, 10),
};
static const struct mainui_item display_tail_items[] = {
    CYCLE("Lock Screen Time", MAINUI_SET_DIMTIME, mainui_dim_times, 1),
    RANGE("Contrast", MAINUI_SET_COLORCONTRAST, 1, 10),
    RANGE("Saturation", MAINUI_SET_COLORSATURATION, 0, 10),
    RANGE("Color Temperature", MAINUI_SET_COLORTEMPERATURE, 0, 20),
    RANGE("Exposure", MAINUI_SET_COLORBRIGHTNESS, 1, 10),
};
static const struct mainui_item volume_items[] = {
    RANGE("Volume", MAINUI_SET_VOL, 0, 20),
    RANGE("BGM Volume", MAINUI_SET_BGMVOL, 0, 20),
    RANGE("Rumble", MAINUI_SET_RUMBLELEVEL, 0, 5),
    SWITCH("Enable Keytone", MAINUI_SET_ENABLEKEYTONE),
};
static const struct mainui_item keymap_items[] = {
    CYCLE("Keymap", MAINUI_SET_KEYMAP, keymaps, 0),
    CYCLE("Dpad to Analog Key(hold)", MAINUI_SET_DPAD2AXIS, dpad_keys, 1),
    CYCLE("Confirmation Button", MAINUI_SET_BCONFIRM, two_values, 1),
    CYCLE("OSD Shortcut", MAINUI_SET_OSDSHORTCUT, osd_shortcuts, 0),
    SUBMENU("Button Mapping", MAINUI_SCREEN_BUTTON_MAPPING),
};
static const struct mainui_item button_mapping_items[] = {
    INFO("Status"),
    CYCLE("Layout", MAINUI_SET_MAP_LAYOUT, map_layouts, 1),
    CYCLE("Select", MAINUI_SET_MAP_SELECT, map_select, 1),
    CYCLE("Start", MAINUI_SET_MAP_START, map_start, 1),
    CYCLE("F1", MAINUI_SET_MAP_F1, map_f1, 1),
    CYCLE("F2", MAINUI_SET_MAP_F2, map_f2, 1),
    CYCLE("Vol+", MAINUI_SET_MAP_VOLUP, map_volup, 1),
    CYCLE("Vol-", MAINUI_SET_MAP_VOLDOWN, map_voldown, 1),
    ACTION("Reset", MAINUI_ACTION_RESET_MAPPING),
};
// BrickOS Themes > Appearance (docs/material-you-frontend.md).
static const struct mainui_item appearance_items[] = {
    CYCLE("Accent colour", MAINUI_SET_APPEAR_ACCENT, appear_accents, 1),
    CYCLE("Background", MAINUI_SET_APPEAR_BACKGROUND, appear_backgrounds, 1),
    CYCLE("Text size", MAINUI_SET_APPEAR_TEXT_SIZE, appear_text_sizes, 1),
    SUBMENU("Font", MAINUI_SCREEN_APPEARANCE_FONT),
    CYCLE("Card shape", MAINUI_SET_APPEAR_SHAPE, appear_shapes, 1),
    CYCLE("Wallpaper dimming", MAINUI_SET_APPEAR_WALLPAPER, appear_dimmings, 1),
    CYCLE("Focus halo", MAINUI_SET_APPEAR_FOCUS, appear_halos, 1),
    CYCLE("Animations", MAINUI_SET_APPEAR_ANIMATIONS, appear_motions, 1),
    CYCLE("Game lists", MAINUI_SET_APPEAR_GAME_VIEW, appear_views, 1),
    CYCLE("Recent", MAINUI_SET_APPEAR_RECENT_VIEW, appear_views, 1),
    CYCLE("Favorites", MAINUI_SET_APPEAR_FAVORITES_VIEW, appear_views, 1),
    ACTION("Reset appearance", MAINUI_ACTION_RESET_APPEARANCE),
};
static const struct mainui_item system_items[] = {
    ACTION("Power off", MAINUI_ACTION_POWER_OFF),
    SUBMENU("Language", MAINUI_SCREEN_LANGUAGE),
    SWITCH("Netplay", MAINUI_SET_NETPLAY),
    SUBMENU("USB Mode", MAINUI_SCREEN_USB_MODE),
    ACTION("Refresh Roms", MAINUI_ACTION_REFRESH_ROMS),
    SUBMENU("Date And Time", MAINUI_SCREEN_DATETIME),
    ACTION("Factory Reset", MAINUI_ACTION_FACTORY_RESET),
    SUBMENU("Developer", MAINUI_SCREEN_DEVELOPER),
    SUBMENU("Hardware Test", MAINUI_SCREEN_IO_TEST),
    ACTION("Flash Mode", MAINUI_ACTION_FLASH_MODE),
    SUBMENU("Device Info", MAINUI_SCREEN_DEVICE_INFO),
};
static const struct mainui_item brickos_startup_items[] = {
    { .label = "Desktop", .kind = MAINUI_ITEM_INITIAL_VIEW_CHOICE, .action = MAINUI_BRICKOS_VIEW_DESKTOP },
    { .label = "Games", .kind = MAINUI_ITEM_INITIAL_VIEW_CHOICE, .action = MAINUI_BRICKOS_VIEW_GAMES },
    { .label = "Favorites", .kind = MAINUI_ITEM_INITIAL_VIEW_CHOICE, .action = MAINUI_BRICKOS_VIEW_FAVORITES },
    { .label = "Recent", .kind = MAINUI_ITEM_INITIAL_VIEW_CHOICE, .action = MAINUI_BRICKOS_VIEW_RECENT },
    { .label = "GameSwitcher", .kind = MAINUI_ITEM_INITIAL_VIEW_CHOICE, .action = MAINUI_BRICKOS_VIEW_GAMESWITCHER },
    { .label = "Apps", .kind = MAINUI_ITEM_INITIAL_VIEW_CHOICE, .action = MAINUI_BRICKOS_VIEW_APPS },
    { .label = "Last used", .kind = MAINUI_ITEM_INITIAL_VIEW_CHOICE, .action = MAINUI_BRICKOS_VIEW_LAST_USED },
};
static const struct mainui_item tab_visibility_items[] = {
    SWITCH("Favorites", MAINUI_SET_FAVORITETAB),
    SWITCH("Recent", MAINUI_SET_RECENTPLAYEDTAB),
    SWITCH("Bests", MAINUI_SET_BESTSTAB),
    SWITCH("Games", MAINUI_SET_RETROGAMETAB),
    SWITCH("Ports", MAINUI_SET_PORTSTAB),
    SWITCH("Apps", MAINUI_SET_APPSTAB),
};
static const struct mainui_item developer_items[] = { SWITCH("Enable SSH", MAINUI_SET_ENABLESSH) };
static const struct mainui_item usb_items[] = { CYCLE("USB Bottom", MAINUI_SET_USBMODE, usb_modes, 0) };

static int add_items(struct mainui_item *items, int n, int max, const struct mainui_item *from, int count_from) {
    for (int i = 0; i < count_from && n < max; ++i) items[n++] = from[i];
    return n;
}

static int add_list(struct mainui_item *items, int n, int max, enum mainui_list list, int entries) {
    for (int i = 0; i < entries && n < max; ++i)
        items[n++] = (struct mainui_item){ .label = "", .kind = MAINUI_ITEM_LIST, .action = (int)list };
    return n;
}

#define ADD(table) add_items(items, n, max, table, (int)(sizeof(table) / sizeof(table[0])))

// ---- Settings extensions --------------------------------------------------------------------

static int extension_info(const struct mainui_model *m, int node, struct mainui_extension_info *info) {
    memset(info, 0, sizeof(*info));
    const struct mainui_extensions *x = &m->extensions;
    return x->info && node >= 0 ? x->info(x->context, node, info) : -1;
}

// A callback's count of rows; it fills at most max, and rows may be NULL when max is 0.
static int extension_children(const struct mainui_model *m, int node, int *nodes, int max) {
    const struct mainui_extensions *x = &m->extensions;
    int n = x->children && node >= 0 ? x->children(x->context, node, nodes, max) : 0;
    return n > 0 ? n : 0;
}

static int extension_section_rows(const struct mainui_model *m, enum mainui_screen screen,
                                  struct mainui_extension_row *rows, int max) {
    const struct mainui_extensions *x = &m->extensions;
    if (!x->section_rows || !mainui_model_extension_section_id(screen)) return 0;
    int n = x->section_rows(x->context, (int)screen, rows, max);
    return n > 0 ? n : 0;
}

static struct mainui_item extension_item(const struct mainui_model *m, int node) {
    struct mainui_extension_info info;
    extension_info(m, node, &info);
    return (struct mainui_item){ .label = info.label ? info.label : "", .kind = MAINUI_ITEM_EXTENSION, .action = node };
}

// The menu node of the extension screen in front: the deepest one on the stack.
static int extension_screen_node(const struct mainui_model *m) {
    for (int i = m->depth - 1; i >= 0; --i)
        if (m->stack[i].kind == MAINUI_WIN_SETTINGS && m->stack[i].screen == MAINUI_SCREEN_EXTENSION) return m->stack[i].index;
    return -1;
}

static int extension_menu_items(const struct mainui_model *m, struct mainui_item *items, int max) {
    int node = extension_screen_node(m), count = extension_children(m, node, NULL, 0);
    if (count <= 0 || max <= 0) return 0;
    int *nodes = malloc((size_t)count * sizeof(*nodes));
    if (!nodes) return 0;
    count = extension_children(m, node, nodes, count);
    int n = 0;
    for (int i = 0; i < count && n < max; ++i) items[n++] = extension_item(m, nodes[i]);
    free(nodes);
    return n;
}

// Places the rows extensions add to a built-in section among its rows as they are now (with
// conditional rows shown or not): before or after their anchor row, else at the end. In Wi-Fi
// and Bluetooth they always follow the switch, ahead of the network and device lists. Rows
// for the same place keep extension order.
static int insert_extension_rows(const struct mainui_model *m, enum mainui_screen screen,
                                 struct mainui_item *items, int n, int max) {
    static struct mainui_extension_row rows[MAINUI_EXTENSION_SECTION_ROWS];
    int count = extension_section_rows(m, screen, rows, MAINUI_EXTENSION_SECTION_ROWS);
    if (count > MAINUI_EXTENSION_SECTION_ROWS) count = MAINUI_EXTENSION_SECTION_ROWS;
    if (count <= 0) return n;
    struct mainui_item *built = malloc((size_t)(n ? n : 1) * sizeof(*built));
    int *place = malloc((size_t)count * sizeof(*place));
    if (!built || !place) { free(built); free(place); return n; }
    memcpy(built, items, (size_t)n * sizeof(*built));
    for (int i = 0; i < count; ++i) {
        int at = n;
        if (screen == MAINUI_SCREEN_WIFI || screen == MAINUI_SCREEN_BLUETOOTH) {
            at = n > 0 ? 1 : 0;
        } else if (rows[i].anchor != MAINUI_EXTENSION_END && rows[i].anchor_id) {
            for (int k = 0; k < n; ++k) {
                char id[96];
                mainui_model_row_id(built[k].label, id, (int)sizeof(id));
                if (id[0] && strcmp(id, rows[i].anchor_id) == 0) {
                    at = rows[i].anchor == MAINUI_EXTENSION_AFTER ? k + 1 : k;
                    // LED Policy's Custom rows stay right after it.
                    if (rows[i].anchor == MAINUI_EXTENSION_AFTER && built[k].kind == MAINUI_ITEM_CYCLE &&
                        built[k].setting == MAINUI_SET_LED_OPTION)
                        while (at < n && mainui_model_led_setting(built[at].setting) &&
                               built[at].setting != MAINUI_SET_LED_OPTION) ++at;
                    break;
                }
            }
        }
        place[i] = at;
    }
    int out = 0;
    for (int at = 0; at <= n; ++at) {
        for (int i = 0; i < count && out < max; ++i)
            if (place[i] == at) items[out++] = extension_item(m, rows[i].node);
        if (at < n && out < max) items[out++] = built[at];
    }
    free(built);
    free(place);
    return out;
}

// The folder level of the Time Zone screen in front: the deepest one on the stack.
static int zone_level(const struct mainui_model *m) {
    for (int i = m->depth - 1; i >= 0; --i)
        if (m->stack[i].kind == MAINUI_WIN_SETTINGS && m->stack[i].screen == MAINUI_SCREEN_TIMEZONE) return m->stack[i].system;
    return 0;
}

int mainui_model_screen_items(const struct mainui_model *m, enum mainui_screen screen,
                              struct mainui_item *items, int max) {
    const int *s = m->settings;
    int n = 0;
    switch (screen) {
    case MAINUI_SCREEN_DISPLAY:
        n = ADD(display_items);
        if (s[MAINUI_SET_LED_OPTION] == MAINUI_LED_CUSTOM) n = ADD(led_custom_items);
        n = ADD(display_tail_items);
        break;
    case MAINUI_SCREEN_VOLUME: n = ADD(volume_items); break;
    case MAINUI_SCREEN_KEYMAP: n = ADD(keymap_items); break;
    case MAINUI_SCREEN_BUTTON_MAPPING: n = ADD(button_mapping_items); break;
    case MAINUI_SCREEN_SYSTEM: n = ADD(system_items); break;
    case MAINUI_SCREEN_TAB_VISIBILITY: n = ADD(tab_visibility_items); break;
    case MAINUI_SCREEN_DEVELOPER: n = ADD(developer_items); break;
    case MAINUI_SCREEN_USB_MODE: n = ADD(usb_items); break;
    case MAINUI_SCREEN_WIFI: {
        static const struct mainui_item wifi[] = { SWITCH("Wi-Fi", MAINUI_SET_WIFI) };
        n = ADD(wifi);
        if (s[MAINUI_SET_WIFI]) n = add_list(items, n, max, MAINUI_LIST_WIFI, count(m, MAINUI_LIST_WIFI, 0));
        break;
    }
    case MAINUI_SCREEN_BLUETOOTH: {
        // With Bluetooth on, the paired devices and the ones the scan finds, each under its
        // section title; the titles show with no device under them (w1).
        static const struct mainui_item bluetooth[] = { SWITCH("Bluetooth", MAINUI_SET_BLUETOOTH) };
        static const struct mainui_item mine[] = { HEADER("My Devices") }, other[] = { HEADER("Other Devices") };
        n = ADD(bluetooth);
        if (s[MAINUI_SET_BLUETOOTH]) {
            n = ADD(mine);
            n = add_list(items, n, max, MAINUI_LIST_PAIRED, count(m, MAINUI_LIST_PAIRED, 0));
            n = ADD(other);
            n = add_list(items, n, max, MAINUI_LIST_BLUETOOTH, count(m, MAINUI_LIST_BLUETOOTH, 0));
        }
        break;
    }
    case MAINUI_SCREEN_HOTSPOT: {
        static const struct mainui_item hotspot[] = {
            SWITCH("Hotspot", MAINUI_SET_HOSTAP), ACTION("SSID", MAINUI_ACTION_SSID),
            ACTION("Password", MAINUI_ACTION_PASSWORD),
        };
        n = ADD(hotspot);
        break;
    }
    case MAINUI_SCREEN_HOMEPAGE: {
        static const struct mainui_item home[] = { SWITCH("Navigation page", MAINUI_SET_ENABLEHOME) };
        static const struct mainui_item focus[] = { SWITCH("Allow focus to the tab bar", MAINUI_SET_FOCUSONTAB) };
        static const struct mainui_item visibility[] = { SUBMENU("Tab Visibility", MAINUI_SCREEN_TAB_VISIBILITY) };
        n = ADD(home);
        // The focus switch only shows while the home page is enabled.
        if (s[MAINUI_SET_ENABLEHOME]) n = ADD(focus);
        n = ADD(visibility);
        // Where BrickOS starts sits with the tabs it can start on.
        if (m->presentation == MAINUI_PRESENTATION_MATERIAL_YOU) {
            static const struct mainui_item startup[] = { SUBMENU("Default destination", MAINUI_SCREEN_BRICKOS_STARTUP) };
            n = ADD(startup);
        }
        break;
    }
    case MAINUI_SCREEN_DATETIME: {
        static const struct mainui_item zone[] = { SUBMENU("Time Zone", MAINUI_SCREEN_TIMEZONE) };
        static const struct mainui_item switches[] = {
            SWITCH("24-hour clock", MAINUI_SET_24HOURCLOCK),
            SWITCH("Network Synchronization Time", MAINUI_SET_USENETWORKTIME),
        };
        static const struct mainui_item set_date[] = { ACTION("Set Date", MAINUI_ACTION_SET_DATE) };
        n = ADD(zone);
        n = ADD(switches);
        // Without network time a Set Date row is added (g07c).
        if (!s[MAINUI_SET_USENETWORKTIME]) n = ADD(set_date);
        break;
    }
    case MAINUI_SCREEN_SET_DATE: {
        static const struct mainui_item date[] = {
            { .label = "Year", .kind = MAINUI_ITEM_DATE, .action = 0, .min = 1900, .max = 3000 },
            { .label = "Month", .kind = MAINUI_ITEM_DATE, .action = 1, .min = 1, .max = 12 },
            { .label = "Day", .kind = MAINUI_ITEM_DATE, .action = 2, .min = 1, .max = 31 },
            { .label = "Hour", .kind = MAINUI_ITEM_DATE, .action = 3, .max = 23 },
            { .label = "Minute", .kind = MAINUI_ITEM_DATE, .action = 4, .max = 59 },
            { .label = "Second", .kind = MAINUI_ITEM_DATE, .action = 5, .max = 59 },
        };
        n = ADD(date);
        break;
    }
    case MAINUI_SCREEN_THEMES: {
        // The appearance choices. Frontend leads, as it decides the row after it: BrickOS uses
        // nothing from the stock theme, so only Stock++ has a Theme row, and BrickOS has its own
        // Appearance screen instead.
        static const struct mainui_item frontend[] = { SUBMENU("Frontend", MAINUI_SCREEN_FRONTEND) };
        static const struct mainui_item theme[] = { SUBMENU("Theme", MAINUI_SCREEN_THEME_LIST) };
        static const struct mainui_item appearance[] = { SUBMENU("Appearance", MAINUI_SCREEN_APPEARANCE) };
        static const struct mainui_item visuals[] = {
            SUBMENU("Bootlogo", MAINUI_SCREEN_BOOTLOGO),
            SUBMENU("Splashscreen", MAINUI_SCREEN_SPLASHSCREEN),
            SUBMENU("Loading Screen", MAINUI_SCREEN_LOADING_SCREEN),
        };
        n = ADD(frontend);
        n = m->presentation == MAINUI_PRESENTATION_MATERIAL_YOU ? ADD(appearance) : ADD(theme);
        n = ADD(visuals);
        break;
    }
    case MAINUI_SCREEN_THEME_LIST: n = add_list(items, 0, max, MAINUI_LIST_THEMES, count(m, MAINUI_LIST_THEMES, 0)); break;
    case MAINUI_SCREEN_APPEARANCE:
        if (m->presentation == MAINUI_PRESENTATION_MATERIAL_YOU) n = ADD(appearance_items);
        break;
    case MAINUI_SCREEN_APPEARANCE_FONT:
        // Default, the system font, always leads the fonts folder.
        if (m->presentation == MAINUI_PRESENTATION_MATERIAL_YOU)
            n = add_list(items, 0, max, MAINUI_LIST_FONTS, count(m, MAINUI_LIST_FONTS, 0) + 1);
        break;
    case MAINUI_SCREEN_LANGUAGE: n = add_list(items, 0, max, MAINUI_LIST_LANGUAGES, count(m, MAINUI_LIST_LANGUAGES, 0)); break;
    case MAINUI_SCREEN_TIMEZONE: {
        int level = zone_level(m);
        n = add_list(items, 0, max, MAINUI_LIST_TIMEZONES, count(m, MAINUI_LIST_TIMEZONES, level));
        break;
    }
    case MAINUI_SCREEN_DEVICE_INFO: {
        static const struct mainui_item info[] = {
            INFO("Model Name"), INFO("Model Number"), INFO("Version"), INFO("UI Version"), INFO("Linux Version"),
        };
        n = ADD(info);
        break;
    }
    case MAINUI_SCREEN_BOOTLOGO: {
        // Random leads the images.
        int entries = count(m, MAINUI_LIST_BOOTLOGOS, 0);
        if (entries) n = add_list(items, 0, max, MAINUI_LIST_BOOTLOGOS, entries + 1);
        else {
            static const struct mainui_item empty[] = { INFO("No valid images installed") };
            n = ADD(empty);
        }
        break;
    }
    case MAINUI_SCREEN_SPLASHSCREEN: {
        // Random leads the images.
        int entries = count(m, MAINUI_LIST_SPLASHSCREENS, 0);
        if (entries) n = add_list(items, 0, max, MAINUI_LIST_SPLASHSCREENS, entries + 1);
        else {
            static const struct mainui_item empty[] = { INFO("No valid images installed") };
            n = ADD(empty);
        }
        break;
    }
    case MAINUI_SCREEN_LOADING_SCREEN:
        // TRIMUI, RetroArch's own logo, always leads the presets.
        n = add_list(items, 0, max, MAINUI_LIST_LOADING_SCREENS, count(m, MAINUI_LIST_LOADING_SCREENS, 0) + 1);
        break;
    case MAINUI_SCREEN_BRICKOS_STARTUP:
        if (m->presentation == MAINUI_PRESENTATION_MATERIAL_YOU) n = ADD(brickos_startup_items);
        break;
    case MAINUI_SCREEN_FRONTEND: {
        static const struct mainui_item choices[] = {
            { .label = "Stock++", .kind = MAINUI_ITEM_PRESENTATION_CHOICE, .action = MAINUI_PRESENTATION_STOCK },
            { .label = "BrickOS", .kind = MAINUI_ITEM_PRESENTATION_CHOICE, .action = MAINUI_PRESENTATION_MATERIAL_YOU },
        };
        n = ADD(choices);
        break;
    }
    case MAINUI_SCREEN_EXTENSION: n = extension_menu_items(m, items, max); break;
    case MAINUI_SCREEN_IO_TEST:
    case MAINUI_SCREEN_COUNT: break;
    }
    if (mainui_model_extension_section_id(screen)) n = insert_extension_rows(m, screen, items, n, max);
    return n;
}

#define SCREEN_ITEMS_MAX 128

static const struct { const char *id; enum mainui_screen screen; } extension_sections[] = {
    {"display", MAINUI_SCREEN_DISPLAY}, {"volume", MAINUI_SCREEN_VOLUME}, {"wifi", MAINUI_SCREEN_WIFI},
    {"hotspot", MAINUI_SCREEN_HOTSPOT}, {"bluetooth", MAINUI_SCREEN_BLUETOOTH}, {"themes", MAINUI_SCREEN_THEMES},
    {"keymap", MAINUI_SCREEN_KEYMAP}, {"homepage", MAINUI_SCREEN_HOMEPAGE}, {"system", MAINUI_SCREEN_SYSTEM},
    {"date-time", MAINUI_SCREEN_DATETIME}, {"developer", MAINUI_SCREEN_DEVELOPER}, {"device-info", MAINUI_SCREEN_DEVICE_INFO},
};

int mainui_model_extension_section(const char *id) {
    for (size_t i = 0; i < sizeof(extension_sections) / sizeof(extension_sections[0]); ++i)
        if (strcmp(extension_sections[i].id, id) == 0) return (int)extension_sections[i].screen;
    return -1;
}

const char *mainui_model_extension_section_id(enum mainui_screen screen) {
    for (size_t i = 0; i < sizeof(extension_sections) / sizeof(extension_sections[0]); ++i)
        if (extension_sections[i].screen == screen) return extension_sections[i].id;
    return NULL;
}

void mainui_model_row_id(const char *label, char *out, int size) {
    int n = 0, gap = 0;
    if (size <= 0) return;
    for (const char *p = label; *p; ++p) {
        char c = *p;
        int alnum = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
        if (!alnum) { gap = n > 0; continue; }
        if (gap && n + 1 < size) out[n++] = '-';
        gap = 0;
        if (n + 1 < size) out[n++] = (char)(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
    }
    out[n] = 0;
}

int mainui_model_screen_item_capacity(const struct mainui_model *m, enum mainui_screen screen) {
    if (screen == MAINUI_SCREEN_BOOTLOGO || screen == MAINUI_SCREEN_SPLASHSCREEN) {
        enum mainui_list list = screen == MAINUI_SCREEN_BOOTLOGO ? MAINUI_LIST_BOOTLOGOS : MAINUI_LIST_SPLASHSCREENS;
        int entries = count(m, list, 0);
        return entries > 0 ? entries + 1 : 1;
    }
    if (screen == MAINUI_SCREEN_LOADING_SCREEN) return count(m, MAINUI_LIST_LOADING_SCREENS, 0) + 1;
    if (screen == MAINUI_SCREEN_APPEARANCE_FONT) return count(m, MAINUI_LIST_FONTS, 0) + 1;
    if (screen == MAINUI_SCREEN_EXTENSION) {
        int rows = extension_children(m, extension_screen_node(m), NULL, 0);
        return rows > 0 ? rows : 1;
    }
    int added = extension_section_rows(m, screen, NULL, 0);
    return SCREEN_ITEMS_MAX + (added < MAINUI_EXTENSION_SECTION_ROWS ? added : MAINUI_EXTENSION_SECTION_ROWS);
}

// Screens whose values system.json holds; leaving one rewrites the file, as leaving the
// official Display screen did even with no change.
static int screen_persists(enum mainui_screen screen) {
    switch (screen) {
    case MAINUI_SCREEN_DISPLAY: case MAINUI_SCREEN_VOLUME: case MAINUI_SCREEN_KEYMAP:
    case MAINUI_SCREEN_HOMEPAGE: case MAINUI_SCREEN_TAB_VISIBILITY: case MAINUI_SCREEN_SYSTEM:
    case MAINUI_SCREEN_DATETIME: case MAINUI_SCREEN_DEVELOPER: case MAINUI_SCREEN_HOTSPOT:
        return 1;
    default:
        return 0;
    }
}

static struct mainui_window *open_screen(struct mainui_model *m, enum mainui_screen screen) {
    struct mainui_window *w = push(m, MAINUI_WIN_SETTINGS);
    if (!w) return NULL;
    w->screen = screen;
    // Themes shows the boot visuals and the loading screen: their folders are read again.
    if (screen == MAINUI_SCREEN_THEMES) {
        struct mainui_effect *boot = effect(m, MAINUI_EFFECT_REFRESH_BOOT_VISUALS);
        boot->list = MAINUI_LIST_BOOTLOGOS;
        struct mainui_effect *splash = effect(m, MAINUI_EFFECT_REFRESH_BOOT_VISUALS);
        splash->list = MAINUI_LIST_SPLASHSCREENS;
        effect(m, MAINUI_EFFECT_REFRESH_LOADING_SCREENS);
    }
    if (screen == MAINUI_SCREEN_LOADING_SCREEN) effect(m, MAINUI_EFFECT_REFRESH_LOADING_SCREENS);
    // Appearance shows the chosen font's name, and its list the folder: both read it again.
    if (screen == MAINUI_SCREEN_APPEARANCE || screen == MAINUI_SCREEN_APPEARANCE_FONT) effect(m, MAINUI_EFFECT_REFRESH_FONTS);
    if (screen == MAINUI_SCREEN_BOOTLOGO || screen == MAINUI_SCREEN_SPLASHSCREEN) {
        struct mainui_effect *e = effect(m, MAINUI_EFFECT_REFRESH_BOOT_VISUALS);
        e->list = screen == MAINUI_SCREEN_BOOTLOGO ? MAINUI_LIST_BOOTLOGOS : MAINUI_LIST_SPLASHSCREENS;
    }
    if (screen == MAINUI_SCREEN_TIMEZONE) w->system = 0;
    if (screen == MAINUI_SCREEN_FRONTEND) w->focus = m->presentation == MAINUI_PRESENTATION_MATERIAL_YOU ? 1 : 0;
    // LED Policy shows the LED daemon's mode and whether the daemon runs.
    if (screen == MAINUI_SCREEN_DISPLAY) effect(m, MAINUI_EFFECT_REFRESH_LEDS);
    // Keymap shows whether the hold key is remapped, and Button Mapping the whole profile.
    if (screen == MAINUI_SCREEN_KEYMAP || screen == MAINUI_SCREEN_BUTTON_MAPPING) effect(m, MAINUI_EFFECT_REFRESH_MAPPING);
    // Button Mapping opens on Layout, below the read-only Status.
    if (screen == MAINUI_SCREEN_BUTTON_MAPPING) w->focus = 1;
    if (screen == MAINUI_SCREEN_IO_TEST) effect(m, MAINUI_EFFECT_IO_TEST)->value = 1;
    if (screen == MAINUI_SCREEN_SET_DATE) {
        w->date[0] = 1900; w->date[1] = w->date[2] = 1;
        w->date_dst = -1;
        w->dirty = 1;
        effect(m, MAINUI_EFFECT_SET_DATE);
    }
    // The loads of the rows extensions added here.
    if (extension_section_rows(m, screen, NULL, 0) > 0) {
        struct mainui_effect *e = effect(m, MAINUI_EFFECT_EXTENSION_OPEN);
        e->value = screen;
        e->index = -1;
    }
    return w;
}

static void open_extension(struct mainui_model *m, int node) {
    struct mainui_extension_info info;
    if (node < 0 || extension_info(m, node, &info) < 0 || info.kind != MAINUI_EXTENSION_MENU) return;
    struct mainui_window *w = open_screen(m, MAINUI_SCREEN_EXTENSION);
    if (!w) return;
    w->index = node;
    struct mainui_effect *e = effect(m, MAINUI_EFFECT_EXTENSION_OPEN);
    e->value = MAINUI_SCREEN_EXTENSION;
    e->index = node;
}

// LEFT or RIGHT on a select (wrapping) or a range (within its bounds).
static void extension_step(struct mainui_model *m, int node, int step) {
    struct mainui_extension_info info;
    if (extension_info(m, node, &info) < 0 || !info.usable) return;
    int value = info.value;
    if (info.kind == MAINUI_EXTENSION_SELECT && info.item_count > 0) {
        value = ((info.value + step) % info.item_count + info.item_count) % info.item_count;
    } else if (info.kind == MAINUI_EXTENSION_RANGE) {
        value = info.value + step * (info.step > 0 ? info.step : 1);
        if (value < info.min) value = info.min;
        if (value > info.max) value = info.max;
    }
    if (value == info.value) return;
    struct mainui_effect *e = effect(m, MAINUI_EFFECT_EXTENSION_CHANGE);
    e->index = node;
    e->value = value;
    e->launcher = 0;
}

static void extension_confirm(struct mainui_model *m, int node) {
    struct mainui_extension_info info;
    if (extension_info(m, node, &info) < 0 || !info.usable) return;
    switch (info.kind) {
    case MAINUI_EXTENSION_MENU:
        open_extension(m, node);
        break;
    case MAINUI_EXTENSION_CMD: {
        effect(m, MAINUI_EFFECT_EXTENSION_RUN)->index = node;
        // The full output shows live; otherwise a progress view waits, and B cancels.
        struct mainui_window *w = push(m, info.output == MAINUI_EXTENSION_OUTPUT_FULL ? MAINUI_WIN_TEXT : MAINUI_WIN_PROGRESS);
        if (!w) break;
        w->index = node;
        if (w->kind == MAINUI_WIN_PROGRESS) w->progress = MAINUI_PROGRESS_EXTENSION;
        break;
    }
    case MAINUI_EXTENSION_TOGGLE: {
        struct mainui_effect *e = effect(m, MAINUI_EFFECT_EXTENSION_CHANGE);
        e->index = node;
        e->value = !info.value;
        e->launcher = 1;
        break;
    }
    default:
        break;
    }
}

void mainui_model_extension_confirm(struct mainui_model *m, int node) {
    open_dialog(m, MAINUI_DIALOG_EXTENSION_CONFIRM, -1, -1, node);
}

static void clamp_text(struct mainui_window *w) {
    int last = w->item_count - MAINUI_TEXT_ROWS;
    if (w->first > last) w->first = last;
    if (w->first < 0) w->first = 0;
}

void mainui_model_extension_output(struct mainui_model *m, int node, int lines) {
    struct mainui_window *w = top(m);
    if (w->kind != MAINUI_WIN_TEXT || w->index != node) {
        w = push(m, MAINUI_WIN_TEXT);
        if (!w) return;
        w->index = node;
    }
    w->item_count = lines;
    clamp_text(w);
}

void mainui_model_text_lines(struct mainui_model *m, int lines) {
    struct mainui_window *w = top(m);
    if (w->kind != MAINUI_WIN_TEXT) return;
    w->item_count = lines;
    clamp_text(w);
}

void mainui_model_extension_close_view(struct mainui_model *m, int node) {
    const struct mainui_window *w = top(m);
    if (m->depth > 1 && w->kind == MAINUI_WIN_TEXT && w->index == node) pop(m);
}

void mainui_model_extension_close(struct mainui_model *m, int node) {
    while (m->depth > 1) {
        const struct mainui_window *w = top(m);
        if (w->index != node) break;
        if (!(w->kind == MAINUI_WIN_DIALOG && w->dialog == MAINUI_DIALOG_EXTENSION_CONFIRM) &&
            !(w->kind == MAINUI_WIN_PROGRESS && w->progress == MAINUI_PROGRESS_EXTENSION))
            break;
        pop(m);
    }
}

static void finish_date(struct mainui_model *m, struct mainui_window *w) {
    if (!w->dirty) return;
    struct mainui_effect *e = effect(m, MAINUI_EFFECT_APPLY_DATE);
    snprintf(e->text, sizeof(e->text), "%04d-%02d-%02d %02d:%02d:%02d",
             w->date[0], w->date[1], w->date[2], w->date[3], w->date[4], w->date[5]);
    e->value = w->date_dst;
    w->dirty = 0;
}

void mainui_model_finish(struct mainui_model *m) {
    for (int i = m->depth - 1; i >= 0; --i) {
        struct mainui_window *w = &m->stack[i];
        if (w->kind == MAINUI_WIN_SETTINGS && w->screen == MAINUI_SCREEN_SET_DATE) finish_date(m, w);
        if (w->kind == MAINUI_WIN_SETTINGS && w->screen == MAINUI_SCREEN_IO_TEST) {
            effect(m, MAINUI_EFFECT_IO_TEST)->value = 0;
            w->kind = MAINUI_WIN_TABS;
        }
    }
}

static int date_days(const struct mainui_window *w) {
    static const int days[] = {31,28,31,30,31,30,31,31,30,31,30,31};
    int y = w->date[0], month = w->date[1];
    return days[month - 1] + (month == 2 && y % 4 == 0 && (y % 100 != 0 || y % 400 == 0));
}

// The entry an item shows within its list: the list's rows follow the rows before them.
static int list_entry(const struct mainui_item *items, int index) {
    int first = index;
    while (first > 0 && items[first - 1].kind == MAINUI_ITEM_LIST && items[first - 1].action == items[index].action) --first;
    return index - first;
}

// The next row the focus can take from focus, stepping by step (+1 or -1) and wrapping.
static int focus_step(const struct mainui_item *items, int n, int focus, int step) {
    for (int i = 0; i < n; ++i) {
        focus = (focus + step + n) % n;
        if (items[focus].kind != MAINUI_ITEM_HEADER) return focus;
    }
    return focus;
}

// The stock Keymap rotation and Button Mapping's Xbox layout exclude each other: choosing one
// sets the other back, through its own effect.
static void exclude_rotation(struct mainui_model *m, enum mainui_setting changed) {
    int *s = m->settings;
    if (changed == MAINUI_SET_MAP_LAYOUT && s[MAINUI_SET_MAP_LAYOUT] == MAINUI_MAP_XBOX && s[MAINUI_SET_KEYMAP] == 1)
        set_setting(m, MAINUI_SET_KEYMAP, 0);
    if (changed == MAINUI_SET_KEYMAP && s[MAINUI_SET_KEYMAP] == 1 && s[MAINUI_SET_MAP_LAYOUT] == MAINUI_MAP_XBOX)
        set_setting(m, MAINUI_SET_MAP_LAYOUT, MAINUI_MAP_NINTENDO);
}

static void cycle_item(struct mainui_model *m, const struct mainui_item *item, int step) {
    int current = -1;
    for (int i = 0; i < item->value_count; ++i)
        if (item->values[i] == m->settings[item->setting]) current = i;
    // A mapping the screen does not offer (Custom) moves to Default, or Nintendo, either way.
    if (current < 0 && mainui_model_mapping_setting(item->setting)) {
        set_setting(m, item->setting, item->values[0]);
        return;
    }
    if (current < 0) current = 0;
    int next = current + step;
    if (next < 0 || next >= item->value_count) {
        if (!item->wrap) {
            // These stock callbacks re-publish unchanged keymap state and the
            // OSD's hint even when the requested step reaches a limit.
            if (item->setting == MAINUI_SET_KEYMAP || item->setting == MAINUI_SET_OSDSHORTCUT)
                set_setting(m, item->setting, item->values[current]);
            exclude_rotation(m, item->setting);
            return;
        }
        next = (next + item->value_count) % item->value_count;
    }
    set_setting(m, item->setting, item->values[next]);
    exclude_rotation(m, item->setting);
    // The frontend writes ledpolicy with the option; the model keeps the same value.
    if (item->setting == MAINUI_SET_LED_OPTION)
        m->settings[MAINUI_SET_LEDPOLICY] = m->settings[MAINUI_SET_LED_OPTION] == MAINUI_LED_FOLLOW_THEME;
}

static void refresh_theme_tabs(struct mainui_model *m) {
    enum mainui_tab tabs[MAINUI_TAB_COUNT];
    struct mainui_tab_state current[MAINUI_TAB_COUNT];
    int n = mainui_model_visible_tabs(m, tabs);
    for (int i = 0; i < n; ++i) {
        enum mainui_tab tab = tabs[i];
        current[i] = m->tabs[tab];
        // initTab skips the Recent menu's positions (stock type 18).
        if (tab == MAINUI_TAB_RECENT) continue;
        m->tabs[tab] = (struct mainui_tab_state){0};
        if (m->refresh_tab_count == n) {
            int items = tab_count(m, tab);
            struct mainui_tab_state saved = m->refresh_tabs[i];
            if (saved.focus >= 0 && saved.focus < items) {
                m->tabs[tab] = saved;
                if (m->tabs[tab].first < 0) m->tabs[tab].first = 0;
                if (list_tab(tab))
                    keep_visible(&m->tabs[tab].first, saved.focus, MAINUI_TAB_LIST_ROWS, items);
            }
        }
    }
    // refresh calls initTab before restoreState. restoreState appends, rather than
    // replacing, its positions. Retain only counts/data that could still match.
    for (int i = 0; i < n && m->refresh_tab_count + i < MAINUI_TAB_COUNT; ++i)
        m->refresh_tabs[m->refresh_tab_count + i] = current[i];
    if (m->refresh_tab_count <= MAINUI_TAB_COUNT - n) m->refresh_tab_count += n;
    else m->refresh_tab_count = MAINUI_TAB_COUNT + 1;
}

static void screen_key(struct mainui_model *m, enum mainui_key key, int confirm, int back) {
    struct mainui_window *w = top(m);
    if (w->screen == MAINUI_SCREEN_IO_TEST) {
        // The IO test reads the controls itself; only MENU+START leaves it.
        if (key == MAINUI_KEY_MENU_START) {
            effect(m, MAINUI_EFFECT_IO_TEST)->value = 0;
            pop(m);
        }
        return;
    }
    if (back) {
        enum mainui_screen screen = w->screen;
        int zone_parent = screen == MAINUI_SCREEN_TIMEZONE && w->system > 0;
        int extended = screen == MAINUI_SCREEN_EXTENSION || extension_section_rows(m, screen, NULL, 0) > 0;
        if (screen == MAINUI_SCREEN_SET_DATE) finish_date(m, w);
        pop(m);
        if (zone_parent) top(m)->focus = top(m)->first = 0;
        if (screen_persists(screen)) persist(m);
        // Values that have not settled yet run now.
        if (extended) effect(m, MAINUI_EFFECT_EXTENSION_LEAVE)->value = screen;
        // A mapping change not yet written is written now; Keymap's rotation can change Layout.
        if (screen == MAINUI_SCREEN_BUTTON_MAPPING || screen == MAINUI_SCREEN_KEYMAP) effect(m, MAINUI_EFFECT_SAVE_MAPPING);
        // An appearance change not yet written is written now.
        if (screen == MAINUI_SCREEN_APPEARANCE || screen == MAINUI_SCREEN_APPEARANCE_FONT) effect(m, MAINUI_EFFECT_SAVE_APPEARANCE);
        return;
    }
    struct mainui_item fixed_items[SCREEN_ITEMS_MAX];
    int capacity = mainui_model_screen_item_capacity(m, w->screen);
    int dynamic_visuals = capacity > SCREEN_ITEMS_MAX;
    if (!dynamic_visuals) capacity = SCREEN_ITEMS_MAX;
    struct mainui_item *items = dynamic_visuals ? calloc((size_t)capacity, sizeof(*items)) : fixed_items;
    if (!items) return;
    int n = mainui_model_screen_items(m, w->screen, items, capacity);
    if (n == 0) goto done;
    // A list that shrank (a device went away) leaves the focus on its last row.
    if (w->focus >= n) w->focus = n - 1;
    if (items[w->focus].kind == MAINUI_ITEM_HEADER) w->focus = focus_step(items, n, w->focus, -1);
    const struct mainui_item *item = &items[w->focus];
    // MENU on a device of My Devices opens its menu, Remove (d1); elsewhere it does nothing
    // (k2a: Display; k2f: the Bluetooth switch and a found device).
    if (key == MAINUI_KEY_MENU) {
        if (item->kind == MAINUI_ITEM_LIST && item->action == MAINUI_LIST_PAIRED)
            open_popup(m, MAINUI_POPUP_BT_DEVICE, MAINUI_LIST_PAIRED, -1, list_entry(items, w->focus));
        goto done;
    }
    switch (key) {
    case MAINUI_KEY_UP:
        // Settings lists wrap at both ends (Display, System, Language, Date And Time), and the
        // read-only Device Info rows take the focus too (e13e); section titles do not (w1).
        w->focus = focus_step(items, n, w->focus, -1);
        keep_visible(&w->first, w->focus, 6, n);
        if (w->screen == MAINUI_SCREEN_THEME_LIST)
            effect(m, MAINUI_EFFECT_PREVIEW_THEME)->index = list_entry(items, w->focus);
        goto done;
    case MAINUI_KEY_DOWN:
        w->focus = focus_step(items, n, w->focus, 1);
        keep_visible(&w->first, w->focus, 6, n);
        if (w->screen == MAINUI_SCREEN_THEME_LIST)
            effect(m, MAINUI_EFFECT_PREVIEW_THEME)->index = list_entry(items, w->focus);
        goto done;
    case MAINUI_KEY_LEFT:
    case MAINUI_KEY_RIGHT: {
        int step = key == MAINUI_KEY_RIGHT ? 1 : -1;
        if (item->kind == MAINUI_ITEM_RANGE) {
            int value = m->settings[item->setting] + step;
            if (value < item->min) value = item->min;
            if (value > item->max) value = item->max;
            // Stock range handlers re-apply their callback at either limit.
            // Contrast/Exposure also repair a loaded zero to the UI minimum, 1.
            set_setting(m, item->setting, value);
        } else if (item->kind == MAINUI_ITEM_CYCLE) {
            // An invalid profile is kept as it is until Reset.
            if (!(m->mapping_locked && mainui_model_mapping_setting(item->setting))) cycle_item(m, item, step);
        } else if (item->kind == MAINUI_ITEM_EXTENSION) {
            extension_step(m, item->action, step);
        } else if (item->kind == MAINUI_ITEM_INITIAL_VIEW_CHOICE) {
            // These choices are selected with the configured confirm button; horizontal
            // movement leaves the saved preference untouched.
        } else if (item->kind == MAINUI_ITEM_DATE) {
            int max = item->action == 2 ? date_days(w) : item->max;
            int current = w->date[item->action];
            // Stock leaves an invalid day intact when month/year changes; mktime normalizes
            // the complete calendar on exit. Only a day key wraps to this month's bounds.
            // LEFT tests the lower bound before decrementing. A retained day above
            // February's length therefore walks down through 30/29 rather than
            // wrapping early; RIGHT tests the upper bound before incrementing.
            w->date[item->action] = step < 0 ? (current <= item->min ? max : current - 1) :
                                             (current >= max ? item->min : current + 1);
        }
        goto done;
    }
    default:
        break;
    }
    if (!confirm) goto done;
    switch (item->kind) {
    case MAINUI_ITEM_PRESENTATION_CHOICE:
        if (w->screen == MAINUI_SCREEN_FRONTEND &&
            (item->action == MAINUI_PRESENTATION_STOCK || item->action == MAINUI_PRESENTATION_MATERIAL_YOU))
            effect(m, MAINUI_EFFECT_SAVE_PRESENTATION)->value = item->action;
        break;
    case MAINUI_ITEM_INITIAL_VIEW_CHOICE:
        if (w->screen == MAINUI_SCREEN_BRICKOS_STARTUP &&
            item->action >= MAINUI_BRICKOS_VIEW_DESKTOP && item->action < MAINUI_BRICKOS_VIEW_COUNT) {
            m->brickos_initial_view = (enum mainui_brickos_initial_view)item->action;
            effect(m, MAINUI_EFFECT_SAVE_BRICKOS_PREFERENCES)->value = item->action;
        }
        break;
    case MAINUI_ITEM_SWITCH:
        set_setting(m, item->setting, !m->settings[item->setting]);
        // Wi-Fi and Bluetooth write system.json at once (g08, g09).
        if (item->setting == MAINUI_SET_WIFI || item->setting == MAINUI_SET_BLUETOOTH) persist(m);
        // Bluetooth waits under a progress message until the services are up or down (w1).
        if (item->setting == MAINUI_SET_BLUETOOTH) {
            struct mainui_window *progress = push(m, MAINUI_WIN_PROGRESS);
            if (progress)
                progress->progress = m->settings[MAINUI_SET_BLUETOOTH] ? MAINUI_PROGRESS_BLUETOOTH_ON : MAINUI_PROGRESS_BLUETOOTH_OFF;
        }
        break;
    case MAINUI_ITEM_SUBMENU:
        open_screen(m, item->screen);
        break;
    case MAINUI_ITEM_EXTENSION:
        extension_confirm(m, item->action);
        break;
    case MAINUI_ITEM_ACTION:
        switch ((enum mainui_action)item->action) {
        case MAINUI_ACTION_POWER_OFF: open_dialog(m, MAINUI_DIALOG_POWER_OFF, -1, -1, -1); break;
        case MAINUI_ACTION_FACTORY_RESET: open_dialog(m, MAINUI_DIALOG_FACTORY_RESET, -1, -1, -1); break;
        // Flash Mode asks first, like Power off (static: RebootEfexAction's dialog).
        case MAINUI_ACTION_FLASH_MODE: open_dialog(m, MAINUI_DIALOG_FLASH_MODE, -1, -1, -1); break;
        case MAINUI_ACTION_REFRESH_ROMS: refresh_roms(m); break;
        // The hotspot's name and password are edited with the keyboard (static).
        case MAINUI_ACTION_SSID: open_keyboard(m, -1, MAINUI_TYPE_HOTSPOT_SSID, -1); break;
        case MAINUI_ACTION_PASSWORD: open_keyboard(m, -1, MAINUI_TYPE_HOTSPOT_PASSWORD, -1); break;
        case MAINUI_ACTION_SET_DATE: open_screen(m, MAINUI_SCREEN_SET_DATE); break;
        case MAINUI_ACTION_RESET_MAPPING: open_dialog(m, MAINUI_DIALOG_RESET_MAPPING, -1, -1, -1); break;
        case MAINUI_ACTION_RESET_APPEARANCE: open_dialog(m, MAINUI_DIALOG_RESET_APPEARANCE, -1, -1, -1); break;
        }
        break;
    case MAINUI_ITEM_LIST: {
        int entry = list_entry(items, w->focus);
        if (item->action == MAINUI_LIST_BOOTLOGOS || item->action == MAINUI_LIST_SPLASHSCREENS) {
            struct mainui_effect *e = effect(m, MAINUI_EFFECT_APPLY_BOOT_VISUAL);
            e->list = item->action;
            e->index = entry == 0 ? MAINUI_BOOT_VISUAL_RANDOM : entry - 1;
            break;
        }
        if (item->action == MAINUI_LIST_LOADING_SCREENS) {
            effect(m, MAINUI_EFFECT_APPLY_LOADING_SCREEN)->index = entry;
            break;
        }
        if (item->action == MAINUI_LIST_WIFI) {
            // A saved network, connected or not, opens its popup (k2b: Connect, Change
            // Password, Forget); a locked one asks its password; an open one connects at once
            // (static: WifiItemAction::dowork).
            int flags = entry_flags(m, MAINUI_LIST_WIFI, 0, entry);
            if (flags & MAINUI_ENTRY_SAVED) open_popup(m, MAINUI_POPUP_WIFI, MAINUI_LIST_WIFI, -1, entry);
            else if (flags & MAINUI_ENTRY_LOCKED) open_keyboard(m, -1, MAINUI_TYPE_WIFI_PASSWORD, entry);
            else wifi_work(m, MAINUI_EFFECT_WIFI_CONNECT, MAINUI_PROGRESS_WIFI, entry)->value = 1;
            break;
        }
        if (item->action == MAINUI_LIST_TIMEZONES) {
            // The zoneinfo browser: a folder opens its own list, a zone is linked and stored
            // at once, and the browser closes back to Date And Time from any folder (k2e5;
            // static: FileAction::dowork saves system.json and returns the result that closes
            // the browser, a single window that changes folder in place).
            int level = w->system;
            if (level > 0 && entry == 0) {
                pop(m);
                top(m)->focus = top(m)->first = 0;
                break;
            }
            list_effect(m, MAINUI_EFFECT_SELECT, MAINUI_LIST_TIMEZONES, level, entry);
            if (entry_flags(m, MAINUI_LIST_TIMEZONES, level, entry) & MAINUI_ENTRY_FOLDER) {
                struct mainui_window *folder = open_screen(m, MAINUI_SCREEN_TIMEZONE);
                if (folder) folder->system = level + 1;
            } else {
                persist(m);
                while (m->depth > 1 && top(m)->kind == MAINUI_WIN_SETTINGS && top(m)->screen == MAINUI_SCREEN_TIMEZONE) pop(m);
            }
            break;
        }
        list_effect(m, MAINUI_EFFECT_SELECT, item->action, -1, entry);
        if (item->action == MAINUI_LIST_THEMES) {
            // The list closes back to the Themes menu, on its Theme row. A changed theme still
            // resets the tabs as the stock rebuild does (theme-unchanged-official), but the
            // Settings grid keeps the Themes tile for the way back.
            int changed = m->settings[MAINUI_SET_THEME] != entry;
            set_setting(m, MAINUI_SET_THEME, entry);
            persist(m);
            pop(m);
            if (changed) {
                refresh_theme_tabs(m);
                m->tabs[MAINUI_TAB_SETTINGS].focus = MAINUI_SCREEN_THEMES;
                m->tabs[MAINUI_TAB_SETTINGS].first = 0;
            }
        } else if (item->action == MAINUI_LIST_LANGUAGES) {
            // A language applies and the list closes back to System, on its row (k2e2).
            set_setting(m, MAINUI_SET_LANGUAGE, entry);
            persist(m);
            pop(m);
        } else if (item->action == MAINUI_LIST_FONTS) {
            // A font applies and the list closes back to Appearance, on its Font row, and is saved.
            pop(m);
            effect(m, MAINUI_EFFECT_SAVE_APPEARANCE);
        }
        break;
    }
    default:
        break;
    }
done:
    if (dynamic_visuals) free(items);
}

// ---- Status bar ----------------------------------------------------------------------------

// Static: draw_nav_bar formats "%02d:%02d" and, without the 24-hour clock, takes 12 from an hour
// past 12 and draws "PM" when hour * 100 + minute is above 1299, else "AM". So the official marks
// 12:00 to 12:59 "AM"; BrickOS marks them "PM" (a stock defect). Midnight stays 00, as there.
const char *mainui_status_clock(const char *clock, int hour24, char *out, int size) {
    int hour, minute;
    snprintf(out, (size_t)size, "%s", clock);
    if (hour24 || sscanf(clock, "%d:%d", &hour, &minute) != 2) return NULL;
    snprintf(out, (size_t)size, "%02d:%02d", hour > 12 ? hour - 12 : hour, minute);
    return hour >= 12 ? "PM" : "AM";
}

// Static: draw_battery_icon.
const char *mainui_status_battery_icon(int percent) {
    return percent > 79 ? "ic-batery-100%.png" : percent > 49 ? "ic-batery-75%.png" : percent > 24 ? "ic-batery-50%.png"
         : percent > 4  ? "ic-batery-25%.png"
                        : "ic-batery-0%.png";
}

// Static: draw_battery_icon gives one digit 30 pixels, two 45, and more 60.
int mainui_status_percent_box(int percent) {
    return percent <= 9 ? 30 : percent <= 99 ? 45 : 60;
}

// ---- Keyboard and search -------------------------------------------------------------------

char mainui_keyboard_char(int key, int shift, int symbols) {
    static const char rows[] = "1234567890qwertyuiopasdfghjkl-zxcvbnm()/";
    // The symbol page (k1); shift leaves it as it is.
    static const char symbol_rows[] = "1234567890!@#$%^&*/?~_+{}|:\"<>`-=[]\\;',.";
    if (key < 0 || key >= 40) return 0;
    if (symbols) return symbol_rows[key];
    char c = rows[key];
    if (shift && c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
    return c;
}

// The bottom row's five keys (shift, symbols, space, delete, OK) sit under column pairs (e20).
static int keyboard_bottom_key(int column) {
    return 40 + column / 2;
}

static int keyboard_bottom_column(int key) {
    return (key - 40) * 2;
}

static void keyboard_key(struct mainui_model *m, enum mainui_key key, int confirm, int back) {
    struct mainui_window *w = top(m);
    int f = w->focus;
    if (back) { pop(m); return; }
    // The bottom keys show L, R, X, Y, and START as their buttons (e20), and those buttons press
    // them: shift, symbols, space, and delete, with the focus left where it is (kb1).
    if (key == MAINUI_KEY_L1) f = 40, confirm = 1;
    else if (key == MAINUI_KEY_R1) f = 41, confirm = 1;
    else if (key == MAINUI_KEY_X) f = 42, confirm = 1;
    else if (key == MAINUI_KEY_Y) f = 43, confirm = 1;
    if (confirm && f == 44) key = MAINUI_KEY_START;
    // Left and right wrap within a row, up from the top row goes to the bottom key under it, and
    // down from a bottom key to the top row (k1). The L, R, X, and Y shortcuts leave the focus.
    switch (key) {
    case MAINUI_KEY_LEFT:
        if (f < 40) w->focus = (f / 10) * 10 + (f % 10 + 9) % 10;
        else w->focus = 40 + (f - 40 + 4) % 5;
        return;
    case MAINUI_KEY_RIGHT:
        if (f < 40) w->focus = (f / 10) * 10 + (f % 10 + 1) % 10;
        else w->focus = 40 + (f - 40 + 1) % 5;
        return;
    case MAINUI_KEY_UP:
        if (f >= 40) w->focus = 30 + keyboard_bottom_column(f);
        else if (f < 10) w->focus = keyboard_bottom_key(f);
        else w->focus = f - 10;
        return;
    case MAINUI_KEY_DOWN:
        if (f >= 40) w->focus = keyboard_bottom_column(f);
        else if (f >= 30) w->focus = keyboard_bottom_key(f - 30);
        else w->focus = f + 10;
        return;
    case MAINUI_KEY_START: {
        // START is OK: run the search and show the results, or hand the text over.
        struct mainui_window typed = *w;
        if (typed.purpose == MAINUI_TYPE_WIFI_PASSWORD) {
            pop(m);
            struct mainui_effect *e = wifi_work(m, MAINUI_EFFECT_WIFI_CONNECT, MAINUI_PROGRESS_WIFI, typed.index);
            memcpy(e->text, typed.text, sizeof(e->text));
            return;
        }
        if (typed.purpose != MAINUI_TYPE_SEARCH) {
            pop(m);
            struct mainui_effect *e = effect(m, MAINUI_EFFECT_EDIT_TEXT);
            memcpy(e->text, typed.text, sizeof(e->text));
            e->index = typed.index;
            e->value = typed.purpose == MAINUI_TYPE_HOTSPOT_PASSWORD;
            return;
        }
        struct mainui_effect *e = effect(m, MAINUI_EFFECT_SEARCH);
        memcpy(e->text, typed.text, sizeof(e->text));
        e->system = typed.system;
        struct mainui_window *results = push(m, MAINUI_WIN_SEARCH);
        if (results) {
            results->system = typed.system;
            memcpy(results->text, typed.text, sizeof(results->text));
        }
        return;
    }
    default:
        break;
    }
    if (!confirm) return;
    size_t length = strlen(w->text);
    if (f < 40) {
        if (length + 1 < sizeof(w->text)) {
            w->text[length] = mainui_keyboard_char(f, w->shift, w->symbols);
            w->text[length + 1] = 0;
        }
    } else if (f == 40) {
        w->shift = !w->shift;
    } else if (f == 41) {
        w->symbols = !w->symbols;
    } else if (f == 42) {
        if (length + 1 < sizeof(w->text)) { w->text[length] = ' '; w->text[length + 1] = 0; }
    } else if (length) {
        // Text can contain UTF-8 although the keyboard's keys are ASCII.
        // Delete one code point, not one byte.
        size_t start = length - 1;
        while (start && ((unsigned char)w->text[start] & 0xc0) == 0x80) --start;
        w->text[start] = 0;
    }
}

// Up and down wrap; left and right page by six and stop at the ends (the system lists).
static void list_move(int *focus, int *first, int n, int rows, enum mainui_key key) {
    if (n <= 0) return;
    switch (key) {
    case MAINUI_KEY_UP: *focus = *focus > 0 ? *focus - 1 : n - 1; break;
    case MAINUI_KEY_DOWN: *focus = *focus + 1 < n ? *focus + 1 : 0; break;
    case MAINUI_KEY_RIGHT: *focus = *focus + MAINUI_ROM_PAGE < n ? *focus + MAINUI_ROM_PAGE : n - 1; break;
    case MAINUI_KEY_LEFT: *focus = *focus >= MAINUI_ROM_PAGE ? *focus - MAINUI_ROM_PAGE : 0; break;
    default: return;
    }
    keep_visible(first, *focus, rows, n);
}

// The Appearance choice behind a window's view, or -1 when the window always shows its list.
static int view_setting(const struct mainui_model *m, const struct mainui_window *w) {
    if (m->presentation != MAINUI_PRESENTATION_MATERIAL_YOU || !w) return -1;
    if (w->kind == MAINUI_WIN_ROMS)
        return w->system >= 0 && w->system < MAINUI_BEST_SYSTEM_BASE ? MAINUI_SET_APPEAR_GAME_VIEW : -1;
    if (w->kind != MAINUI_WIN_TABS) return -1;
    return m->tab == MAINUI_TAB_RECENT ? MAINUI_SET_APPEAR_RECENT_VIEW :
           m->tab == MAINUI_TAB_FAVORITES ? MAINUI_SET_APPEAR_FAVORITES_VIEW : -1;
}

int mainui_model_view(const struct mainui_model *m, const struct mainui_window *w) {
    int setting = view_setting(m, w);
    int view = setting < 0 ? MAINUI_VIEW_LIST : m->settings[setting];
    return view >= 0 && view < MAINUI_VIEWS ? view : MAINUI_VIEW_LIST;
}

// START: List, Grid, Carousel, List, through the same SET an Appearance row sends, so the row and
// appearance.json follow. The focus stays.
static void cycle_view(struct mainui_model *m, const struct mainui_window *w) {
    int setting = view_setting(m, w);
    if (setting >= 0) set_setting(m, (enum mainui_setting)setting, (mainui_model_view(m, w) + 1) % MAINUI_VIEWS);
}

// A page key in the Grid (a page) or Carousel (MAINUI_ROM_PAGE) view: stops at both ends.
static int page_step(int focus, int n, int step) {
    int next = focus + step;
    return next < 0 ? 0 : next >= n ? n - 1 : next;
}

// Grid and Carousel movement for the focus of a game list or a Recent or Favorites tab. Returns
// 1 when the key was a move, and sets *leave for UP past the top (the Grid's top row, or the
// Carousel); first stays what the List view would hold, for state.json and a switch back.
static int view_move(int view, int *focus, int n, enum mainui_key key, int pages, int *leave) {
    *leave = 0;
    if (n <= 0) { *leave = key == MAINUI_KEY_UP; return key == MAINUI_KEY_UP; }
    if (*focus >= n) *focus = n - 1;
    if (pages && (key == MAINUI_KEY_L1 || key == MAINUI_KEY_R1)) {
        int step = view == MAINUI_VIEW_GRID ? MAINUI_GAME_GRID_PAGE : MAINUI_ROM_PAGE;
        *focus = page_step(*focus, n, key == MAINUI_KEY_L1 ? -step : step);
        return 1;
    }
    if (view == MAINUI_VIEW_GRID) {
        if (key != MAINUI_KEY_UP && key != MAINUI_KEY_DOWN && key != MAINUI_KEY_LEFT && key != MAINUI_KEY_RIGHT) return 0;
        int next = tiled_move(*focus, n, key, MAINUI_GAME_GRID_COLUMNS, MAINUI_GAME_GRID_ROWS);
        if (next < 0) *leave = 1;
        else *focus = next;
        return 1;
    }
    if (key == MAINUI_KEY_LEFT || key == MAINUI_KEY_RIGHT) { *focus = row_move(*focus, n, key); return 1; }
    if (key == MAINUI_KEY_UP) { *leave = 1; return 1; }
    return key == MAINUI_KEY_DOWN;
}

static void roms_key(struct mainui_model *m, enum mainui_key key, int confirm, int back) {
    struct mainui_window *w = top(m);
    int n = count(m, MAINUI_LIST_ROMS, w->system);
    if (back) { pop(m); return; }
    if (key == MAINUI_KEY_MENU) { open_popup(m, MAINUI_POPUP_ROMS, MAINUI_LIST_ROMS, w->system, n ? w->focus : -1); return; }
    if (key == MAINUI_KEY_START) { cycle_view(m, w); return; }
    if (n == 0) return;
    if (confirm || key == MAINUI_KEY_Y) { launch(m, MAINUI_LIST_ROMS, w->system, w->focus, -1); return; }
    if (key == MAINUI_KEY_L2 || key == MAINUI_KEY_R2) {
        struct mainui_effect *e = effect(m, MAINUI_EFFECT_JUMP_GROUP);
        e->list = MAINUI_LIST_ROMS; e->system = w->system; e->index = w->focus;
        e->value = key == MAINUI_KEY_L2 ? -1 : 1;
        return;
    }
    if (key == MAINUI_KEY_X) {
        if (count(m, MAINUI_LIST_LAUNCHERS, w->system) > 0)
            open_popup(m, MAINUI_POPUP_LAUNCHERS, MAINUI_LIST_ROMS, w->system, w->focus);
        return;
    }
    int view = mainui_model_view(m, w), leave;
    if (view != MAINUI_VIEW_LIST) {
        // UP past the top stays: a game list has nothing above it.
        if (view_move(view, &w->focus, n, key, 1, &leave)) keep_visible(&w->first, w->focus, MAINUI_ROM_LIST_ROWS, n);
        return;
    }
    // L1 and R1 page like left and right (k2d: 1 → 7 → 13 → 7).
    if (key == MAINUI_KEY_L1) key = MAINUI_KEY_LEFT;
    else if (key == MAINUI_KEY_R1) key = MAINUI_KEY_RIGHT;
    list_move(&w->focus, &w->first, n, MAINUI_ROM_LIST_ROWS, key);
}

static void search_key(struct mainui_model *m, enum mainui_key key, int confirm, int back) {
    struct mainui_window *w = top(m);
    int n = count(m, MAINUI_LIST_SEARCH, 0);
    if (back) { pop(m); return; }  // back to the keyboard, with its text
    if (n == 0) return;
    if (key == MAINUI_KEY_MENU) { open_popup(m, MAINUI_POPUP_SEARCH, MAINUI_LIST_SEARCH, w->system, w->focus); return; }
    if (confirm) { launch(m, MAINUI_LIST_SEARCH, w->system, w->focus, -1); return; }
    // Results show five two-line rows (e20). Left and right do nothing; L1 and R1 page by six
    // like the system lists (k2g: 1 -> 7 -> 1).
    if (key == MAINUI_KEY_LEFT || key == MAINUI_KEY_RIGHT) return;
    if (key == MAINUI_KEY_L1) key = MAINUI_KEY_LEFT;
    else if (key == MAINUI_KEY_R1) key = MAINUI_KEY_RIGHT;
    list_move(&w->focus, &w->first, n, mainui_search_rows(w->system), key);
}

static void edit_key(struct mainui_model *m, enum mainui_key key, int confirm, int back) {
    struct mainui_window *w = top(m);
    int n = count(m, MAINUI_LIST_EDIT, w->system);
    // Leaving the editor asks the Save dialog (e15).
    if (back) { open_dialog(m, MAINUI_DIALOG_SAVE_EDIT, MAINUI_LIST_EDIT, w->system, -1); return; }
    if (n == 0) return;
    if (confirm) {
        w->dirty = 1;
        list_effect(m, MAINUI_EFFECT_TOGGLE_EDIT, MAINUI_LIST_EDIT, w->system, w->focus);
        return;
    }
    // The editor pages like the Games grid: 3 columns, 2 rows, by label (e15).
    int next = grid_move(w->focus, n, key);
    if (next >= 0) w->focus = next;
}

// ---- Home and tab windows ------------------------------------------------------------------

static void home_key(struct mainui_model *m, enum mainui_key key, int confirm, int back) {
    int tabs[MAINUI_HOME_COUNT];
    int n = mainui_model_home_items(m, tabs), pos = 0;
    while (pos < n && tabs[pos] != m->home_focus) ++pos;
    if (pos >= n) pos = n;
    if (m->presentation == MAINUI_PRESENTATION_MATERIAL_YOU && (n <= 0 || pos >= n)) {
        m->home_focus = n > 0 ? tabs[0] : MAINUI_TAB_SETTINGS;
        m->home_search = 0;
        m->home_first = 0;
        pos = 0;
    }
    // Material You uses one horizontal DPAD carousel; Up and Down transfer focus to the search
    // affordance above it. The stock home retains its measured shortcut-row behavior.
    switch (key) {
    case MAINUI_KEY_LEFT:
        if (!m->home_search) {
            int target = m->presentation == MAINUI_PRESENTATION_MATERIAL_YOU ? material_home_move(pos, n, key) : (pos + n - 1) % n;
            if (m->presentation == MAINUI_PRESENTATION_STOCK || target >= 0) m->home_focus = tabs[target];
        }
        home_fit(m);
        return;
    case MAINUI_KEY_RIGHT:
        if (!m->home_search) {
            int target = m->presentation == MAINUI_PRESENTATION_MATERIAL_YOU ? material_home_move(pos, n, key) : (pos + 1) % n;
            if (m->presentation == MAINUI_PRESENTATION_STOCK || target >= 0) m->home_focus = tabs[target];
        }
        home_fit(m);
        return;
    case MAINUI_KEY_UP:
    case MAINUI_KEY_DOWN:
        if (m->presentation == MAINUI_PRESENTATION_MATERIAL_YOU) {
            m->home_search = key == MAINUI_KEY_UP;
            return;
        }
        m->home_search = key == MAINUI_KEY_UP;
        return;
    default: break;
    }
    // The stock homepage shortcut row can return to a tab bar; the BrickOS desktop is a root.
    if (back) {
        if (m->presentation == MAINUI_PRESENTATION_MATERIAL_YOU) return;
        open_tabs(m, (enum mainui_tab)m->home_focus, 1);
        return;
    }
    // Y opened the focused tab like A (n03). The GameSwitcher card opens its app, as Apps does.
    if (confirm || key == MAINUI_KEY_Y) {
        if (m->home_search) {
            open_keyboard(m, -1, MAINUI_TYPE_SEARCH, -1);
        } else if (m->home_focus == MAINUI_HOME_GAMESWITCHER) {
            effect(m, MAINUI_EFFECT_LAUNCH_GAMESWITCHER)->value = 1;
        } else {
            open_tabs(m, (enum mainui_tab)m->home_focus, 0);
        }
    }
}

static void change_tab(struct mainui_model *m, int step) {
    enum mainui_tab tabs[MAINUI_TAB_COUNT];
    int n, pos = tab_position(m, m->tab, tabs, &n);
    // Tabs stop at both ends (e01, n03).
    int next = pos + step;
    if (next >= 0 && next < n) m->tab = tabs[next];
}

// START cycles the level-1 layout: 1 the paged grid, 2 the compact grid, 0 one row (r8, r9). The
// grid pages open on their first item again (r8: GBA to DC; r9: SFC to DC).
static void toggle_layout(struct mainui_model *m) {
    int style = m->settings[MAINUI_SET_MENUSTYLEL1];
    set_setting(m, MAINUI_SET_MENUSTYLEL1, style == 1 ? 2 : style == 2 ? 0 : 1);
    for (int tab = 0; tab < MAINUI_TAB_COUNT; ++tab)
        if (!list_tab((enum mainui_tab)tab)) m->tabs[tab].focus = m->tabs[tab].first = 0;
    persist(m);
}

static void leave_page(struct mainui_model *m) {
    if (m->settings[MAINUI_SET_FOCUSONTAB]) m->bar_focused = 1;
    else if (m->settings[MAINUI_SET_ENABLEHOME]) open_home(m);
}

static void page_key(struct mainui_model *m, enum mainui_key key, int confirm) {
    enum mainui_tab tab = m->tab;
    struct mainui_tab_state *t = &m->tabs[tab];
    int n = tab_count(m, tab);
    if (t->focus >= n) t->focus = n ? n - 1 : 0;
    if (list_tab(tab)) {
        enum mainui_list list = tab_list(tab);
        switch (key) {
        case MAINUI_KEY_UP:
            // Up from the first entry moves to the tab bar (n04).
            if (t->focus == 0 || n == 0) { leave_page(m); return; }
            --t->focus;
            break;
        case MAINUI_KEY_DOWN:
            // Down from the last entry wraps to the first (n09).
            if (n) t->focus = t->focus + 1 < n ? t->focus + 1 : 0;
            break;
        default:
            // Left and right do nothing in a tab list (n07).
            if (n && (confirm || key == MAINUI_KEY_Y)) launch(m, list, 0, t->focus, -1);
            return;
        }
        keep_visible(&t->first, t->focus, MAINUI_TAB_LIST_ROWS, n);
        return;
    }
    if (confirm) {
        if (!n) return;
        if (tab == MAINUI_TAB_GAMES || tab == MAINUI_TAB_BESTS) {
            struct mainui_window *w = push(m, MAINUI_WIN_ROMS);
            if (w) {
                // The list opens where it was left, else at its top (static: MenuWindow::init and
                // DBCachedTextMenu::restorePos take the focus when it is a row of the list and the
                // first row when both saved rows are; fc1, the DC list at 4/58 with row 4 on top).
                w->system = t->focus + (tab == MAINUI_TAB_BESTS ? MAINUI_BEST_SYSTEM_BASE : 0);
                int games = count(m, MAINUI_LIST_ROMS, w->system), pos, start, end;
                if (m->catalog.position && m->catalog.position(m->catalog.context, MAINUI_LIST_ROMS, w->system, &pos, &start, &end) == 0) {
                    if (pos >= 0 && pos < games) w->focus = pos;
                    if (start >= 0 && end >= 0 && start < games && end < games) w->first = start;
                    keep_visible(&w->first, w->focus, MAINUI_ROM_LIST_ROWS, games);
                }
            }
        } else if (tab == MAINUI_TAB_SETTINGS) {
            // The stock tiles open their screens in enum order; the tiles after them open
            // settings extensions.
            if (t->focus < MAINUI_SETTINGS_TILES) open_screen(m, (enum mainui_screen)t->focus);
            else open_extension(m, m->extensions.tile_node ? m->extensions.tile_node(m->extensions.context, t->focus - MAINUI_SETTINGS_TILES) : -1);
        } else {
            launch(m, tab_list(tab), 0, t->focus, -1);
        }
        return;
    }
    if (key != MAINUI_KEY_UP && key != MAINUI_KEY_DOWN && key != MAINUI_KEY_LEFT && key != MAINUI_KEY_RIGHT) return;
    int style = mainui_model_tab_layout(m, tab);
    int next = style == 2 ? compact_move(t->focus, n, key) : style == 0 ? row_move(t->focus, n, key) : grid_move(t->focus, n, key);
    if (next < 0) leave_page(m);
    else t->focus = next;
}

static void tabs_key(struct mainui_model *m, enum mainui_key key, int confirm, int back) {
    struct mainui_tab_state *t = &m->tabs[m->tab];
    int entry = list_tab(m->tab) && tab_count(m, m->tab) ? t->focus : -1;
    switch (key) {
    // On BrickOS's Recent and Favorites START changes their view; elsewhere the level-1 layout.
    case MAINUI_KEY_START:
        if (view_setting(m, top(m)) >= 0) cycle_view(m, top(m));
        else toggle_layout(m);
        return;
    case MAINUI_KEY_SELECT: open_keyboard(m, -1, MAINUI_TYPE_SEARCH, -1); return;
    case MAINUI_KEY_MENU: open_popup(m, MAINUI_POPUP_TAB, (int)tab_list(m->tab), 0, entry); return;
    default: break;
    }
    if (m->bar_focused) {
        if (back) {
            // With the home page off, B stops at the tab bar (g05).
            if (m->settings[MAINUI_SET_ENABLEHOME]) open_home(m);
            return;
        }
        switch (key) {
        case MAINUI_KEY_LEFT: case MAINUI_KEY_L1: change_tab(m, -1); return;
        case MAINUI_KEY_RIGHT: case MAINUI_KEY_R1: change_tab(m, 1); return;
        case MAINUI_KEY_DOWN: m->bar_focused = 0; return;
        default: break;
        }
        // A on the tab bar moves to the page (n03).
        if (confirm) m->bar_focused = 0;
        return;
    }
    if (back) {
        if (m->presentation == MAINUI_PRESENTATION_MATERIAL_YOU) {
            m->home_focus = m->tab;
            open_home(m);
        } else leave_page(m);
        return;
    }
    if (key == MAINUI_KEY_L1) { change_tab(m, -1); return; }
    if (key == MAINUI_KEY_R1) { change_tab(m, 1); return; }
    if (m->presentation == MAINUI_PRESENTATION_MATERIAL_YOU) {
        if (list_tab(m->tab)) {
            int n = tab_count(m, m->tab);
            if (t->focus >= n) t->focus = n ? n - 1 : 0;
            int view = mainui_model_view(m, top(m)), leave;
            if (view != MAINUI_VIEW_LIST) {
                if ((confirm || key == MAINUI_KEY_Y) && n) launch(m, tab_list(m->tab), 0, t->focus, -1);
                else if (view_move(view, &t->focus, n, key, 0, &leave) && leave) { m->home_focus = m->tab; open_home(m); return; }
                keep_visible(&t->first, t->focus, MAINUI_TAB_LIST_ROWS, n);
                return;
            }
            if (key == MAINUI_KEY_UP) {
                if (t->focus == 0 || !n) { m->home_focus = m->tab; open_home(m); return; }
                --t->focus;
            } else if (key == MAINUI_KEY_DOWN && t->focus + 1 < n) ++t->focus;
            else if ((confirm || key == MAINUI_KEY_Y) && n)
                launch(m, tab_list(m->tab), 0, t->focus, -1);
            keep_visible(&t->first, t->focus, MAINUI_TAB_LIST_ROWS, n);
            return;
        }
        if (m->tab == MAINUI_TAB_GAMES || m->tab == MAINUI_TAB_BESTS ||
            m->tab == MAINUI_TAB_APPS || m->tab == MAINUI_TAB_PORTS) {
            if (confirm) { page_key(m, key, confirm); return; }
            if (key == MAINUI_KEY_UP || key == MAINUI_KEY_DOWN ||
                key == MAINUI_KEY_LEFT || key == MAINUI_KEY_RIGHT) {
                int n = tab_count(m, m->tab);
                int style = mainui_model_tab_layout(m, m->tab);
                int next = style == 2 ? compact_move(t->focus, n, key) :
                           style == 0 ? row_move(t->focus, n, key) : grid_move(t->focus, n, key);
                if (n > 0 && next >= 0) t->focus = next;
                return;
            }
            return;
        }
        if (confirm) { page_key(m, key, confirm); return; }
        if (key == MAINUI_KEY_UP || key == MAINUI_KEY_DOWN ||
            key == MAINUI_KEY_LEFT || key == MAINUI_KEY_RIGHT) {
            int n = tab_count(m, m->tab);
            int next = grid_move(t->focus, n, key);
            if (n > 0 && next >= 0) t->focus = next;
            int per_page = MAINUI_GRID_COLUMNS * MAINUI_GRID_ROWS;
            t->first = (t->focus / per_page) * per_page;
            return;
        }
        return;
    }
    page_key(m, key, confirm);
}

// Only advertise controls handled by the active view. Stock rendering does not use this list.
int mainui_model_hints(const struct mainui_model *m, struct mainui_hint hints[MAINUI_HINTS_MAX]) {
    const struct mainui_window *w = mainui_model_top(m);
    const char *confirm = m->settings[MAINUI_SET_BCONFIRM] == 1 ? "B" : "A";
    const char *back = m->settings[MAINUI_SET_BCONFIRM] == 1 ? "A" : "B";
    int n = 0, entries = 0;
#define HINT(key_, action_) do { if (n < MAINUI_HINTS_MAX) hints[n++] = (struct mainui_hint){key_, MAINUI_HINT_##action_}; } while (0)
    switch (w->kind) {
    case MAINUI_WIN_HOME:
        HINT(m->home_search ? "↑/↓" : "DPAD", MOVE);
        if (m->home_search) HINT(confirm, SEARCH);
        else HINT(confirm, OPEN);
        break;
    case MAINUI_WIN_TABS: {
        entries = tab_count(m, m->tab);
        int view = mainui_model_view(m, w);
        if (entries > 0 || m->bar_focused)
            HINT(m->bar_focused ? "DPAD" : view == MAINUI_VIEW_GRID ? "DPAD" : view == MAINUI_VIEW_CAROUSEL ? "←/→" :
                 list_tab(m->tab) ? "↑/↓" : "DPAD", MOVE);
        if (entries > 0 || m->bar_focused) {
            if (m->bar_focused) HINT(confirm, SELECT);
            else if (m->tab == MAINUI_TAB_GAMES || m->tab == MAINUI_TAB_BESTS || m->tab == MAINUI_TAB_SETTINGS)
                HINT(confirm, OPEN);
            else HINT(confirm, LAUNCH);
        }
        HINT(back, BACK);
        HINT("MENU", ACTIONS);
        HINT("SELECT", SEARCH);
        // These are the catalogs whose visible geometry actually changes with START.
        // Recent and Favorites in BrickOS change their view.
        if (m->tab == MAINUI_TAB_GAMES || m->tab == MAINUI_TAB_APPS || m->tab == MAINUI_TAB_PORTS || view_setting(m, w) >= 0)
            HINT("START", LAYOUT);
        break;
    }
    case MAINUI_WIN_ROMS:
        entries = count(m, MAINUI_LIST_ROMS, w->system);
        if (entries > 0) {
            HINT(mainui_model_view(m, w) == MAINUI_VIEW_CAROUSEL ? "←/→" : "DPAD", MOVE);
            if (entry_flags(m, MAINUI_LIST_ROMS, w->system, w->focus) & MAINUI_ENTRY_FOLDER) HINT(confirm, OPEN);
            else HINT(confirm, LAUNCH);
        }
        HINT(back, BACK);
        HINT("MENU", ACTIONS);
        if (entries > 1) { HINT("L1/R1", PAGE); HINT("L2/R2", GROUP); }
        if (entries > 0 && count(m, MAINUI_LIST_LAUNCHERS, w->system) > 0) HINT("X", EMULATOR);
        if (view_setting(m, w) >= 0) HINT("START", LAYOUT);
        break;
    case MAINUI_WIN_SEARCH:
        entries = count(m, MAINUI_LIST_SEARCH, 0);
        if (entries > 0) { HINT("↑/↓", MOVE); HINT(confirm, LAUNCH); }
        HINT(back, KEYBOARD);
        if (entries > 1) HINT("L1/R1", PAGE);
        if (entries > 0) HINT("MENU", ACTIONS);
        break;
    case MAINUI_WIN_FILES:
        entries = count(m, MAINUI_LIST_FILES, w->system);
        if (entries > 0) { HINT("↑/↓", MOVE); HINT(confirm, OPEN); }
        HINT(back, BACK);
        break;
    case MAINUI_WIN_EDIT:
        entries = count(m, MAINUI_LIST_EDIT, w->system);
        if (entries > 0) { HINT("DPAD", MOVE); HINT(confirm, TOGGLE); }
        HINT(back, DONE); // asks whether to save the edited visibility
        break;
    case MAINUI_WIN_SETTINGS: {
        if (w->screen == MAINUI_SCREEN_IO_TEST) { HINT("MENU+START", EXIT); break; }
        int capacity = mainui_model_screen_item_capacity(m, w->screen);
        struct mainui_item fixed_items[SCREEN_ITEMS_MAX];
        struct mainui_item *items = capacity > SCREEN_ITEMS_MAX ? calloc((size_t)capacity, sizeof(*items)) : fixed_items;
        if (items) {
            entries = mainui_model_screen_items(m, w->screen, items, capacity);
            int focus = w->focus < entries ? w->focus : entries - 1;
            if (entries > 1) HINT("↑/↓", MOVE);
            if (focus >= 0) {
                const struct mainui_item *item = &items[focus];
                switch (item->kind) {
                case MAINUI_ITEM_CYCLE:
                    // Button Mapping's rows are read-only while its profile is invalid.
                    if (!(m->mapping_locked && mainui_model_mapping_setting(item->setting))) HINT("←/→", CHANGE);
                    break;
                case MAINUI_ITEM_RANGE: case MAINUI_ITEM_DATE:
                    HINT("←/→", CHANGE); break;
                case MAINUI_ITEM_SWITCH: HINT(confirm, TOGGLE); break;
                case MAINUI_ITEM_SUBMENU: HINT(confirm, OPEN); break;
                case MAINUI_ITEM_ACTION: HINT(confirm, SELECT); break;
                case MAINUI_ITEM_INITIAL_VIEW_CHOICE: case MAINUI_ITEM_PRESENTATION_CHOICE:
                    HINT(confirm, APPLY); break;
                case MAINUI_ITEM_LIST:
                    if (item->action == MAINUI_LIST_WIFI) {
                        if (entry_flags(m, MAINUI_LIST_WIFI, 0, list_entry(items, focus)) & MAINUI_ENTRY_SAVED)
                            HINT(confirm, ACTIONS);
                        else HINT(confirm, CONNECT);
                    } else if (item->action == MAINUI_LIST_BLUETOOTH || item->action == MAINUI_LIST_PAIRED) {
                        if (!(entry_flags(m, item->action, 0, list_entry(items, focus)) & MAINUI_ENTRY_CONNECTED))
                            HINT(confirm, CONNECT);
                    } else if (item->action == MAINUI_LIST_TIMEZONES && w->system > 0 && list_entry(items, focus) == 0)
                        HINT(confirm, BACK);
                    else if (item->action == MAINUI_LIST_TIMEZONES &&
                             (entry_flags(m, MAINUI_LIST_TIMEZONES, w->system, list_entry(items, focus)) & MAINUI_ENTRY_FOLDER))
                        HINT(confirm, OPEN);
                    else HINT(confirm, APPLY);
                    if (item->action == MAINUI_LIST_PAIRED) HINT("MENU", ACTIONS);
                    break;
                case MAINUI_ITEM_EXTENSION: {
                    struct mainui_extension_info info;
                    if (extension_info(m, item->action, &info) < 0 || !info.usable) break;
                    switch (info.kind) {
                    case MAINUI_EXTENSION_MENU: HINT(confirm, OPEN); break;
                    case MAINUI_EXTENSION_CMD: HINT(confirm, SELECT); break;
                    case MAINUI_EXTENSION_TOGGLE: HINT(confirm, TOGGLE); break;
                    case MAINUI_EXTENSION_SELECT: case MAINUI_EXTENSION_RANGE: HINT("←/→", CHANGE); break;
                    case MAINUI_EXTENSION_INFO: break;
                    }
                    break;
                }
                case MAINUI_ITEM_INFO: case MAINUI_ITEM_HEADER: break;
                }
            }
            if (items != fixed_items) free(items);
        }
        HINT(back, BACK);
        break;
    }
    case MAINUI_WIN_POPUP:
        if (w->item_count > 1) HINT("↑/↓", CHOOSE);
        if (w->item_count > 0) HINT(confirm, SELECT);
        HINT(back, CANCEL);
        break;
    case MAINUI_WIN_DIALOG:
        HINT("←/→", CHOOSE);
        if (w->focus == MAINUI_DIALOG_CANCEL) HINT(confirm, CANCEL);
        else if (w->dialog == MAINUI_DIALOG_POWER_OFF && w->focus == MAINUI_DIALOG_REBOOT) HINT(confirm, REBOOT);
        else if (w->dialog == MAINUI_DIALOG_POWER_OFF) HINT(confirm, POWER_OFF);
        else if (w->dialog == MAINUI_DIALOG_UPGRADE) HINT(confirm, UPDATE);
        else if (w->dialog == MAINUI_DIALOG_SAVE_EDIT) HINT(confirm, SAVE);
        else HINT(confirm, CONFIRM);
        HINT(back, CANCEL);
        break;
    case MAINUI_WIN_NOTICE:
        HINT("A/B", CLOSE);
        break;
    case MAINUI_WIN_PROGRESS:
        if (w->progress == MAINUI_PROGRESS_EXTENSION) HINT(back, CANCEL);
        break;
    case MAINUI_WIN_TEXT:
        if (w->item_count > MAINUI_TEXT_ROWS) HINT("↑/↓", MOVE);
        HINT(back, CLOSE);
        break;
    case MAINUI_WIN_KEYBOARD: {
        size_t length = strlen(w->text);
        HINT("DPAD", MOVE);
        if (w->focus == 40) HINT(confirm, SHIFT);
        else if (w->focus == 41) HINT(confirm, SYMBOLS);
        else if (w->focus == 42 && length + 1 < sizeof(w->text)) HINT(confirm, SPACE);
        else if (w->focus == 43 && length) HINT(confirm, DELETE);
        else if (w->focus == 44) {
            if (w->purpose == MAINUI_TYPE_SEARCH) HINT(confirm, SEARCH);
            else HINT(confirm, DONE);
        }
        else if (w->focus < 40 && length + 1 < sizeof(w->text)) HINT(confirm, TYPE);
        HINT(back, CANCEL);
        HINT("L1", SHIFT); HINT("R1", SYMBOLS);
        if (length + 1 < sizeof(w->text)) HINT("X", SPACE);
        if (length) HINT("Y", DELETE);
        if (w->purpose == MAINUI_TYPE_SEARCH) HINT("START", SEARCH);
        else HINT("START", DONE);
        break;
    }
    }
#undef HINT
    return n;
}

// ---- Dispatch ------------------------------------------------------------------------------

int mainui_model_open_files(struct mainui_model *m, int title) {
    struct mainui_window *w = push(m, MAINUI_WIN_FILES);
    if (!w) return -1;
    w->system = 0;
    w->index = title;
    return 0;
}

static void files_key(struct mainui_model *m, enum mainui_key key, int confirm, int back) {
    struct mainui_window *w = top(m);
    int n = count(m, MAINUI_LIST_FILES, w->system);
    if (back) {
        if (w->system > 0) {
            struct mainui_effect *e = effect(m, MAINUI_EFFECT_FILE_BACK);
            e->system = w->system;
        } else pop(m);
        return;
    }
    if (n <= 0) return;
    if (w->focus < 0 || w->focus >= n) w->focus = 0;
    if (confirm) {
        struct mainui_effect *e = effect(m, MAINUI_EFFECT_FILE_SELECT);
        e->system = w->system;
        e->index = w->focus;
        return;
    }
    // FileBrowserMenu handles only up/down, unlike DBCachedTextMenu's paging.
    if (key == MAINUI_KEY_UP || key == MAINUI_KEY_DOWN)
        list_move(&w->focus, &w->first, n, MAINUI_ROM_LIST_ROWS, key);
}

enum mainui_key mainui_model_timed_key(struct mainui_model *m, enum mainui_key key, uint64_t now) {
    struct mainui_window *w = top(m);
    int list = m->presentation == MAINUI_PRESENTATION_STOCK &&
        (w->kind == MAINUI_WIN_ROMS || w->kind == MAINUI_WIN_SEARCH ||
        (w->kind == MAINUI_WIN_TABS && !m->bar_focused &&
         (m->tab == MAINUI_TAB_RECENT || m->tab == MAINUI_TAB_FAVORITES)));
    if (!list || key > MAINUI_KEY_RIGHT) return key;
    // ListMenu clears its direction history on a gap strictly greater than
    // 199 ms. The first action after that gap is not appended to the history.
    if (now < w->nav_ms || now - w->nav_ms > 199) {
        w->nav_ms = now;
        w->nav_count = 0;
    } else if (!w->nav_count || w->nav_key == key) {
        w->nav_ms = now;
        w->nav_key = key;
        if (w->nav_count < 11) ++w->nav_count;
    }
    if (w->nav_count <= 10 || key > MAINUI_KEY_DOWN) return key;
    if (w->kind == MAINUI_WIN_SEARCH)
        return key == MAINUI_KEY_UP ? MAINUI_KEY_L1 : MAINUI_KEY_R1;
    return key == MAINUI_KEY_UP ? MAINUI_KEY_LEFT : MAINUI_KEY_RIGHT;
}

int mainui_model_key(struct mainui_model *m, enum mainui_key key) {
    if ((unsigned)key >= MAINUI_KEY_COUNT) return 0;
    struct mainui_model before = *m;
    // With the B confirmation setting, A and B swap meanings (g04).
    int b_confirms = m->settings[MAINUI_SET_BCONFIRM] == 1;
    int confirm = key == (b_confirms ? MAINUI_KEY_B : MAINUI_KEY_A);
    int back = key == (b_confirms ? MAINUI_KEY_A : MAINUI_KEY_B);
    struct mainui_window *w = top(m);
    switch (w->kind) {
    case MAINUI_WIN_HOME: home_key(m, key, confirm, back); break;
    case MAINUI_WIN_TABS: tabs_key(m, key, confirm, back); break;
    case MAINUI_WIN_ROMS: roms_key(m, key, confirm, back); break;
    case MAINUI_WIN_FILES: files_key(m, key, confirm, back); break;
    case MAINUI_WIN_POPUP:
        // Popup rows wrap at both ends and a popup opens on its first row (n04).
        if (back) pop(m);
        else if (confirm) popup_choose(m);
        else if (w->item_count && key == MAINUI_KEY_UP) w->focus = (w->focus + w->item_count - 1) % w->item_count;
        else if (w->item_count && key == MAINUI_KEY_DOWN) w->focus = (w->focus + 1) % w->item_count;
        break;
    case MAINUI_WIN_DIALOG:
        // Left focuses Cancel and right OK (n04); B cancels. The power dialog steps along Power
        // off, Reboot and Cancel instead, stopping at either end.
        if (back) { w->focus = MAINUI_DIALOG_CANCEL; dialog_confirm(m); }
        else if (confirm) dialog_confirm(m);
        else if (w->dialog == MAINUI_DIALOG_POWER_OFF && (key == MAINUI_KEY_LEFT || key == MAINUI_KEY_RIGHT)) {
            static const int order[3] = {MAINUI_DIALOG_OK, MAINUI_DIALOG_REBOOT, MAINUI_DIALOG_CANCEL};
            int at = w->focus == MAINUI_DIALOG_REBOOT ? 1 : w->focus == MAINUI_DIALOG_CANCEL ? 2 : 0;
            at += key == MAINUI_KEY_RIGHT ? 1 : -1;
            if (at >= 0 && at <= 2) w->focus = order[at];
        }
        else if (key == MAINUI_KEY_LEFT) w->focus = MAINUI_DIALOG_CANCEL;
        else if (key == MAINUI_KEY_RIGHT) w->focus = MAINUI_DIALOG_OK;
        break;
    case MAINUI_WIN_SETTINGS: screen_key(m, key, confirm, back); break;
    case MAINUI_WIN_KEYBOARD: keyboard_key(m, key, confirm, back); break;
    case MAINUI_WIN_SEARCH: search_key(m, key, confirm, back); break;
    case MAINUI_WIN_EDIT: edit_key(m, key, confirm, back); break;
    case MAINUI_WIN_PROGRESS:
        // Keys wait for the end, except B on a settings extension's command, which cancels it.
        if (w->progress == MAINUI_PROGRESS_EXTENSION && back) {
            effect(m, MAINUI_EFFECT_EXTENSION_CANCEL)->index = w->index;
            pop(m);
        }
        break;
    case MAINUI_WIN_TEXT:
        if (back) {
            // Closing the live view of a running command cancels it.
            effect(m, MAINUI_EFFECT_EXTENSION_CANCEL)->index = w->index;
            pop(m);
        } else if (key == MAINUI_KEY_UP || key == MAINUI_KEY_DOWN) {
            w->first += key == MAINUI_KEY_UP ? -1 : 1;
            clamp_text(w);
        } else if (key == MAINUI_KEY_L1 || key == MAINUI_KEY_R1) {
            w->first += key == MAINUI_KEY_L1 ? -MAINUI_TEXT_ROWS : MAINUI_TEXT_ROWS;
            clamp_text(w);
        }
        break;
    case MAINUI_WIN_NOTICE:
        // A or B closes it before its 5 s (static: NotifyWindow closes on either button or on
        // its timer).
        if (confirm || back) pop(m);
        break;
    }
    return memcmp(&before, m, sizeof(before)) != 0;
}

int mainui_model_event(struct mainui_model *m, enum mainui_event event) {
    struct mainui_model before = *m;
    const struct mainui_window *w = top(m);
    switch (event) {
    case MAINUI_EVENT_POWER_HOLD: {
        // The stock sPoweroffdialogshow guard remains set through the POWER
        // dialog and progress. The IO test reads POWER itself.
        int shown = w->kind == MAINUI_WIN_DIALOG && w->dialog == MAINUI_DIALOG_POWER_OFF;
        for (int i = 0; i < m->depth; ++i)
            shown |= (m->stack[i].index == 1 && m->stack[i].kind == MAINUI_WIN_DIALOG &&
                      m->stack[i].dialog == MAINUI_DIALOG_POWER_OFF) ||
                     (m->stack[i].kind == MAINUI_WIN_PROGRESS && (m->stack[i].progress == MAINUI_PROGRESS_POWER_OFF ||
                                                                  m->stack[i].progress == MAINUI_PROGRESS_REBOOT));
        if (!shown && !(w->kind == MAINUI_WIN_SETTINGS && w->screen == MAINUI_SCREEN_IO_TEST))
            open_dialog(m, MAINUI_DIALOG_POWER_OFF, -1, -1, 1);
        break;
    }
    case MAINUI_EVENT_UPGRADE_FOUND:
        if (!(w->kind == MAINUI_WIN_DIALOG && w->dialog == MAINUI_DIALOG_UPGRADE))
            open_dialog(m, MAINUI_DIALOG_UPGRADE, -1, -1, -1);
        break;
    case MAINUI_EVENT_PROGRESS_DONE:
        if (w->kind == MAINUI_WIN_PROGRESS) pop(m);
        break;
    case MAINUI_EVENT_NOTICE_DONE:
        if (w->kind == MAINUI_WIN_NOTICE) pop(m);
        break;
    case MAINUI_EVENT_POWER_REQUEST: {
        int shown = 0;
        for (int i = 0; i < m->depth; ++i)
            shown |= m->stack[i].kind == MAINUI_WIN_PROGRESS && (m->stack[i].progress == MAINUI_PROGRESS_POWER_OFF ||
                                                                 m->stack[i].progress == MAINUI_PROGRESS_REBOOT);
        if (!shown) {
            struct mainui_window *progress = push(m, MAINUI_WIN_PROGRESS);
            if (progress) {
                progress->progress = MAINUI_PROGRESS_POWER_OFF;
                progress->index = 2;
                effect(m, MAINUI_EFFECT_POWER_OFF)->value = 2;
            }
        }
        break;
    }
    }
    return memcmp(&before, m, sizeof(before)) != 0;
}

void mainui_model_notice(struct mainui_model *m, enum mainui_notice notice) {
    if (notice < 0 || notice >= MAINUI_NOTICE_COUNT) return;
    struct mainui_window *w = push(m, MAINUI_WIN_NOTICE);
    if (w) w->notice = notice;
}
