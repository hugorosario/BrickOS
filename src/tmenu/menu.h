#ifndef BRICKOS_TMENU_MENU_H
#define BRICKOS_TMENU_MENU_H

#include <stdint.h>

// The menu's state engine: the stock library's three screens, their key handling,
// and the save and load actions (docs/tmenu-abi.md, "Handlers"). It performs no I/O
// itself; the host and the renderer are reached through tmenu_ops.

#define TMENU_ITEMS 8
#define TMENU_MAX_SLOTS 100
#define TMENU_ROWS 7
#define TMENU_NOTIFY_BYTES 32
#define TMENU_NOTIFY_MS 2000

enum tmenu_item {
    TMENU_ITEM_CONTINUE, TMENU_ITEM_SAVE, TMENU_ITEM_LOAD, TMENU_ITEM_VOLUME,
    TMENU_ITEM_ADVANCED, TMENU_ITEM_NETPLAY, TMENU_ITEM_FULLSCREEN, TMENU_ITEM_EXIT,
};

enum tmenu_key {
    TMENU_KEY_NONE = -1, TMENU_KEY_CONFIRM = 1, TMENU_KEY_BACK = 2,
    TMENU_KEY_UP = 5, TMENU_KEY_DOWN = 6, TMENU_KEY_LEFT = 7, TMENU_KEY_RIGHT = 8,
};

enum tmenu_screen { TMENU_SCREEN_MAIN, TMENU_SCREEN_SAVE, TMENU_SCREEN_LOAD };

// Handler results, as the official library's: 0 keep waiting, 4 redraw, 1/2/3 return.
enum tmenu_result {
    TMENU_RESULT_NONE = 0, TMENU_RESULT_CLOSE = 1, TMENU_RESULT_ADVANCED = 2,
    TMENU_RESULT_NETPLAY = 3, TMENU_RESULT_REDRAW = 4,
};

struct tmenu_ops {
    void *context;
    int (*saved_slots)(void *context, int max, int *slots);
    int (*save_state)(void *context, int slot);
    int (*load_state)(void *context, int slot);
    void (*quit)(void *context);
    void (*fullscreen)(void *context);  // NULL when the host has none
    int (*keyshm_get)(void *context, int index);
    void (*keyshm_set)(void *context, int index, int value);
    void (*set_volume)(void *context, int volume);
    void (*sleep_us)(void *context, unsigned microseconds);
    // Writes the thumbnail of the paused frame for a slot, from the host's path.
    void (*save_thumbnail)(void *context, int slot);
    void (*sync)(void *context);
    uint64_t (*now_ms)(void *context);
};

struct tmenu_strings {
    const char *items[TMENU_ITEMS];
    const char *record;       // "Record %d/%d"
    const char *load_fail;
    const char *save_fail;
    const char *slot_saved;   // "slot%d saved"
    const char *on, *off;
};

struct tmenu_menu {
    enum tmenu_screen screen;
    int close_after_save;   // asynchronous host must resume its frame loop
    int focus;
    int first;                // first main-screen item drawn
    int rows;                 // stock 7, BrickOS 6; does not change actions or slot behavior
    int save_index;           // slot 0..slot_count-1
    int load_index;           // index into saved[]
    int visible[TMENU_ITEMS];
    int fullscreen_on;
    int slot_count;
    int saved[TMENU_MAX_SLOTS];
    int saved_count;
    unsigned char slot_saved[TMENU_MAX_SLOTS];
    char notify[TMENU_NOTIFY_BYTES + 1];
    uint64_t notify_ms;
    const struct tmenu_strings *strings;
    struct tmenu_ops ops;
};

// Lists the saved slots and sets the visible items. has_fullscreen is 1 when the
// host table has both Fullscreen entries (0x28 and 0x30), and fullscreen_on the
// initial state from 0x30.
void tmenu_menu_open(struct tmenu_menu *menu, const struct tmenu_ops *ops, const struct tmenu_strings *strings,
                     int slot_count, int has_fullscreen, int fullscreen_on);
enum tmenu_result tmenu_menu_key(struct tmenu_menu *menu, enum tmenu_key key);

// Copies the notification to draw on this frame into text and returns 1, or returns 0.
// Once its 2 s are over it is still drawn on this frame and then cleared, as the
// official loop does.
int tmenu_menu_notification(struct tmenu_menu *menu, char text[TMENU_NOTIFY_BYTES + 1]);

// The result ShowMenu returns for a handler result that ends the menu.
int tmenu_menu_return_value(enum tmenu_result result);

#endif
