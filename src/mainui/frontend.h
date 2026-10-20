#ifndef BRICKOS_MAINUI_FRONTEND_H
#define BRICKOS_MAINUI_FRONTEND_H

#include "actions.h"
#include "appearance.h"
#include "appearance_fonts.h"
#include "assets.h"
#include "boot_visuals.h"
#include "loading_screens.h"
#include "catalog.h"
#include "effects.h"
#include "extension_runner.h"
#include "extensions.h"
#include "lists.h"
#include "launch.h"
#include "model.h"
#include "network.h"
#include "settings.h"
#include "brickos_prefs.h"

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>

// The frontend without SDL: the model with the SD catalog, the recent and favorite lists,
// system.json, the launch handoff, and the effect adapters. Every model effect is carried out
// here, or only recorded in a dry run (diagnostic sessions), which writes nothing.

#define MAINUI_WIFI_MAX 32
#define MAINUI_BT_MAX 32
#define MAINUI_ZONE_LEVELS 4
#define MAINUI_BT_QUEUE 12
#define MAINUI_NETPLAY_BUILTIN_LABELS 10
#define MAINUI_POWER_WAIT_MS 60000
// How long the one-row cards slide to a new focus, and the background fades in with them.
#define MAINUI_MOTION_MS 200

// One folder of the Time Zone browser, loaded when first shown.
struct mainui_zone_folder {
    char path[128];              // relative to zoneinfo; "" for zoneinfo itself
    struct mainui_zone *zones;
    int count, loaded;
};

struct mainui_file_folder {
    char path[1024];
    struct mainui_file *files;
    int count;
};

// Device Info rows in screen order. The official Serial row is left out: its value comes from the
// official frontend's authentication chip, and nothing uses it (the user's decision).
enum mainui_info {
    MAINUI_INFO_MODEL_NAME, MAINUI_INFO_MODEL_NUMBER, MAINUI_INFO_VERSION, MAINUI_INFO_UI_VERSION,
    MAINUI_INFO_LINUX_VERSION, MAINUI_INFO_COUNT,
};

// Button Mapping's Status row: the BrickOS input daemon runs, or not, or its profile is invalid.
enum mainui_mapping_status { MAINUI_MAPPING_INACTIVE, MAINUI_MAPPING_ACTIVE, MAINUI_MAPPING_INVALID };

// What LED Policy says about the LED daemon in an LED mode: it runs, it does not, or it yields
// to BrickTools' LED daemon.
enum mainui_led_status { MAINUI_LED_RUNNING, MAINUI_LED_NOT_RUNNING, MAINUI_LED_BRICKTOOLS };

struct mainui_search_hit {
    int system;  // index into the visible systems
    int game;    // index into hit_games, independent of the currently open ROM list
};

