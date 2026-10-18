#ifndef BRICKOS_MAINUI_MODEL_H
#define BRICKOS_MAINUI_MODEL_H

#include <stdint.h>

// The frontend's screen, focus, and action model, independent of SDL and of files. It keeps a
// window stack, as the official MainUI does (its /tmp/state.json), turns key presses into
// focus changes and window transitions, and reports every action with an outside effect
// (launch, setting change, list edit, system command) as a queued effect for the adapters.
// Catalog sizes come from mainui_catalog callbacks, so host tests can replay traces against
// fixed content.
//
// The navigation rules are the ones measured on the official MainUI (the inventory and
// navigation sessions of task 1.3/2.2); a rule not measured yet is marked "open" where it is
// implemented.

#define MAINUI_ROM_PATH_BYTES 1024
// A nonempty folder component consumes at least a separator and one byte. Keep
// every representable ancestor plus the tab/root and search/popup windows.
#define MAINUI_STACK_MAX (MAINUI_ROM_PATH_BYTES / 2 + 16)
#define MAINUI_EFFECTS_MAX 32
#define MAINUI_TEXT_MAX 128  // official ImeWidget's 128-position input buffer, including NUL
#define MAINUI_POPUP_ITEMS_MAX 8
#define MAINUI_MATERIAL_HOME_VISIBLE 4

// Physical keys, before the configured confirmation button is applied.
enum mainui_key {
    MAINUI_KEY_UP, MAINUI_KEY_DOWN, MAINUI_KEY_LEFT, MAINUI_KEY_RIGHT,
    MAINUI_KEY_A, MAINUI_KEY_B, MAINUI_KEY_X, MAINUI_KEY_Y,
    MAINUI_KEY_L1, MAINUI_KEY_R1, MAINUI_KEY_L2, MAINUI_KEY_R2,
    MAINUI_KEY_START, MAINUI_KEY_SELECT, MAINUI_KEY_MENU,
    MAINUI_KEY_MENU_START,  // the chord that leaves the IO test
    MAINUI_KEY_COUNT,
};

// Explicit visual/navigation policy for a frontend cycle. Omitted command-line selection is
// STOCK so diagnostics and existing installations retain their current behavior.
enum mainui_presentation {
    MAINUI_PRESENTATION_STOCK,
    MAINUI_PRESENTATION_MATERIAL_YOU,
};

enum mainui_brickos_initial_view {
    MAINUI_BRICKOS_VIEW_DESKTOP, MAINUI_BRICKOS_VIEW_GAMES, MAINUI_BRICKOS_VIEW_FAVORITES,
    MAINUI_BRICKOS_VIEW_RECENT, MAINUI_BRICKOS_VIEW_GAMESWITCHER, MAINUI_BRICKOS_VIEW_APPS,
    MAINUI_BRICKOS_VIEW_LAST_USED,
    MAINUI_BRICKOS_VIEW_COUNT,
};

// Tabs in the official order. Bests and Ports show only when their folders have content.
enum mainui_tab {
    MAINUI_TAB_FAVORITES, MAINUI_TAB_RECENT, MAINUI_TAB_BESTS, MAINUI_TAB_GAMES,
    MAINUI_TAB_PORTS, MAINUI_TAB_APPS, MAINUI_TAB_NETPLAY, MAINUI_TAB_SETTINGS,
    MAINUI_TAB_COUNT,
};
// The BrickOS desktop's cards: the tabs, then the destinations that are not tabs. GameSwitcher's
// card shows while its app is installed and opens the app.
enum { MAINUI_HOME_GAMESWITCHER = MAINUI_TAB_COUNT, MAINUI_HOME_COUNT };

// Lists the catalog is asked about.
enum mainui_list {
    MAINUI_LIST_FAVORITES, MAINUI_LIST_RECENT, MAINUI_LIST_BESTS, MAINUI_LIST_SYSTEMS,
    MAINUI_LIST_PORTS, MAINUI_LIST_APPS, MAINUI_LIST_NETPLAY,
    MAINUI_LIST_ROMS,           // the games of one system (argument: the system index)
    MAINUI_LIST_LAUNCHERS,      // a system's launchlist (argument: the system index)
    MAINUI_LIST_SEARCH,         // results of the last search
    MAINUI_LIST_THEMES, MAINUI_LIST_LANGUAGES, MAINUI_LIST_TIMEZONES,
    MAINUI_LIST_WIFI, MAINUI_LIST_BLUETOOTH,
    MAINUI_LIST_EDIT,           // entries of the Edit screen (argument: MAINUI_TAB_GAMES or APPS)
    MAINUI_LIST_DEVICE_INFO,
    MAINUI_LIST_PAIRED,         // Bluetooth: My Devices (MAINUI_LIST_BLUETOOTH is Other Devices)
    MAINUI_LIST_FILES,          // ordinary FileBrowser (argument: its current folder level)
    MAINUI_LIST_BOOTLOGOS, MAINUI_LIST_SPLASHSCREENS,
    MAINUI_LIST_LOADING_SCREENS, // the presets; the model shows TRIMUI before them as entry 0
    MAINUI_LIST_FONTS,           // the fonts folder; the model shows Default before them as entry 0
    MAINUI_LIST_COUNT,
};

