#ifndef BRICKOS_MAINUI_EXTENSIONS_H
#define BRICKOS_MAINUI_EXTENSIONS_H

#include <stddef.h>
#include <stdio.h>
#include <sys/types.h>

#include "json_tree.h"

// Settings extensions: Settings sections and rows that users and apps add with a JSON file and
// shell scripts, in the format of BrickTools' menu.json and EmulationStation's custom menus
// (docs/settings-extensions.md). This registry finds the files, keeps one flat node table built
// from them, translates their texts with each folder's lang/<code>.json, and applies the tags
// scripts print. It runs nothing; extension_runner.c does. The model reaches it through
// struct mainui_extensions callbacks that the frontend provides.

#define MAINUI_EXT_SETTINGS_DIR "/mnt/SDCARD/BrickOS/settings"
#define MAINUI_EXT_APPS_DIR "/mnt/SDCARD/Apps"
#define MAINUI_EXT_LOG "/tmp/brickos-settings-extensions.log"
#define MAINUI_EXT_LOG_MAX (64 * 1024)
#define MAINUI_EXT_MAX_ITEMS 512   // per extension, chained files and extend rows included
#define MAINUI_EXT_MAX_DEPTH 8     // menu levels
#define MAINUI_EXT_MAX_TILES 64
#define MAINUI_EXT_PATH 1024
#define MAINUI_EXT_TEXT 512        // a translated text with its placeholders filled

enum mainui_ext_kind {
    MAINUI_EXT_MENU, MAINUI_EXT_CMD, MAINUI_EXT_TOGGLE, MAINUI_EXT_SELECT, MAINUI_EXT_RANGE, MAINUI_EXT_INFO,
};

enum mainui_ext_output { MAINUI_EXT_OUTPUT_NONE, MAINUI_EXT_OUTPUT_LINE, MAINUI_EXT_OUTPUT_FULL };

enum mainui_ext_anchor { MAINUI_EXT_ANCHOR_END, MAINUI_EXT_ANCHOR_AFTER, MAINUI_EXT_ANCHOR_BEFORE };

struct mainui_ext_node {
    enum mainui_ext_kind kind;
    int extension;               // the extension that defines it
    int folder;                  // the folder of the file that defines it: scripts run there
    int parent, first_child, next_sibling, child_count;  // -1 for none
    int depth;                   // 1 for the rows of a section or of an extend entry
    char path[64];               // index path in the extension's menu ("2", "1.0"); "" for extend rows
    const char *id;              // authored id, or NULL
    const char *label, *description, *icon, *execute, *load, *value_text;  // NULL when absent
    enum mainui_ext_output output;
    const char **items;          // SELECT
    int item_count;
    int min, max, step;          // RANGE
    int value, initial;          // TOGGLE 0/1, SELECT index, RANGE number; initial is the file's
    int disabled;                // from the file or a tag
    // The frontend's state of the row: busy while a script for it runs (job), the value the
    // last load or accepted execute left (applied), and a select or range change waiting to
    // settle (pending, since the tick that first saw it).
    int busy, job, applied, pending;
    unsigned long long pending_since;
    // Extend rows: the built-in section (enum mainui_screen) and where in it.
    int section;                 // -1 for the rows of an extension's own menu
    enum mainui_ext_anchor anchor;
    const char *anchor_id;
};

struct mainui_ext_placeholder {
    char *key, *value;
};

struct mainui_ext_extension {
    char name[256];              // folder name: <name>/<name>.json, or the app's folder
    char dir[MAINUI_EXT_PATH];
    int app;                     // from Apps/<app>/settings.json
    char fallback_title[256];    // the folder name, or the app's label
    const char *title;           // authored, or NULL
    char icon[MAINUI_EXT_PATH];  // the tile's icon file, or "" for the generic one
    int root;                    // the MENU node of its tile, or -1 when it only extends
    int folder;                  // its top folder
    int first_node, node_count;
    struct mainui_ext_placeholder *placeholders;
    int placeholder_count;
};

struct mainui_ext_catalog_entry {
    const char *key, *value;
    int order;
};

struct mainui_ext_folder {
    char dir[MAINUI_EXT_PATH];
    int extension;
    int loaded;                  // the catalog for the current language was looked for
    struct mainui_json_tree tree;
    struct mainui_ext_catalog_entry *entries;
    int entry_count;
};

struct mainui_ext_file {
    char path[MAINUI_EXT_PATH];
    int exists;
    dev_t dev;
    ino_t ino;
    off_t size;
    long long mtime_ns;
};