struct mainui_frontend {
    struct mainui_fs fs;
    struct mainui_names names;       // installed dictionary; never copied into the repository
    int dry;                        // record effects instead of carrying them out
    FILE *record;                   // effect records, or NULL
    struct mainui_settings settings;
    struct mainui_model model;
    struct mainui_brickos_preferences brickos_preferences;
    char brickos_state_digest[65];
    int brickos_view_touched;
    uint64_t brickos_view_deadline_ms;
    struct mainui_system *systems;
    int system_count;
    int visible[128];               // the Games grid: indexes into systems
    int visible_count;
    struct mainui_app *apps;
    int app_count;
    int shown_apps[256];            // the Apps tab: indexes into apps
    int shown_app_count;
    int gameswitcher_off;           // Settings > GameSwitcher turned it off: no app, no card
    uint64_t gameswitcher_read_ms;  // the last read of that switch
    struct mainui_app *ports;      // /mnt/SDCARD/Ports and /mnt/exUDISK/Ports
    struct mainui_system *bests;    // /mnt/SDCARD/Best and /mnt/exUDISK/Best
    int best_count, shown_best_count, shown_bests[128];
    int best_game_counts[128];
    int port_count;
    int shown_ports[256];           // the Ports tab: indexes into ports
    int shown_port_count;
    struct mainui_game *games;      // the games of games_system (a visible index), or NULL
    int game_count, games_system;
    char games_path[1024];         // empty for the configured root, otherwise a ROM folder
    // Each system's game count for the Games grid (by system index; -1 until counted), filled by
    // mainui_frontend_count_games and by every load of a system's games.
    int *game_counts;
    int count_mode;                 // 0 none, 1 counted at once, 2 counted by a thread
    int count_stop, counts_changed; // read and written atomically
    int counter_running;
    pthread_t counter;
    struct mainui_entries recent, favorites;
    struct mainui_positions positions;  // /mnt/romwinidx.json
    int positions_changed;
    size_t recent_shown[64], favorites_shown[512];
    int recent_count, favorites_count;
    struct mainui_theme *themes;
    int theme_count;
    struct mainui_boot_visual *bootlogos;
    struct mainui_boot_visual *splashscreens;
    int bootlogo_count, splashscreen_count;
    int bootlogo_active, splashscreen_active; // catalog index, -1 custom, -2 not readable yet
    // Random Bootlogo and Splashscreen (indexed by enum mainui_boot_visual_kind). Each random
    // selector gets a new image once per boot, from a worker started after the first frame.
    // Every boot-visual read or write holds lock; the worker sets changed when it is done.
    struct {
        int random[2];
        pthread_mutex_t lock;
        int lock_ready;
        pthread_t worker;
        int worker_running, changed;
        uint32_t draws[2];                    // the worker's random values, drawn when it starts
        uint32_t (*random_value)(void *context); // tests inject it; NULL reads /dev/urandom
        void *random_context;
    } boot_visuals;
    // The idle music picture for the official OSD (docs/mainui.md). While BrickOS is active
    // without the BrickOS OSD, a worker writes the stock picture in the BrickOS palette to its
    // look's folder, and the tick names that folder in the music server's theme file.
    struct {
        // Writes source recoloured for the accent seed and background level to destination;
        // 0 on success. main binds the SDL one and tests inject theirs; NULL keeps the stock picture.
        int (*write)(const char *source, const char *destination, const unsigned char seed[3], double level);
        struct mainui_music_job *job; // owned by the worker while it runs
        pthread_t worker;
        int running, done, again;
        char named[32];               // the look whose folder the theme file names, or empty
    } music;
    struct mainui_loading_screen *loading_screens;
    int loading_screen_count;
    int loading_screen_active; // preset index, MAINUI_LOADING_SCREEN_TRIMUI, -1 custom, -2 not readable
    char languages[16][32];
    char language_icons[16][64];    // each language's flag-and-name skin image
    char language_names[16][64];    // each language file's own name ("lang"), empty when it has none
    int language_count;
    // Stock Netplay resolves built-in labels in every installed language, even when
    // this card has no Emus entry. Stock maps language IDs 3..9 and 11..13 to
    // legacy types 5..14; GPSP (ID 10) has no standalone fallback mapping.
    char netplay_builtin_labels[16][MAINUI_NETPLAY_BUILTIN_LABELS][128];
    char device_info[MAINUI_INFO_COUNT][128];
    uint32_t io_held;              // current raw hardware-test control mask
    int io_motor, io_fn1;          // held L1+R1 rumble and GPIO243 side FN indicator
    int io_shortcut_suspended;     // temporary ownership of keymon's hotkeyshow=OFF
    char hotspot_ssid[MAINUI_TEXT_MAX], hotspot_password[MAINUI_TEXT_MAX];
    struct mainui_lang lang;
    struct mainui_search_hit *hits;
    int hit_count;
    struct mainui_game *hit_games; // owned metadata/path snapshots, allocated only for matches
    int hit_capacity;
    struct mainui_effect_ops ops;   // hardware writes; unset members are skipped
    // SDL-independent BGM callback, bound by the audio loop. Normal SET effects
    // call it after SHM publication, including clamped steps and queued keys.
    void *bgm_context;
    void (*bgm_volume)(void *context, int level);
    // Applied theme sounds change before the next queued key and before hint/persistence
    // publication. Same-theme selection and dry effects invoke no audio callback.
    void *theme_context;
    void (*theme_audio)(void *context, const struct mainui_fs *fs, const char *theme);
    // Rumble preview owns a cancellable monotonic 200 ms wait, independent of
    // UI ticks. Its immutable writer snapshot remains valid until close joins.
    struct mainui_effect_ops rumble_ops;
    pthread_mutex_t rumble_mutex;
    pthread_cond_t rumble_cond;
    pthread_t rumble_thread;
    struct timespec rumble_until;
    int rumble_initialized, rumble_running, rumble_cancel, rumble_active;
    int rumble_done, rumble_failed;
    // Refresh owns immutable catalog paths until its worker is joined. Publication
    // of refresh_done uses release/acquire; results are applied on the UI thread.
    int refresh_pending, refresh_running, refresh_done, refresh_failed;
    pthread_t refresh_thread;
    struct mainui_runner runner;    // shell commands; NULL run skips them
    // Power command completion is published by the worker; the UI owns its
    // subsequent 60-second wait. Close joins before releasing this context.
    struct mainui_runner power_runner;
    int power_pending, power_running, power_done, power_failed, power_source;
    int power_reboot;               // 1 when the worker runs reboot, 0 for poweroff
    int power_wait_ms;
    uint64_t power_wait_since;
    pthread_t power_thread;
    struct mainui_encoder encoder;  // installed ARE adapter; unset means unavailable
    struct mainui_resume resume;    // the stack /tmp/state.json held at the start
    int resume_found;
    // Wi-Fi: the networks the Wi-Fi screen lists, while it is in front.
    struct mainui_wifi_network wifi[MAINUI_WIFI_MAX];
    int wifi_count;
    struct mainui_wifi_status wifi_status;
    struct mainui_netplay_host hosts[MAINUI_NETPLAY_MAX];
    int host_count, netplay_fd;
    uint64_t netplay_retry_ms;
    // One background HTTP cover at a time. The private download is copied atomically
    // into the stock SD image path when safe; existing local files always take priority.
    // Tests may enable this on a fake root and select an isolated HTTP port.
    int cover_enabled, cover_running, cover_done, cover_stop, cover_result, cover_cached;
    unsigned cover_port, cover_sequence, cover_next;
    pthread_t cover_thread;
    struct mainui_netplay_host cover_host;
    char cover_directory[128], cover_file[128];
    char cover_cache_file[1024];
    // What the screen draws behind its window (static: TabMenuWindow::draw and
    // draw_back_internal). The home tabs take the focused item's backdrop at once; a new
    // background fades in with the cards' motion, and the item's effect script, or else its
    // colour on the LEDs, waits until it has stayed for the whole motion.
    struct {
        struct mainui_backdrop shown;  // the item whose background is drawn
        int effect_due;                // its effect or colour has not run yet
        char path[128];            // the background drawn; empty for the theme's
        char from[128];            // the one it fades from
        uint64_t start_ms;
        unsigned serial;           // counts new backgrounds
        int started;
        // Effect scripts run one at a time on their own thread; a request still waiting is
        // replaced by a newer one (static: BGWorkThread holds one).
        pthread_t thread;
        pthread_mutex_t lock;
        pthread_cond_t wake;
        int thread_running, stop, has_queued;
        char queued[128];
    } backdrop;
    // A carousel's cards slide to a new focus (the stock cards jump): Stock++'s one-row layout,
    // and in BrickOS the one-row layout, and game lists, Recent and Favorites in the Carousel
    // view. At start_ms they stood `from` cards off their places: 1 while the focused card is
    // still where its right neighbor stands. step is the last left or right key, -1 or 1; jump
    // marks a page or group key, whose move puts the cards in place. The carousel followed is
    // named by the presentation, the window's kind and depth, the tab, and the system.
    struct {
        int valid, focus, count, step, jump;
        int presentation, kind, depth, system;
        enum mainui_tab tab;
        int duration_ms;
        float from;
        uint64_t start_ms;
    } carousel;
    // BrickOS's system background (docs/material-you-frontend.md#system-backgrounds): the system
    // whose picture shows, or -1, the one fading out, and the fade, on the carousel's clock. root
    // is the destination at the stack's root; a new one, like the first turn, starts with no fade.
    struct {
        int valid, root, shown, from, duration_ms;
        uint64_t start_ms;
    } system_background;
    int wifi_shown, wifi_scanning;
    int wifi_first;                 // the screen's first scan runs: the list says Scanning
    uint64_t wifi_next_ms;          // the next scan, or the reading of the one asked
    int wifi_rssi;                  // the connection's signal (signal_poll), or MAINUI_RSSI_NONE
    int dsp1;                       // 1 while /dev/dsp1, a second sound device, can be read
    // Wi-Fi work under its progress window, run in steps from the tick: a connect, or the saved
    // network popup's Connect or Forget.
    int wifi_job;                   // enum mainui_effect_kind of the work
    int wifi_step;                  // 0 when nothing runs
    int wifi_id, wifi_polls, wifi_open;
    int wifi_current;               // the connect is for the network in use
    uint64_t wifi_step_ms;
    char wifi_ssid[MAINUI_SSID_MAX], wifi_password[MAINUI_TEXT_MAX];
    uint64_t status_next_ms;        // the next reading of the connection for the status bar
    // Bluetooth: My Devices and Other Devices, and the trimui_btmanager FIFOs.
    struct mainui_bt_device paired[MAINUI_BT_MAX], found[MAINUI_BT_MAX];
    int paired_count, found_count;
    int bt_fd;                      // the result FIFO, open for reading while Bluetooth runs
    int bt_request_fd;              // the request FIFO, open once it is needed
    char bt_data[8192];
    size_t bt_length;
    int bt_listed, bt_scanning, bt_shown;
    int bt_devices_known;           // a device list has arrived since the start
    char bt_queue[MAINUI_BT_QUEUE][64];  // requests waiting their turn
    int bt_queued;
    uint64_t bt_sent_ms, bt_info_ms, progress_ms, notice_ms;
    char bt_pending[16];            // the connect or remove waiting for its answer, or ""
    uint64_t bt_pending_ms;
    int bt_pairable;                // the adapter was made pairable for a connect
    int tz_flip;                    // alternates TZ so the clock reloads a changed zone
    struct mainui_zone_folder zones[MAINUI_ZONE_LEVELS];
    struct mainui_file_folder *file_folders; // caches retained until browser closes
    int *file_history;
    int file_depth, file_count, file_capacity;
    int resume_files, resume_file_title, resume_file_focus, resume_file_first;
    int resume_file_has_paths;
    char resume_file_root[1024], resume_file_path[1024], resume_file_focus_name[256];
    char resume_focus_path[1024];
    char upgrade_path[1024];        // selected file, retained only while its dialog is open
    int exit_requested;             // a launch or upgrade was handed to the init
    int exit_status;
    int effects_done, effects_failed;
    // Settings extensions (docs/settings-extensions.md): the registry of their files, the
    // runner of their scripts, and the texts their windows show.
    struct mainui_ext_registry ext;
    struct mainui_ext_runner ext_runner;
    // GameSwitcher's picker, run over the frontend instead of being handed to the init
    // (docs/mainui.md): its job while it runs, the app entry it stands for, the checkpoint it
    // named, and how it ended. picker_handoff marks a launch the picker asked for.
    struct mainui_ext_runner picker_runner;
    int picker_job, picker_ended, picker_status, picker_handoff, picker_lines;
    struct mainui_list_entry picker_entry;
    char picker_id[40], picker_state[48];
    int ext_on_settings;            // the Settings grid was in front after the last key
    int ext_changed;                // a script changed something on screen since the last tick
    uint64_t ext_now;               // the last tick
    char ext_line[256];             // a command's newest visible line, under its progress view
    char ext_notice[256];           // the line notice
    char ext_confirm[256];          // the confirm: prompt
    char *ext_view;                 // the output view: up to 64 KiB of text, its row and job, and
    size_t ext_view_length;         // the exit status (-1 while the script runs)
    int ext_view_node, ext_view_job, ext_view_status, ext_view_lines;
    // Settings > Keymap > Button Mapping (docs/mainui.md): the model's MAP values come from the
    // input daemon's profile, and a change is written once it has settled for 500 ms.
    enum mainui_mapping_status mapping_status;
    int mapping_pending;            // a change waits to be written
    uint64_t mapping_pending_since; // 0 until the next tick starts the settle time
    uint64_t mapping_read_ms;       // the last read while Button Mapping is in front
    // Settings > Display > LED Policy (docs/ledd.md): the model's LED values come from ledpolicy
    // and the LED daemon's configuration, and the daemon's health is read each second.
    enum mainui_led_status led_status;
    uint64_t led_read_ms;           // the last health read while Display is in front
    // BrickOS Themes > Appearance (docs/material-you-frontend.md): the model's APPEAR values and
    // the font come from appearance.json, and a change is written once it has settled for 500 ms.
    struct mainui_appearance appearance;
    struct mainui_font_entry *fonts; // the fonts folder's valid fonts
    int font_count;
    int appearance_pending;            // a change waits to be written
    uint64_t appearance_pending_since; // 0 until the next tick starts the settle time
};