// The system.json settings the model presents. Text settings (theme, language, usbmode,
// dpad2axis) are held as the index of their value. The MAP entries are Settings > Keymap >
// Button Mapping's rows, which the frontend reads from and writes to the input daemon's
// profile, never to system.json. The LED entries are Settings > Display's LED Policy row and
// its Custom rows, which the frontend reads from and writes to the LED daemon's configuration;
// only LED Policy also sets the stock ledpolicy. The APPEAR entries are the BrickOS
// Themes > Appearance rows, which the frontend reads from and writes to appearance.json.
enum mainui_setting {
    MAINUI_SET_VOL, MAINUI_SET_MUTE, MAINUI_SET_BGMVOL, MAINUI_SET_BRIGHTNESS, MAINUI_SET_KEYMAP,
    MAINUI_SET_LANGUAGE, MAINUI_SET_THEME, MAINUI_SET_HOSTAP, MAINUI_SET_NETPLAY, MAINUI_SET_WIFI,
    MAINUI_SET_CPUFREQ, MAINUI_SET_LEDSWITCH, MAINUI_SET_TOPLED, MAINUI_SET_SHOULDERLED,
    MAINUI_SET_F1F2LED, MAINUI_SET_LEDPOLICY, MAINUI_SET_MENUSTYLEL1, MAINUI_SET_MENUSTYLEL2,
    MAINUI_SET_BLUETOOTH, MAINUI_SET_USENETWORKTIME, MAINUI_SET_24HOURCLOCK, MAINUI_SET_RUMBLELEVEL,
    MAINUI_SET_DIMTIME, MAINUI_SET_COLORBRIGHTNESS, MAINUI_SET_COLORCONTRAST,
    MAINUI_SET_COLORTEMPERATURE, MAINUI_SET_COLORSATURATION, MAINUI_SET_USBMODE,
    MAINUI_SET_ENABLEHOME, MAINUI_SET_FOCUSONTAB, MAINUI_SET_BCONFIRM, MAINUI_SET_OSDSHORTCUT,
    MAINUI_SET_PICTURESIZE, MAINUI_SET_FAVORITETAB, MAINUI_SET_RECENTPLAYEDTAB, MAINUI_SET_BESTSTAB,
    MAINUI_SET_RETROGAMETAB, MAINUI_SET_PORTSTAB, MAINUI_SET_APPSTAB, MAINUI_SET_NETPLAYTAB,
    MAINUI_SET_ENABLEKEYTONE, MAINUI_SET_ENABLESSH, MAINUI_SET_DPAD2AXIS,
    MAINUI_SET_MAP_LAYOUT, MAINUI_SET_MAP_SELECT, MAINUI_SET_MAP_START, MAINUI_SET_MAP_F1,
    MAINUI_SET_MAP_F2, MAINUI_SET_MAP_VOLUP, MAINUI_SET_MAP_VOLDOWN,
    MAINUI_SET_LED_OPTION, MAINUI_SET_LED_EFFECT, MAINUI_SET_LED_COLOUR, MAINUI_SET_LED_TIME,
    MAINUI_SET_APPEAR_ACCENT, MAINUI_SET_APPEAR_BACKGROUND, MAINUI_SET_APPEAR_TEXT_SIZE, MAINUI_SET_APPEAR_SHAPE,
    MAINUI_SET_APPEAR_WALLPAPER, MAINUI_SET_APPEAR_FOCUS, MAINUI_SET_APPEAR_ANIMATIONS,
    MAINUI_SET_APPEAR_GAME_VIEW, MAINUI_SET_APPEAR_RECENT_VIEW, MAINUI_SET_APPEAR_FAVORITES_VIEW,
    MAINUI_SET_COUNT,
};

// The LED Policy row: the stock Default and Follow Theme, then the LED daemon's modes. Only
// Follow Theme sets ledpolicy to 1. Effect and Colour hold the LED daemon's numbering (Off,
// Linear, Breath, Sniff, Static, Blink 1-3; Random, Red, Green, Blue, Yellow, Purple, Cyan,
// White), and Time is 1 to 10 seconds.
enum mainui_led_option {
    MAINUI_LED_DEFAULT, MAINUI_LED_FOLLOW_THEME, MAINUI_LED_BATTERY, MAINUI_LED_CPU,
    MAINUI_LED_TEMPERATURE, MAINUI_LED_CHARGING, MAINUI_LED_CUSTOM,
    MAINUI_LED_OPTIONS,
};
#define MAINUI_LED_EFFECTS 8
#define MAINUI_LED_COLOURS 8
// 1 for the LED settings.
int mainui_model_led_setting(int setting);

// The BrickOS Appearance options. Each default, today's look, is 0, so an appearance left zeroed
// is today's look; Text size and Wallpaper dimming rows step through them in another order
// (Small, Normal, Large, Extra large; None, Soft, Strong).
enum mainui_accent {
    MAINUI_ACCENT_VIOLET, MAINUI_ACCENT_INDIGO, MAINUI_ACCENT_BLUE, MAINUI_ACCENT_TEAL, MAINUI_ACCENT_GREEN,
    MAINUI_ACCENT_LIME, MAINUI_ACCENT_AMBER, MAINUI_ACCENT_ORANGE, MAINUI_ACCENT_RED, MAINUI_ACCENT_PINK,
    MAINUI_ACCENTS,
};
enum mainui_background { MAINUI_BACKGROUND_DARK, MAINUI_BACKGROUND_DARKER, MAINUI_BACKGROUND_BLACK, MAINUI_BACKGROUNDS };
enum mainui_text_size {
    MAINUI_TEXT_NORMAL, MAINUI_TEXT_SMALL, MAINUI_TEXT_LARGE, MAINUI_TEXT_EXTRA_LARGE, MAINUI_TEXT_SIZES,
};
enum mainui_shape { MAINUI_SHAPE_ROUNDED, MAINUI_SHAPE_SUBTLE, MAINUI_SHAPE_SQUARE, MAINUI_SHAPES };
enum mainui_dimming { MAINUI_DIM_SOFT, MAINUI_DIM_NONE, MAINUI_DIM_STRONG, MAINUI_DIMMINGS };
enum mainui_halo { MAINUI_HALO_PULSE, MAINUI_HALO_STEADY, MAINUI_HALO_OUTLINE, MAINUI_HALOS };
enum mainui_motion { MAINUI_MOTION_NORMAL, MAINUI_MOTION_FAST, MAINUI_MOTION_OFF, MAINUI_MOTIONS };
// How game lists, Recent and Favorites show their entries (docs/material-you-frontend.md).
enum mainui_list_view { MAINUI_VIEW_LIST, MAINUI_VIEW_GRID, MAINUI_VIEW_CAROUSEL, MAINUI_VIEWS };
#define MAINUI_SET_APPEAR_LAST MAINUI_SET_APPEAR_FAVORITES_VIEW
#define MAINUI_APPEAR_SETTINGS (MAINUI_SET_APPEAR_LAST - MAINUI_SET_APPEAR_ACCENT + 1)
// 1 for the Appearance settings.
int mainui_model_appearance_setting(int setting);

