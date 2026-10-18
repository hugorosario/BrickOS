#ifndef BRICKOS_MAINUI_CATALOG_H
#define BRICKOS_MAINUI_CATALOG_H

#include <stddef.h>
#include "names.h"

// The SD catalog the official MainUI presents. Measured on the reference device (tasks 1.6 and
// 3.1):
// - Systems are the folders of /mnt/SDCARD/Emus with a config.json. The Games grid shows those
//   Emus/show.json marks shown, sorted by label in byte order; the Edit screen shows all.
// - A system's games are the files of its rompath whose extension is in extlist (every file
//   without an extlist), leaving out names starting with a dot and the list caches
//   (<TAG>_cache<N>.db). Folders come first. Names sort by stock pronunciation initials
//   ignoring ASCII case, then the original database label in byte order. Named loads use
//   the installed short-name dictionary and the active language. A configured gamelist
//   supplies stock XML rows instead when it contains usable entries.
// - Apps are the folders with a config.json in /usr/trimui/apps, then in /mnt/SDCARD/Apps, in
//   directory order (readdir), each root using its own show.json visibility.
// - Ports are read the same way from /mnt/SDCARD/Ports, then /mnt/exUDISK/Ports, each with its
//   own show.json (static: loadEmusApps; p1: the folders' order on the card). Their config.json
//   "launch" is the full path of a script, written for the shell (PortMaster escapes spaces).

#define MAINUI_LAUNCHERS_MAX 8
// Shared storage for a theme value from catalog through settings, audio and rendering.
#define MAINUI_THEME_PATH_BYTES 512
#define MAINUI_STOCK_APPS_SHOW_FILE "/mnt/SDCARD/Apps/brickos-stock-show.json"

// What a home tab draws behind the focused item (static: EmuItem and AppItem +0xe0..+0x120):
// the background picture and the effect script as full paths, and the LED colour as six hex
// digits; each is empty when the config has none.
struct mainui_backdrop {
    char background[128];
    char themecolor[7];
    char effectsh[128];
};

struct mainui_system {
    char folder[64];             // the folder under Emus
    char label[128];
    char directory[512];         // /mnt/SDCARD/Emus/<folder>
    char launch[512];            // directory/launch, as the recent list records it
    char rompath[512];           // directory/rompath, unresolved (e.g. .../Emus/FC/../../Roms/FC)
    char imgpath[512];
    char gamelist[512];          // optional config.json gamelist, relative to directory
    char extlist[256];
    char icontop[256], icon[256];
    struct mainui_backdrop backdrop;
    int icon_only;               // icontop holds the "icon" picture: the system has no icontop
    int useswap, shortname, hidebios;
    int show;                    // from show.json; 1 when absent
    int launcher_count;
    char launcher_names[MAINUI_LAUNCHERS_MAX][64];
    char launchers[MAINUI_LAUNCHERS_MAX][512];  // full paths
};

struct mainui_app {
    char label[128];
    char directory[512];
    char launch[512];            // config.json "launch", relative to the directory (a port's: a full path)
    char description[256];       // config.json "description", a favorite's sublabel (p7)
    char icon[256];              // icontop, else icon, relative to the directory
    char icon_layer[256];      // separate icon layer when icontop is also configured
    int icon_only;               // icon holds the "icon" picture: the app has no icontop
    int show;
    int stock;                   // from /usr/trimui/apps
    struct mainui_backdrop backdrop;
};

struct mainui_game {
    char name[256];              // the file or folder name
    char label[256];             // original basename or XML name: stock disp column, history
    int folder;
    char pinyin[256];            // one stock-compatible initial per UTF-8 codepoint
    char opinyin[256];           // original filename initials, also used by search
    char display[256];           // translated short name, or empty for label fallback
    char path[1024], image[1024]; // explicit XML paths; empty for filesystem rows
};

// Ordinary FileBrowser rows retain the complete filename, unlike ROM display labels.
struct mainui_file {
    char name[256];
    int folder;
};
struct mainui_fs;

// Stock getFileType: built-in ROM suffix prefixes, case insensitive; -1 otherwise.
int mainui_catalog_file_type(const char *name);
// First configured system whose extlist prefix accepts the filename, or -1. Empty
// extlists do not match here (unlike an unrestricted system's own ROM list).
int mainui_catalog_file_system(const struct mainui_system *systems, int count, const char *name);
int mainui_catalog_file_allowed(const struct mainui_system *systems, int count,
                               const char *name, const char *path);
// Up to 20,000 stock-filtered regular files/directories, folders before files and
// byte-name order within each group. Hidden folders are omitted; include_parent
// enables the stock two-dot prefix entries for entered folders, not the initial root.
// Returns the count, or -1 with *files NULL. The caller frees *files.
int mainui_catalog_files(const struct mainui_fs *fs, const struct mainui_system *systems,
                         int system_count, const char *path, int include_parent, struct mainui_file **files);