// Loads the settings, catalog, lists, themes, and language under root ("" on the device) and
// starts the model on the stock start screen. Returns 0, or -1 when the catalog cannot load.
int mainui_frontend_open(struct mainui_frontend *fe, const char *root, int dry, FILE *record);
void mainui_frontend_set_presentation(struct mainui_frontend *fe, enum mainui_presentation presentation);
// The theme the screens and the menu sounds follow: the Default theme ("../res/") in BrickOS,
// which uses nothing from the stock theme, and the stock theme in Stock++. The saved theme, which
// the OSD and the music widget follow, does not change with the frontend.
const char *mainui_frontend_look_theme(const struct mainui_frontend *fe);
void mainui_frontend_close(struct mainui_frontend *fe);
// Startup effects after adapters are set, and an existing power marker before the first frame.
void mainui_frontend_start(struct mainui_frontend *fe);
// The official exit's markers, after the loop ends cleanly (a launch handoff included): the
// chosen Dpad to Analog Key(hold) marker is made again for the child. Dry runs write nothing.
void mainui_frontend_finish(struct mainui_frontend *fe);
// GameSwitcher's picker runs over the frontend: while it does, MainUI draws nothing and takes no
// user input.
int mainui_frontend_picker_active(const struct mainui_frontend *fe);
// Reads the picker's output and its end. Returns 1 when it ended in this call: on a chosen game
// the resume is handed to the init (exit_requested), and an entry without the picker hands the
// app to the init as before.
int mainui_frontend_picker_poll(struct mainui_frontend *fe, uint64_t now_ms);
// Asks the picker to end, as when MainUI must end; poll until it is no longer active.
void mainui_frontend_picker_cancel(struct mainui_frontend *fe, uint64_t now_ms);
// Polls the network adapters: the Wi-Fi scan while its screen is in front, the Bluetooth
// answers and requests, and progress waits (including power). now_ms is a monotonic clock.
// Returns 1 when the screen changed.
int mainui_frontend_tick(struct mainui_frontend *fe, uint64_t now_ms);