// Button Mapping: what a key row binds its key to. Every destination but Default (no binding)
// and Disabled emits the code the named button sends by default. Custom is a binding the
// screen does not offer, kept until the row changes; it is outside every row's values.
enum mainui_map_dest {
    MAINUI_MAP_DEFAULT, MAINUI_MAP_A, MAINUI_MAP_B, MAINUI_MAP_X, MAINUI_MAP_Y, MAINUI_MAP_L1,
    MAINUI_MAP_R1, MAINUI_MAP_START, MAINUI_MAP_SELECT, MAINUI_MAP_MENU, MAINUI_MAP_F1,
    MAINUI_MAP_F2, MAINUI_MAP_VOLDOWN, MAINUI_MAP_VOLUP, MAINUI_MAP_DISABLED,
    MAINUI_MAP_DESTS,
    MAINUI_MAP_CUSTOM = 99,
};
// The Layout row: the printed labels, or A/B and X/Y swapped (or Custom).
enum { MAINUI_MAP_NINTENDO, MAINUI_MAP_XBOX };
// The destinations' English names; Default and Disabled are translated where they are drawn.
extern const char *const mainui_map_dest_names[MAINUI_MAP_DESTS];
// 1 for the MAP settings.
int mainui_model_mapping_setting(int setting);

enum mainui_window_kind {
    MAINUI_WIN_HOME, MAINUI_WIN_TABS, MAINUI_WIN_ROMS, MAINUI_WIN_POPUP, MAINUI_WIN_DIALOG,
    MAINUI_WIN_SETTINGS, MAINUI_WIN_KEYBOARD, MAINUI_WIN_SEARCH, MAINUI_WIN_EDIT,
    MAINUI_WIN_PROGRESS,  // a wait with a message; keys wait for the end
    MAINUI_WIN_NOTICE,    // a message with OK that closes by itself after 5 s (the frontend's
                          // timer), or earlier with A or B
    MAINUI_WIN_FILES,
    MAINUI_WIN_TEXT,      // a settings extension's output: UP and DOWN scroll, B closes (index: the
                          // node; first: the top line; item_count: the line count)
};

// What a notice says.
enum mainui_notice {
    MAINUI_NOTICE_DEVICE_REMOVED,   // "Device Removed", about 5 s after a device is removed (d1)
    MAINUI_NOTICE_CONNECTION_FAILED,// "Connection Failed" after a failed connect (static: the
                                    // Bluetooth screen's NotifyWindow)
    MAINUI_NOTICE_WIFI_FAILED,      // "Fail to connect / Check your password / Or reboot router"
                                    // after a Wi-Fi connect with a password or to an open network
                                    // (static: WifiMenu event 42, the NotifyWindow of d1)
    MAINUI_NOTICE_UPGRADE_SPACE,    // language 58, index holds free SD megabytes
    MAINUI_NOTICE_BOOT_VISUAL_APPLIED, MAINUI_NOTICE_BOOT_VISUAL_FAILED,
    MAINUI_NOTICE_PREFERENCE_SAVE_FAILED,
    MAINUI_NOTICE_LOADING_SCREEN_APPLIED, MAINUI_NOTICE_LOADING_SCREEN_FAILED,
    MAINUI_NOTICE_EXTENSION_LINE,   // a settings extension's last output line (the frontend's text)
    MAINUI_NOTICE_MAPPING_SAVE_FAILED, // Button Mapping could not write the input profile
    MAINUI_NOTICE_APPEARANCE_SAVE_FAILED, // BrickOS Appearance could not write appearance.json
    MAINUI_NOTICE_COUNT,
};

// What a progress window waits for.
enum mainui_progress {
    MAINUI_PROGRESS_REFRESH,        // Confirmed Refresh Roms: "Refresh" (L53)
    MAINUI_PROGRESS_BLUETOOTH_ON,   // "Turning on bluetooth" (w1)
    MAINUI_PROGRESS_BLUETOOTH_OFF,  // "Turning off bluetooth"
    // Wi-Fi work, in the Bluetooth window with the action's name (static: ProgressDialog, as for
    // Bluetooth): a connect with a password or to an open network ("WiFi"), and the popup's
    // Connect and Forget.
    MAINUI_PROGRESS_WIFI, MAINUI_PROGRESS_WIFI_CONNECT, MAINUI_PROGRESS_WIFI_FORGET,
    // Power off and Flash Mode after OK, until the device goes down (static: ProgressDialog(86)
    // and ProgressDialog(29)).
    MAINUI_PROGRESS_POWER_OFF, MAINUI_PROGRESS_FLASH_MODE,
    // A settings extension's command (index: its node). Unlike the stock waits, B cancels it.
    MAINUI_PROGRESS_EXTENSION,
    // Reboot after the power dialog's Reboot, until the device restarts (index: the source, as
    // for MAINUI_PROGRESS_POWER_OFF).
    MAINUI_PROGRESS_REBOOT,
};

// What the keyboard's text is for. START runs the search, or hands the text to its setting.
enum mainui_keyboard_purpose {
    MAINUI_TYPE_SEARCH, MAINUI_TYPE_WIFI_PASSWORD, MAINUI_TYPE_HOTSPOT_SSID,
    MAINUI_TYPE_HOTSPOT_PASSWORD,
};

// Events from outside the controller.
enum mainui_event {
    MAINUI_EVENT_POWER_HOLD,     // POWER held: the power-off dialog
    MAINUI_EVENT_UPGRADE_FOUND,  // an eligible file was selected: "Update system?"
    MAINUI_EVENT_PROGRESS_DONE,  // an adapter's progress wait finished
    MAINUI_EVENT_NOTICE_DONE,    // the notice's time is over
    MAINUI_EVENT_POWER_REQUEST,  // /tmp/poweroff_flag: direct progress, without confirmation
};