// Lists a directory's entry names in the order readdir gives them; returns the count or -1.
// Tests pass a fake to fix the order. A reader writes at most max names and
// returns the copied count, or its total required count. A full/truncated read
// is retried with more storage before any names are consumed; -1 reports failure.
typedef int (*mainui_list_dir)(void *context, const char *path, char names[][256], int max);
int mainui_readdir(void *context, const char *path, char names[][256], int max);

struct mainui_fs {
    const char *root;            // prefix of every device path ("" on the device)
    mainui_list_dir list;
    void *context;
};

// Loads every system (shown or not), sorted as the Games grid and the Edit screen show them.
// Returns the count, or -1. The caller frees *systems.
int mainui_catalog_systems(const struct mainui_fs *fs, struct mainui_system **systems);
// Bests uses SD/Best then exUDISK/Best, replacing exact duplicate rompaths and sorting
// by label in byte order. Equal labels retain source-directory order.
int mainui_catalog_bests(const struct mainui_fs *fs, struct mainui_system **systems);
// The games of a system, sorted; returns the count, or -1. The caller frees *games.
int mainui_catalog_games(const struct mainui_fs *fs, const struct mainui_system *system, struct mainui_game **games);
int mainui_catalog_games_named(const struct mainui_fs *fs, const struct mainui_system *system,
                              const struct mainui_names *names, const char *language, struct mainui_game **games);
int mainui_catalog_games_at(const struct mainui_fs *fs, const struct mainui_system *system,
                           const struct mainui_names *names, const char *language,
                           const char *directory, struct mainui_game **games);
// Visit ROMs from usable XML, otherwise the complete configured directory tree.
// Folder rows are excluded. The callback returns 0 to continue, 1 to stop, or -1
// on failure; the visitor returns that status. Each game is valid during the call.
int mainui_catalog_walk_games(const struct mainui_fs *fs, const struct mainui_system *system,
                             const struct mainui_names *names, const char *language,
                             int (*visit)(void *, const struct mainui_game *), void *context);
const char *mainui_catalog_game_label(const struct mainui_game *game);
// Adjacent nonempty initial groups of the current ROM list, with all folders forming one group.
// Returns the first row of the adjacent group, or -1 at an endpoint/invalid input.
// On success, letter receives the uppercase ASCII initial, or 0 for the folder group.
int mainui_catalog_letter_jump(const struct mainui_game *games, int count, int focus, int direction, char *letter);
// ASCII is preserved; supported CJK becomes a lowercase initial, other Unicode becomes space.
// Returns the byte count, or -1 for invalid UTF-8, invalid arguments, or insufficient capacity.
int mainui_catalog_pinyin(const char *text, char *out, size_t capacity);
// Every app, in the Apps tab order (hidden ones included, with show 0). The caller frees *apps.
int mainui_catalog_apps(const struct mainui_fs *fs, struct mainui_app **apps);
int mainui_catalog_ports(const struct mainui_fs *fs, struct mainui_app **ports);

struct mainui_theme {
    char name[128];
    char path[MAINUI_THEME_PATH_BYTES]; // the system.json "theme" value
    char font[128];              // config.json "font", relative to the theme folder
    char preview[512];           // config.json "preview", resolved against the theme folder
};

// The Themes screen: Default ("../res/") first, then each folder of /mnt/SDCARD/Themes (or of
// /mnt/UDISK/Themes, which replaces it when it exists) with a config.json, in directory order (open: sorted or directory order, which agree on the
// reference card). The caller frees *themes.
int mainui_catalog_themes(const struct mainui_fs *fs, struct mainui_theme **themes);

// The paths a game's recent and favorite entries use: explicit XML paths when supplied,
// otherwise rompath/<filename> and imgpath/<basename>.png.
void mainui_catalog_game_paths(const struct mainui_system *system, const struct mainui_game *game,
                               char *rompath, size_t rompath_size, char *imgpath, size_t imgpath_size);
// 1 when the cover exists under fs->root.
int mainui_catalog_has_cover(const struct mainui_fs *fs, const char *imgpath);

// Refresh ROMs and confirmed Factory Reset remove stock <name>_cache<digits>.db entries from
// loaded ROM directories. No recursion or directory removal; links are unlinked
// without following their targets. Missing media is harmless, paths outside the
// supported card roots are refused. Returns the number removed, or -1 on error.
int mainui_catalog_clear_caches(const struct mainui_fs *fs, const char *rompath);

// Refresh ROMs also removes ARCADE/.mame4all/configs/*.cfg, FC/.fceux/fceux.cfg
// and FC/.fceux/cfg/*.cfg. Same card containment, nonrecursive and link rules.
int mainui_catalog_clear_keymaps(const struct mainui_fs *fs);

#endif