// Diagnostics: switches the theme folder or language file in memory only (system.json is not
// written), for renders of other combinations. NULL keeps the current one.
void mainui_frontend_use(struct mainui_frontend *fe, const char *theme, const char *language);
// A key or an outside event, with its effects carried out. Returns 1 when the screen changed.
int mainui_frontend_key(struct mainui_frontend *fe, enum mainui_key key);
int mainui_frontend_key_at(struct mainui_frontend *fe, enum mainui_key key, uint64_t now_ms);
// Takes the volume and mute keymon holds, which the buttons change without the frontend.
// Returns 1 when a shown value changed.
int mainui_frontend_sync(struct mainui_frontend *fe);
int mainui_frontend_event(struct mainui_frontend *fe, enum mainui_event event);
// In model.c: Apps or GameSwitcher's desktop card came or went. A focused card that went hands
// the focus to the shortcut before it, and the desktop row and the Apps page fit what is left.
// Declared here, not in model.h: the OSD, the menu and GameSwitcher include model.h, and a new
// declaration there renumbers their static symbols, so their bytes would change.
void mainui_model_catalog_changed(struct mainui_model *m);
// File-browser selection: total/free_bytes are the SD statvfs byte counts
// f_bsize*f_blocks and f_bsize*f_bfree (including reserved blocks), not the
// selected file's size. Returns 1 for the confirmation, 0 for the low-space notice,
// or -1 for an invalid/missing candidate or an already active upgrade dialog.
// Nothing is published until the user confirms; this does not execute the upgrade.
int mainui_frontend_offer_upgrade(struct mainui_frontend *fe, const char *path,
                                 uint64_t total, uint64_t free_bytes);