// Settings screens. The tiles of the Settings tab open the first nine, in this order; the tiles
// after them open settings extensions, as MAINUI_SCREEN_EXTENSION with the menu node in index.
// THEMES is the Themes menu; the stock theme list behind its Theme row is THEME_LIST, added last
// so that the tile and saved screen numbers keep their meaning.
enum mainui_screen {
    MAINUI_SCREEN_DISPLAY, MAINUI_SCREEN_VOLUME, MAINUI_SCREEN_WIFI, MAINUI_SCREEN_HOTSPOT,
    MAINUI_SCREEN_BLUETOOTH, MAINUI_SCREEN_THEMES, MAINUI_SCREEN_KEYMAP, MAINUI_SCREEN_HOMEPAGE,
    MAINUI_SCREEN_SYSTEM, MAINUI_SCREEN_TAB_VISIBILITY, MAINUI_SCREEN_LANGUAGE,
    MAINUI_SCREEN_DATETIME, MAINUI_SCREEN_TIMEZONE, MAINUI_SCREEN_DEVELOPER,
    MAINUI_SCREEN_DEVICE_INFO, MAINUI_SCREEN_USB_MODE, MAINUI_SCREEN_IO_TEST,
    MAINUI_SCREEN_SET_DATE, MAINUI_SCREEN_BOOTLOGO, MAINUI_SCREEN_SPLASHSCREEN,
    MAINUI_SCREEN_BRICKOS_STARTUP, MAINUI_SCREEN_FRONTEND, MAINUI_SCREEN_LOADING_SCREEN, MAINUI_SCREEN_EXTENSION,
    MAINUI_SCREEN_BUTTON_MAPPING, MAINUI_SCREEN_THEME_LIST,
    MAINUI_SCREEN_APPEARANCE, MAINUI_SCREEN_APPEARANCE_FONT, // BrickOS only
    MAINUI_SCREEN_COUNT,
};
#define MAINUI_SETTINGS_TILES 9
// ROM windows share an index space: Games 0..127, Bests 128..255.
#define MAINUI_BEST_SYSTEM_BASE 128

enum mainui_popup {
    MAINUI_POPUP_TAB,        // MENU on a tab page or its tab bar
    MAINUI_POPUP_ROMS,       // MENU in a system list
    MAINUI_POPUP_LAUNCHERS,  // X in a system list: the emulator choice
    MAINUI_POPUP_SEARCH,     // MENU on a search result
    MAINUI_POPUP_BT_DEVICE,  // MENU on a device of My Devices (d1)
    MAINUI_POPUP_WIFI,       // A on a saved network (k2b)
};

// Popup rows. A launcher row of the emulator choice is MAINUI_POP_LAUNCHER + its index.
enum mainui_pop {
    MAINUI_POP_EDIT, MAINUI_POP_SEARCH_ALL, MAINUI_POP_REFRESH_ROMS, MAINUI_POP_ADD_FAVORITE,
    MAINUI_POP_REMOVE_FAVORITE, MAINUI_POP_CLEAR_LIST, MAINUI_POP_CLEAR_FAVORITE,
    MAINUI_POP_OPEN, MAINUI_POP_DELETE, MAINUI_POP_SEARCH, MAINUI_POP_PICTURE_SIZE,
    MAINUI_POP_REMOVE_DEVICE, MAINUI_POP_CONNECT, MAINUI_POP_CHANGE_PASSWORD, MAINUI_POP_FORGET,
    MAINUI_POP_LAUNCHER,
};

enum mainui_dialog {
    MAINUI_DIALOG_SAVE_EDIT, MAINUI_DIALOG_CLEAR_RECENT, MAINUI_DIALOG_REMOVE_FAVORITE,
    MAINUI_DIALOG_CLEAR_FAVORITE, MAINUI_DIALOG_DELETE_ROM, MAINUI_DIALOG_FACTORY_RESET,
    MAINUI_DIALOG_POWER_OFF, MAINUI_DIALOG_FLASH_MODE, MAINUI_DIALOG_UPGRADE,
    MAINUI_DIALOG_REMOVE_DEVICE,
    MAINUI_DIALOG_REFRESH_ROMS,
    MAINUI_DIALOG_EXTENSION_CONFIRM,  // a settings extension's confirm: prompt (index: the node)
    MAINUI_DIALOG_RESET_MAPPING,      // Button Mapping's Reset
    MAINUI_DIALOG_RESET_APPEARANCE,   // BrickOS Appearance's Reset appearance
};
// A dialog's buttons; OK has the focus when it opens.
#define MAINUI_DIALOG_CANCEL 0
#define MAINUI_DIALOG_OK 1
// The power dialog's middle button. Its buttons are Power off (OK), Reboot and Cancel, left to
// right, and OK has the focus when it opens.
#define MAINUI_DIALOG_REBOOT 2
// The Bootlogo and Splashscreen lists start with a Random entry; the images follow it, so list
// entry k is catalog index k - 1. A confirm on Random applies this index.
#define MAINUI_BOOT_VISUAL_RANDOM (-1)

