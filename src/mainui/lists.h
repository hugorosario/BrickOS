#ifndef BRICKOS_MAINUI_LISTS_H
#define BRICKOS_MAINUI_LISTS_H

#include <stddef.h>

// The official recent and favorite lists, /mnt/SDCARD/Roms/recentlist.json and
// favourite2.json: one cJSON object per line. Lines the frontend does not change are written
// back byte for byte, including malformed lines and the stock writer's quirks (app entries
// holding raw memory in rompath, favorite lines repeating "order").
//
// Measured on the official MainUI (task 1.6 and the navigation sessions):
// - A launch puts the entry on top of recentlist.json, removing the same game, and keeps 20
//   lines. Launching the entry already on top leaves the file byte-identical.
// - The Recent tab shows each label once (the first line with it).
// - Add Favorite appends a line with "sublabel" and "order" one above the entry count, and the
//   Favorites tab lists entries by order, highest first. An app's or a port's line has label,
//   launch, type, and its description as sublabel, and no order: such lines follow the ordered
//   ones, the later line first (p7, p8).

#define MAINUI_RECENT_LINES 20

// A port's entry has the fields of an app's: label, launch, and its type (p3).
enum mainui_entry_type { MAINUI_ENTRY_APP = 3, MAINUI_ENTRY_GAME = 23, MAINUI_ENTRY_PORT = 47 };
#define MAINUI_ENTRY_SCRIPT(type) ((type) == MAINUI_ENTRY_APP || (type) == MAINUI_ENTRY_PORT)

struct mainui_list_entry {
    char *line;             // the line as read or written, without its newline
    int valid;              // parsed as an object with a label
    char label[256];
    char rompath[1024];
    size_t rompath_length;  // app entries may hold NUL bytes here
    char imgpath[1024];
    char launch[1024];
    char sublabel[64];
    int type;
    int order, has_order;   // the last "order" member
};

struct mainui_entries {
    struct mainui_list_entry *entries;
    size_t count, capacity;
};

// Splits text into lines and parses each. Returns 0, or -1 when out of memory.
int mainui_list_parse(struct mainui_entries *list, const char *text, size_t length);
void mainui_list_free(struct mainui_entries *list);
// The list as file text, one line per entry, each ending in a newline. The caller frees it.
char *mainui_list_format(const struct mainui_entries *list, size_t *length);
// Reads and writes a list file; the write goes to PATH.tmp, then replaces PATH.
int mainui_list_load(struct mainui_entries *list, const char *path);
int mainui_list_save(const struct mainui_entries *list, const char *path);

// Fills an entry from its fields, as a new launch or favorite has them.
void mainui_list_entry_set(struct mainui_list_entry *entry, const char *label, const char *rompath,
                           const char *imgpath, const char *launch, const char *sublabel, int type);

// The entries the Recent tab shows, as indexes into the list; returns their count.
size_t mainui_recent_visible(const struct mainui_entries *list, size_t *indexes, size_t max);
// Records a launch. A game replaces its earlier line (same rompath); an app line is added each
// time, as the stock writer's app lines never compare equal. Returns 0 or -1.
int mainui_recent_add(struct mainui_entries *list, const struct mainui_list_entry *entry);

// The entries the Favorites tab shows, in its order; returns their count.
size_t mainui_favorites_visible(const struct mainui_entries *list, size_t *indexes, size_t max);
// The index of the line for the same game (rompath) or app (launch), or -1.
int mainui_favorites_find(const struct mainui_entries *list, const struct mainui_list_entry *entry);
// Appends a favorite with order = entry count + 1. Returns 0, 1 when it was already a
// favorite, or -1.
int mainui_favorites_add(struct mainui_entries *list, const struct mainui_list_entry *entry);

int mainui_list_remove(struct mainui_entries *list, size_t index);
void mainui_list_clear(struct mainui_entries *list);

// The system lists' positions, /mnt/romwinidx.json (static: MenuWindow::quit records a list's
// focused row, first row, and last row under the system's rompath; runWindows writes the file in
// cJSON's formatted print as MainUI exits, main reads it at start, and MenuWindow::init opens the
// list there). Entries for systems the frontend does not show are kept.
#define MAINUI_POSITIONS_FILE "/mnt/romwinidx.json"
#define MAINUI_POSITIONS_MAX 1024 // includes every ancestor of a full-length ROM path

struct mainui_position {
    char rompath[1024];  // system root or nested stock ppath, unresolved
    int pos, start, end;
};

struct mainui_positions {
    struct mainui_position entries[MAINUI_POSITIONS_MAX];
    int count;
};

// Reads the file's "list" array; entries without a rompath and the three numbers are left out.
// Returns 0, or -1 when the text is not such an object.
int mainui_positions_parse(struct mainui_positions *positions, const char *text, size_t length);
const struct mainui_position *mainui_positions_find(const struct mainui_positions *positions, const char *rompath);
// Records a list's position in every entry for rompath, or in a new entry at the end, as
// add2SubWinIdx does. Returns 1 when the table changed, 0 when it held the values, -1 when full.
int mainui_positions_set(struct mainui_positions *positions, const char *rompath, int pos, int start, int end);
// The file text as the official writes it, without a final newline. Returns its length, or -1
// when out is too small.
int mainui_positions_format(const struct mainui_positions *positions, char *out, size_t size);

#endif