// Ordinary conditional browser, also used by a restored stock type-4 window.
// NULL path selects /mnt/, or /mnt/P2EXT for the stock other-platform card condition.
// title is the stock language index.
// Returns 0, or -1 without changing the current window when the root cannot load.
int mainui_frontend_browse(struct mainui_frontend *fe, const char *path, int title);
const struct mainui_file_folder *mainui_frontend_file_folder(const struct mainui_frontend *fe, int level);

// Writes the system lists' positions to /mnt/romwinidx.json when they changed, as the official
// does as it exits (not in a dry run). Returns 0, or -1 when the write failed.
int mainui_frontend_save_positions(struct mainui_frontend *fe);

// The recent or favorite entry the Recent/Favorites tab shows at a row, or NULL.
const struct mainui_list_entry *mainui_frontend_recent(const struct mainui_frontend *fe, int row);
const struct mainui_list_entry *mainui_frontend_favorite(const struct mainui_frontend *fe, int row);
// The label of the system whose launcher an entry runs ("FC"), or NULL for an app or no match.
const char *mainui_frontend_entry_system(const struct mainui_frontend *fe, const struct mainui_list_entry *e);
const char *mainui_frontend_entry_label(const struct mainui_frontend *fe, const struct mainui_list_entry *e);
// The app whose launcher an app entry runs, or the port a port entry runs, or NULL.
const struct mainui_app *mainui_frontend_entry_app(const struct mainui_frontend *fe, const struct mainui_list_entry *e);
// The connected Bluetooth device with an audio icon, which the status bar shows, or NULL.
const struct mainui_bt_device *mainui_frontend_audio_device(const struct mainui_frontend *fe);
// The games of a visible system (loaded on first use); returns the count.
int mainui_frontend_games(struct mainui_frontend *fe, int visible_system);
// Current ROM window's directory; search and counts continue to use the system root.
int mainui_frontend_roms(struct mainui_frontend *fe, int visible_system);
// The selected result's owned metadata, valid until the next search or close.
const struct mainui_game *mainui_frontend_search_game(const struct mainui_frontend *fe, int row);
const struct mainui_system *mainui_frontend_system(const struct mainui_frontend *fe, int visible_system);
// How far a motion that started at start_ms has come at now_ms, 0 to 1, easing out (cubic); 1
// once MAINUI_MOTION_MS have passed or for no motion (start_ms 0).
float mainui_frontend_motion_progress(uint64_t start_ms, uint64_t now_ms);
// The same over duration_ms (0: already done).
float mainui_frontend_motion_progress_over(uint64_t start_ms, uint64_t now_ms, int duration_ms);
// A BrickOS motion's duration: MAINUI_MOTION_MS times the Animations scale (1, 0.5, 0); Stock++'s
// is MAINUI_MOTION_MS.
int mainui_frontend_motion_ms(const struct mainui_frontend *fe);
// How far the background fade has come at now_ms, 0 to 255, on the cards' easing.
int mainui_frontend_backdrop_alpha(const struct mainui_frontend *fe, uint64_t now_ms);
// How many cards the followed carousel's cards still stand off their places at now_ms; 0 when
// they are in place.
float mainui_frontend_carousel_shift(const struct mainui_frontend *fe, uint64_t now_ms);
// The system whose background BrickOS draws, under popups, dialogs, notices, progress views and
// a keyboard: the focused system on Games, the focused collection on Bests (from
// MAINUI_BEST_SYSTEM_BASE), the system of a game list or of a search limited to one; else -1.
int mainui_frontend_background_system(struct mainui_frontend *fe);
// How far the system background's fade has come at now_ms, 0 to 1 (1 at rest).
float mainui_frontend_background_fade(const struct mainui_frontend *fe, uint64_t now_ms);
// Follows the focus: the one-row cards start sliding and the focused item's background fading
// in. The tick calls it too, but the main loop calls it each turn so the motion starts with the
// key. Returns 1 when a frame is needed.
int mainui_frontend_motion(struct mainui_frontend *fe, uint64_t now_ms);
// A frame that began at from_ms took stall_ms, such as one decoding a picture for the first time:
// the slides and fades still under way at from_ms start stall_ms later, so they go on from where
// that frame drew them instead of ending at once.
void mainui_frontend_motion_stall(struct mainui_frontend *fe, uint64_t from_ms, uint64_t stall_ms);
// Counts the games of every shown system for the Games grid, which the official grid shows only
// for systems it has opened (a stock defect). background 1 counts in a thread, and the tick
// reports each new count; 0 counts before returning. A catalog reload counts again.
void mainui_frontend_count_games(struct mainui_frontend *fe, int background);
// The first MainUI of each boot writes a new image for each random Bootlogo and Splashscreen,
// on a worker, once the first frame is shown; /tmp/brickos-boot-visuals.rotated marks the
// boot as done. A dry run logs the picks and writes nothing, the marker included. Returns 1
// when a worker started; close waits for it.
int mainui_frontend_boot_rotation(struct mainui_frontend *fe);
// Binds the idle music picture's writer, once its image library is ready, and starts the
// picture when it is wanted (docs/mainui.md).
void mainui_frontend_music_writer(struct mainui_frontend *fe,
                                  int (*write)(const char *source, const char *destination,
                                               const unsigned char seed[3], double level));
// Raw Hardware Test snapshot, motor ownership and read-only side FN polling.
int mainui_frontend_io(struct mainui_frontend *fe, uint32_t held);
// A shown system's game count, or -1 while it is not counted.
int mainui_frontend_game_count(const struct mainui_frontend *fe, int visible_system);
// 1 when a Recent entry (arg unused) or a system's game (arg: the visible system) is a favorite.
int mainui_frontend_is_favorite(struct mainui_frontend *fe, enum mainui_list list, int arg, int index);

#endif