enum mainui_effect_kind {
    MAINUI_EFFECT_LAUNCH,          // list, system, index, launcher (-1: the system's own)
    MAINUI_EFFECT_SET,             // setting, value: apply the immediate effect
    MAINUI_EFFECT_PERSIST,         // write system.json
    MAINUI_EFFECT_ADD_FAVORITE,    // list, system, index
    MAINUI_EFFECT_REMOVE_FAVORITE, // list, system, index
    MAINUI_EFFECT_CLEAR_RECENT,
    MAINUI_EFFECT_CLEAR_FAVORITES,
    MAINUI_EFFECT_DELETE_ROM,      // system, index
    MAINUI_EFFECT_REFRESH_ROMS,
    MAINUI_EFFECT_SEARCH,          // text, system (-1: every system)
    MAINUI_EFFECT_TOGGLE_EDIT,     // system (the tab), index
    MAINUI_EFFECT_SAVE_EDIT,       // system (the tab)
    MAINUI_EFFECT_SELECT,          // list, index: a theme, language, time zone (system: the folder
                                   // level), or Bluetooth device
    MAINUI_EFFECT_EDIT_TEXT,       // value 0 hotspot SSID, 1 hotspot password; text: the new text
    MAINUI_EFFECT_SET_DATE,
    MAINUI_EFFECT_FACTORY_RESET,
    MAINUI_EFFECT_POWER_OFF,      // value 0: Settings, 1: hold, 2: marker; nonzero respects marker
    MAINUI_EFFECT_FLASH_MODE,
    MAINUI_EFFECT_IO_TEST,         // value 1 on entry, 0 on exit
    MAINUI_EFFECT_WIFI_CONNECT,    // index (the network), text (the password), value 1 for an open
                                   // network (no password)
    MAINUI_EFFECT_WIFI_RECONNECT,  // index: select a saved network again (the popup's Connect)
    MAINUI_EFFECT_WIFI_FORGET,     // index: remove a saved network
    MAINUI_EFFECT_UPGRADE,
    MAINUI_EFFECT_REMOVE_DEVICE,   // list (MAINUI_LIST_PAIRED), index: unpair a saved device
    MAINUI_EFFECT_APPLY_DATE,      // text: local calendar, value: the initial tm_isdst
    MAINUI_EFFECT_JUMP_GROUP,      // ROMS: system, index; value -1 previous, 1 next letter group
    MAINUI_EFFECT_FILE_SELECT,     // system: folder level, index: selected filename
    MAINUI_EFFECT_FILE_BACK,       // system: folder level; return to the parent
    MAINUI_EFFECT_PREVIEW_THEME,   // index: focused Themes row after up/down
    MAINUI_EFFECT_CANCEL_EDIT,     // discard toggles and reload persisted visibility
    MAINUI_EFFECT_REFRESH_BOOT_VISUALS, // list identifies bootlogo or splashscreen
    MAINUI_EFFECT_APPLY_BOOT_VISUAL,    // list and index identify one confirmed bitmap, or MAINUI_BOOT_VISUAL_RANDOM
    MAINUI_EFFECT_REFRESH_LOADING_SCREENS,
    MAINUI_EFFECT_APPLY_LOADING_SCREEN, // index: the confirmed entry, 0 for TRIMUI, n for preset n - 1
    MAINUI_EFFECT_SAVE_BRICKOS_PREFERENCES, // value: enum mainui_brickos_initial_view
    MAINUI_EFFECT_SAVE_PRESENTATION, // value: enum mainui_presentation; apply only after saving
    // Settings extensions; index is the node.
    MAINUI_EFFECT_EXTENSION_OPEN,   // a screen with extension rows opened (value: the screen,
                                    // index: the menu node of MAINUI_SCREEN_EXTENSION, else -1)
    MAINUI_EFFECT_EXTENSION_RUN,    // A on a command row
    MAINUI_EFFECT_EXTENSION_CHANGE, // value: the row's new value; launcher 1 to run at once (a
                                    // toggle), 0 to run once the value settles (select, range)
    MAINUI_EFFECT_EXTENSION_CANCEL, // B on a command's progress or output view
    MAINUI_EFFECT_EXTENSION_ANSWER, // value: 'A' or 'B', the answer to the node's confirm: prompt
    MAINUI_EFFECT_EXTENSION_LEAVE,  // a Settings screen was left: run settled values now
    // Button Mapping. A SET of a MAP setting is written once it has settled for 500 ms.
    MAINUI_EFFECT_REFRESH_MAPPING,  // Keymap or Button Mapping opened: read the profile and status
    MAINUI_EFFECT_SAVE_MAPPING,     // Button Mapping was left: write an unsettled change now
    MAINUI_EFFECT_RESET_MAPPING,    // Reset confirmed: remove the screen's bindings
    // LED Policy. A SET of an LED setting is written to the LED daemon's configuration at once;
    // LED Policy's also sets ledpolicy as the stock row does.
    MAINUI_EFFECT_REFRESH_LEDS,     // Display opened: read the LED configuration and the daemon's health
    // BrickOS Appearance. A SET of an APPEAR setting applies at once and is written to
    // appearance.json once it has settled for 500 ms; a SELECT of MAINUI_LIST_FONTS sets the font.
    MAINUI_EFFECT_REFRESH_FONTS,    // Appearance or the font list opened: read the fonts folder
    MAINUI_EFFECT_SAVE_APPEARANCE,  // Appearance or the font list was left: write an unsettled change now
    MAINUI_EFFECT_RESET_APPEARANCE, // Reset appearance confirmed: every choice back to its default
    MAINUI_EFFECT_LAUNCH_GAMESWITCHER, // the GameSwitcher app; value 1 adds it to Recent, as Apps does
    MAINUI_EFFECT_REBOOT,          // the power dialog's Reboot; value as for MAINUI_EFFECT_POWER_OFF
};

struct mainui_effect {
    enum mainui_effect_kind kind;
    int list, system, index, launcher, setting, value;
    char text[MAINUI_TEXT_MAX];
};

struct mainui_catalog {
    void *context;
    // Entries in a list; arg is the system index for ROMS and LAUNCHERS, the tab for EDIT.
    int (*count)(void *context, enum mainui_list list, int arg);
    // 1 when the folder behind Bests or Ports has content; may be NULL (no content).
    int (*has_content)(void *context, enum mainui_tab tab);
    // MAINUI_ENTRY_* flags of one entry of a list; may be NULL (no flags).
    int (*entry_flags)(void *context, enum mainui_list list, int arg, int index);
    // The position a list was left at (ROMS: the system's saved focused, first, and last rows).
    // Returns 0 with one; may be NULL (lists open at the top).
    int (*position)(void *context, enum mainui_list list, int arg, int *pos, int *start, int *end);
    // 1 while the GameSwitcher app is installed, shown in Apps or hidden; may be NULL (not installed).
    int (*gameswitcher)(void *context);
};

// Entry flags.
#define MAINUI_ENTRY_FOLDER 1     // ROMS or TIMEZONES: a directory
#define MAINUI_ENTRY_LOCKED 2     // WIFI: the network asks a password
#define MAINUI_ENTRY_SAVED 4      // WIFI: wpa_supplicant has the network
#define MAINUI_ENTRY_CONNECTED 8  // WIFI and PAIRED