struct mainui_ext_registry {
    char root[MAINUI_EXT_PATH];  // prefix of every device path ("" on the device)
    char language[32];           // the code: system.json's language without .lang
    unsigned generation;         // increases each time the table is rebuilt
    int built;
    struct mainui_ext_node *nodes;
    int node_count, node_capacity;
    struct mainui_ext_extension *extensions;
    int extension_count, extension_capacity;
    struct mainui_ext_folder *folders;
    int folder_count, folder_capacity;
    struct mainui_ext_file *files;   // the found extension files, then the chained files read
    int file_count, file_capacity, main_count;
    struct mainui_json_tree *trees;
    int tree_count, tree_capacity;
    int tiles[MAINUI_EXT_MAX_TILES]; // extension indices, in tile order
    int tile_count;
    int dry;                         // a dry run writes no log: its lines go to record, if any
    FILE *record;
};

void mainui_ext_init(struct mainui_ext_registry *r, const char *root);
void mainui_ext_free(struct mainui_ext_registry *r);
// Finds the extension files and rebuilds the table when any was added, removed or changed (by
// device, inode, size or modification time); catalogs are always looked for again. Returns 1
// when the table was rebuilt (node numbers changed), 0 when it was kept.
int mainui_ext_scan(struct mainui_ext_registry *r);

const struct mainui_ext_node *mainui_ext_node(const struct mainui_ext_registry *r, int node);
int mainui_ext_tile_count(const struct mainui_ext_registry *r);
int mainui_ext_tile_node(const struct mainui_ext_registry *r, int tile);
int mainui_ext_tile_extension(const struct mainui_ext_registry *r, int tile);
// The node at an index path ("2", "1.0") or with an id, inside an extension; -1 if none.
int mainui_ext_find(const struct mainui_ext_registry *r, int extension, const char *path_or_id);
// The folder a node's scripts run in.
const char *mainui_ext_node_dir(const struct mainui_ext_registry *r, int node);

// A value a script reported: shown and applied (clamped to the row).
void mainui_ext_set_value(struct mainui_ext_registry *r, int node, int value);
// A value the user chose: shown, not yet applied (clamped to the row).
void mainui_ext_show_value(struct mainui_ext_registry *r, int node, int value);
void mainui_ext_set_busy(struct mainui_ext_registry *r, int node, int busy);
void mainui_ext_set_disabled(struct mainui_ext_registry *r, int node, int disabled);

// Translation. language is system.json's value ("pt.lang"); a change drops the catalogs.
void mainui_ext_set_language(struct mainui_ext_registry *r, const char *language);
// The translation of text in the catalog of the folder, then of its extension's top folder;
// text itself when neither has one.
const char *mainui_ext_translate(struct mainui_ext_registry *r, int folder, const char *text);
// Translates text for an extension, then fills its {{name}} placeholders (empty when unset).
void mainui_ext_format(struct mainui_ext_registry *r, int extension, int folder, const char *text, char *out, size_t size);

enum mainui_ext_text { MAINUI_EXT_TEXT_LABEL, MAINUI_EXT_TEXT_DESCRIPTION, MAINUI_EXT_TEXT_VALUE, MAINUI_EXT_TEXT_ITEM };
// A node's text for display: translated and filled. ITEM is the select's current item. Returns
// 0, or -1 (out empty) when the node has no such text.
int mainui_ext_text(struct mainui_ext_registry *r, int node, enum mainui_ext_text which, char *out, size_t size);
void mainui_ext_tile_title(struct mainui_ext_registry *r, int tile, char *out, size_t size);

// Output lines of a node's scripts. Tags in the line are applied and the line is hidden;
// "confirm:" asks the user. Otherwise the line is visible, with each literal \n turned into a
// newline. visible receives the prompt or the visible text.
enum mainui_ext_line { MAINUI_EXT_LINE_HIDDEN, MAINUI_EXT_LINE_VISIBLE, MAINUI_EXT_LINE_CONFIRM };
enum mainui_ext_line mainui_ext_apply_line(struct mainui_ext_registry *r, int node, const char *line,
                                           char *visible, size_t size);

// A dry run (diagnostic sessions) writes nothing: log lines go to record as "extension log: ...".
void mainui_ext_set_dry(struct mainui_ext_registry *r, FILE *record);
// Appends a line to the extension log, truncating it with a marker line at 64 KiB.
void mainui_ext_log(struct mainui_ext_registry *r, const char *format, ...)
    __attribute__((format(printf, 2, 3)));

#endif