// Settings extension rows, as the frontend's registry describes them to the model.
enum mainui_extension_kind {
    MAINUI_EXTENSION_MENU, MAINUI_EXTENSION_CMD, MAINUI_EXTENSION_TOGGLE, MAINUI_EXTENSION_SELECT,
    MAINUI_EXTENSION_RANGE, MAINUI_EXTENSION_INFO,
};
enum mainui_extension_anchor { MAINUI_EXTENSION_END, MAINUI_EXTENSION_AFTER, MAINUI_EXTENSION_BEFORE };
enum mainui_extension_output { MAINUI_EXTENSION_OUTPUT_NONE, MAINUI_EXTENSION_OUTPUT_LINE, MAINUI_EXTENSION_OUTPUT_FULL };

struct mainui_extension_row {
    int node;
    enum mainui_extension_anchor anchor;
    const char *anchor_id;      // a built-in row id (mainui_model_row_id)
};

struct mainui_extension_info {
    enum mainui_extension_kind kind;
    const char *label;          // the authored English label, for traces and tests
    int value, min, max, step;  // TOGGLE 0/1, SELECT index, RANGE number
    int item_count;             // SELECT
    enum mainui_extension_output output;
    int usable;                 // neither disabled nor busy
};

#define MAINUI_EXTENSION_SECTION_ROWS 256

// Callbacks into the frontend's settings extension registry; any may be NULL (no extensions).
struct mainui_extensions {
    void *context;
    int (*tiles)(void *context);                        // extension tiles after the stock nine
    int (*tile_node)(void *context, int tile);          // a tile's menu node
    int (*children)(void *context, int node, int *nodes, int max);  // a menu's rows, in order
    // The rows extensions add to a built-in section, in extension order.
    int (*section_rows)(void *context, int screen, struct mainui_extension_row *rows, int max);
    int (*info)(void *context, int node, struct mainui_extension_info *info);  // 0, or -1
};

// Settings screen items.
enum mainui_item_kind {
    MAINUI_ITEM_RANGE,    // left/right step by one within min..max
    MAINUI_ITEM_CYCLE,    // left/right move through values[], wrapping when wrap is set
    MAINUI_ITEM_SWITCH,   // A toggles 0/1
    MAINUI_ITEM_SUBMENU,  // A opens screen
    MAINUI_ITEM_ACTION,   // A runs action
    MAINUI_ITEM_LIST,     // one entry of a catalog list (themes, languages, networks, ...)
    MAINUI_ITEM_INFO,     // read-only row
    MAINUI_ITEM_HEADER,   // a section title the focus skips (Bluetooth's My Devices, Other Devices)
    MAINUI_ITEM_DATE,     // left/right wrap a local calendar component (action: 0..5)
    MAINUI_ITEM_INITIAL_VIEW_CHOICE, // action: enum mainui_brickos_initial_view
    MAINUI_ITEM_PRESENTATION_CHOICE, // action: enum mainui_presentation
    MAINUI_ITEM_EXTENSION,           // a settings extension row; action: its node
};

enum mainui_action {
    MAINUI_ACTION_POWER_OFF, MAINUI_ACTION_REFRESH_ROMS, MAINUI_ACTION_FACTORY_RESET,
    MAINUI_ACTION_FLASH_MODE, MAINUI_ACTION_SSID, MAINUI_ACTION_PASSWORD, MAINUI_ACTION_SET_DATE,
    MAINUI_ACTION_RESET_MAPPING, MAINUI_ACTION_RESET_APPEARANCE,
};

struct mainui_item {
    const char *label;          // the English label, for traces and tests
    enum mainui_item_kind kind;
    enum mainui_setting setting;
    int min, max;
    const int *values;
    int value_count;
    int wrap;
    enum mainui_screen screen;  // SUBMENU
    int action;                 // ACTION: a mainui_action; LIST: a mainui_list
};

// A window of the stack. focus and first are the stock currpos and pagestart.
struct mainui_window {
    enum mainui_window_kind kind;
    int focus;
    int first;
    int system;                  // ROMS and SEARCH: the system (-1 all); EDIT: the tab; the
                                 // Time Zone screen: its folder level (0 is zoneinfo itself)
    int list;                    // POPUP and DIALOG: the list the action applies to
    int index;                   // POPUP/DIALOG: entry; POWER_OFF progress: 0 Settings, 1 hold, 2 marker
    enum mainui_screen screen;   // SETTINGS
    enum mainui_popup popup;     // POPUP
    enum mainui_dialog dialog;   // DIALOG
    enum mainui_progress progress;  // PROGRESS
    enum mainui_notice notice;      // NOTICE
    int items[MAINUI_POPUP_ITEMS_MAX];
    int item_count;              // POPUP: action count; ROMS: last loaded row count
    int dirty;                   // EDIT: an entry was toggled
    int resumed;                 // ROMS: rebuilt from /tmp/state.json; its title reads TRIMUI
    char text[MAINUI_TEXT_MAX];  // KEYBOARD: the entry line
    char rompath[MAINUI_ROM_PATH_BYTES]; // ROMS: stock ppath; empty is the system root
    int shift;                   // KEYBOARD: shifted letters
    int symbols;                 // KEYBOARD: the symbol page
    enum mainui_keyboard_purpose purpose;  // KEYBOARD
    int date[6];                  // Set Date: year, month, day, hour, minute, second
    int date_dst;                 // localtime's DST flag, retained until mktime on exit
    uint64_t nav_ms;             // live ListMenu direction streak; never persisted
    enum mainui_key nav_key;
    unsigned int nav_count;
};

// Per-tab page state, kept while the tab window is open.
struct mainui_tab_state {
    int focus;
    int first;
};

struct mainui_model {
    struct mainui_window stack[MAINUI_STACK_MAX];
    int depth;
    // Tab window: the open tab, whether the tab bar has the focus, and each page's focus.
    enum mainui_tab tab;
    int bar_focused;
    struct mainui_tab_state tabs[MAINUI_TAB_COUNT];
    // Stock refresh applies the previously queued positions before appending the current
    // positions. Once their count exceeds the tab limit it can never match again.
    struct mainui_tab_state refresh_tabs[MAINUI_TAB_COUNT];
    int refresh_tab_count;
    // Home page: the focused shortcut (a tab, or on the BrickOS desktop MAINUI_HOME_GAMESWITCHER),
    // whether the search box has the focus, and the first visible shortcut.
    int home_focus;
    int home_search;
    int home_first;
    int settings[MAINUI_SET_COUNT];
    struct mainui_effect effects[MAINUI_EFFECTS_MAX];
    int effect_head, effect_count;
    int effects_dropped;
    enum mainui_presentation presentation;
    enum mainui_brickos_initial_view brickos_initial_view;
    struct mainui_catalog catalog;
    struct mainui_extensions extensions;
    // Button Mapping's profile is invalid: its Layout and key rows ignore LEFT and RIGHT.
    int mapping_locked;
};

// Rows visible in a list, used to keep the focus in view, and the list page size.
#define MAINUI_TAB_LIST_ROWS 5
#define MAINUI_ROM_LIST_ROWS 6
#define MAINUI_ROM_PAGE 6
// The paged grid: 3 columns and 2 rows per page. The compact grid: 8 columns and 5 rows.
#define MAINUI_GRID_COLUMNS 3
#define MAINUI_GRID_ROWS 2
#define MAINUI_COMPACT_COLUMNS 8
#define MAINUI_COMPACT_ROWS 5
// The BrickOS game Grid view: 4 columns and 2 rows per page; L1 and R1 move by a page. The
// Carousel view's L1 and R1 move by MAINUI_ROM_PAGE.
#define MAINUI_GAME_GRID_COLUMNS 4
#define MAINUI_GAME_GRID_ROWS 2
#define MAINUI_GAME_GRID_PAGE (MAINUI_GAME_GRID_COLUMNS * MAINUI_GAME_GRID_ROWS)
// The search keyboard: four rows of ten keys, then shift, symbols, space, delete, and OK, each
// under two columns (e20).
#define MAINUI_KEYBOARD_COLUMNS 10
#define MAINUI_KEYBOARD_KEYS 45
#define MAINUI_SEARCH_LIST_ROWS 5
#define MAINUI_DESKTOP_COLUMNS 4
#define MAINUI_DESKTOP_ROWS 2
#define MAINUI_DESKTOP_CARD_X 70
#define MAINUI_DESKTOP_CARD_Y 138
#define MAINUI_DESKTOP_PITCH_X 236
#define MAINUI_DESKTOP_PITCH_Y 252
static inline int mainui_search_rows(int system) {
    return system >= 0 ? MAINUI_ROM_LIST_ROWS : MAINUI_SEARCH_LIST_ROWS;
}

// Picture sizes in the order Picture Size cycles them, and Lock Screen Time values (-1 never).
extern const int mainui_picture_sizes[3];
extern const int mainui_dim_times[6];

// OSD Shortcut: keymon's hotkey, by the index stored as osdshortcut (static: the official
// OSDShortcutHandler's table; k1 showed MENU+R2 at 4, MENU+Y at 10, and OFF at 49, where the
// value stops at both ends).
#define MAINUI_OSD_SHORTCUTS 50
extern const char *const mainui_osd_shortcut_names[MAINUI_OSD_SHORTCUTS];

// Sets the values found in the official MainUI's system.json on the reference device.
void mainui_settings_defaults(int settings[MAINUI_SET_COUNT]);

// Starts the model on the stock start screen after a restart: the Games tab page with its
// first system in focus (the first visible tab when Games is hidden).
void mainui_model_init(struct mainui_model *m, const int settings[MAINUI_SET_COUNT],
                       const struct mainui_catalog *catalog);
// Select the visual/navigation policy. BrickOS begins at the desktop when the loaded state
// has no valid game/app return context; the stock policy and restored launch state stay intact.
void mainui_model_set_presentation(struct mainui_model *m, enum mainui_presentation presentation,
                                   int valid_return);
void mainui_model_apply_brickos_initial_view(struct mainui_model *m, enum mainui_brickos_initial_view view);

// The window stack a launch leaves for the next frontend in /tmp/state.json (l2b, a2): the
// open tab by its place among the visible tabs, each visible tab's focus and first row, and the
// Games system list when one is open.
struct mainui_resume {
    int home, home_focus, home_first;
    // The BrickOS desktop's focus is the GameSwitcher card; home_focus then names a visible tab
    // beside it, so a stock reader of state.json still finds a tab.
    int home_gameswitcher;
    int tab_index;
    int tab_count;
    struct mainui_tab_state tabs[MAINUI_TAB_COUNT];  // by place among the visible tabs
    int system;                                      // the open list's visible system, or -1
    int focus, first;                                // that list's
    int keyboard, search, search_system, search_focus, search_first;
    int files;                                       // consecutive FileBrowser windows above the view
    char query[MAINUI_TEXT_MAX];
    int rom_count;
    struct { int system, focus, first; char path[MAINUI_ROM_PATH_BYTES]; } roms[MAINUI_STACK_MAX - 1];
};
// Fills r from the stack; returns 0, or -1 when the stack is not the tab window, with or
// without a system list, or a search keyboard/results stack over tabs or home.
int mainui_model_resume_state(const struct mainui_model *m, struct mainui_resume *r);
// Rebuilds the stack from r, keeping positions within the current catalog.
void mainui_model_resume(struct mainui_model *m, const struct mainui_resume *r);

// Opens an ordinary FileBrowser over the current window. title is its stock language
// index (225: TRIMUI). The frontend supplies its folder cache through FILES.
int mainui_model_open_files(struct mainui_model *m, int title);

// Handles one key press (a repeat arrives as another press). Returns 1 when the visible state
// changed.
int mainui_model_key(struct mainui_model *m, enum mainui_key key);
// Resolve the official list's sustained direction into a page action. Untimed
// model commands remain ordinary single actions.
enum mainui_key mainui_model_timed_key(struct mainui_model *m, enum mainui_key key, uint64_t now_ms);

// Takes the oldest queued effect; returns 0 when none is left.
int mainui_model_next_effect(struct mainui_model *m, struct mainui_effect *effect);
// Queue destruction effects of open settings, once, before shutting down the adapters.
void mainui_model_finish(struct mainui_model *m);

// The window on top of the stack.
const struct mainui_window *mainui_model_top(const struct mainui_model *m);

// Available controls of the active BrickOS view; querying never performs an action.
#define MAINUI_HINTS_MAX 8
enum mainui_hint_action {
    MAINUI_HINT_MOVE, MAINUI_HINT_OPEN, MAINUI_HINT_LAUNCH, MAINUI_HINT_SEARCH,
    MAINUI_HINT_BACK, MAINUI_HINT_ACTIONS, MAINUI_HINT_LAYOUT, MAINUI_HINT_PAGE,
    MAINUI_HINT_EMULATOR, MAINUI_HINT_GROUP, MAINUI_HINT_CHANGE, MAINUI_HINT_TOGGLE,
    MAINUI_HINT_APPLY, MAINUI_HINT_CONNECT, MAINUI_HINT_CHOOSE, MAINUI_HINT_SELECT,
    MAINUI_HINT_CANCEL, MAINUI_HINT_CONFIRM, MAINUI_HINT_UPDATE, MAINUI_HINT_CLOSE,
    MAINUI_HINT_SAVE, MAINUI_HINT_TYPE, MAINUI_HINT_SHIFT, MAINUI_HINT_SYMBOLS,
    MAINUI_HINT_SPACE, MAINUI_HINT_DELETE, MAINUI_HINT_DONE, MAINUI_HINT_KEYBOARD,
    MAINUI_HINT_EXIT, MAINUI_HINT_POWER_OFF, MAINUI_HINT_REBOOT, MAINUI_HINT_COUNT,
};
struct mainui_hint { const char *key; enum mainui_hint_action action; };
int mainui_model_hints(const struct mainui_model *m, struct mainui_hint hints[MAINUI_HINTS_MAX]);

// Tabs shown in the tab bar and the home row, in order; returns the count.
int mainui_model_visible_tabs(const struct mainui_model *m, enum mainui_tab tabs[MAINUI_TAB_COUNT]);
// Home destinations in presentation order: tabs, and on the BrickOS desktop GameSwitcher after
// Recent while it is installed. Material You puts Games before the other groups.
int mainui_model_home_items(const struct mainui_model *m, int items[MAINUI_HOME_COUNT]);
// The BrickOS desktop's bounded 4×2 grid is shared by focus movement and rendering.
int mainui_desktop_position(int index, int count, int *column, int *row);
int mainui_desktop_move(int index, int count, enum mainui_key key);
// The level-1 layout a tab shows: menustylel1 (1 the paged grid, 2 compact, 0 one row), except
// Settings, which is always the paged grid.
int mainui_model_tab_layout(const struct mainui_model *m, enum mainui_tab tab);
// The view (enum mainui_list_view) a window shows: in BrickOS, a system's game list follows Game
// lists, and the Recent and Favorites tabs their own choices; every other window, a Bests
// collection and all of Stock++ show their list.
int mainui_model_view(const struct mainui_model *m, const struct mainui_window *w);

// The items a settings screen shows now, for the current settings and catalog; returns the
// count (at most max).
int mainui_model_screen_items(const struct mainui_model *m, enum mainui_screen screen,
                              struct mainui_item *items, int max);
// Settings extensions: the registry's callbacks (copied), the Settings tab's tile count (the
// stock nine plus the extension tiles), and the frontend's hooks for extension windows.
void mainui_model_set_extensions(struct mainui_model *m, const struct mainui_extensions *extensions);
int mainui_model_settings_tiles(const struct mainui_model *m);
// Shows an extension's confirm: prompt, its output view, or (with the frontend's text) its line
// notice over the window in front.
void mainui_model_extension_confirm(struct mainui_model *m, int node);
void mainui_model_extension_output(struct mainui_model *m, int node, int lines);
// Closes the node's command progress and confirm dialog, wherever they are on top.
void mainui_model_extension_close(struct mainui_model *m, int node);
// Closes the node's output view when it is on top (its command could not start).
void mainui_model_extension_close_view(struct mainui_model *m, int node);
// The output view's line count, as it grows; keeps its top line in range.
void mainui_model_text_lines(struct mainui_model *m, int lines);
#define MAINUI_TEXT_ROWS 12

// 1 when Dpad to Analog Key(hold) names Select, Start, F1 or F2 and Button Mapping binds that
// key to an offered destination: the input daemon then drops its hold role.
int mainui_model_hold_remapped(const struct mainui_model *m);

// Capacity needed to materialize a screen's items, including dynamic image catalogs.
int mainui_model_screen_item_capacity(const struct mainui_model *m, enum mainui_screen screen);

// The built-in Settings sections settings extensions may add rows to, by their id ("display",
// "date-time", ...): the screen, or -1 for any other name. mainui_model_extension_section_id is
// the reverse, NULL for a screen that is not such a section.
int mainui_model_extension_section(const char *id);
const char *mainui_model_extension_section_id(enum mainui_screen screen);
// A built-in row's stable id: its English label in lowercase, each run of other characters than
// ASCII letters and digits replaced by one '-', none leading or trailing ("F1/F2 LED Scale" is
// "f1-f2-led-scale").
void mainui_model_row_id(const char *label, char *out, int size);

// The character a keyboard key types, or 0 for the four function keys.
char mainui_keyboard_char(int key, int shift, int symbols);

// The status bar's clock for an "HH:MM" clock: unchanged with the 24-hour clock, which returns
// NULL; else the hour past 12 less 12, returning the "AM" or "PM" the bar draws to its left.
const char *mainui_status_clock(const char *clock, int hour24, char *out, int size);
// The battery icon for a charge in percent.
const char *mainui_status_battery_icon(int percent);
// The width of the box the status bar's percentage is drawn in, whatever the text's width.
int mainui_status_percent_box(int percent);

// Handles an event from outside the controller. Returns 1 when the visible state changed.
int mainui_model_event(struct mainui_model *m, enum mainui_event event);
// Shows a notice over the window in front, until MAINUI_EVENT_NOTICE_DONE.
void mainui_model_notice(struct mainui_model *m, enum mainui_notice notice);

#endif
