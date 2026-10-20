#ifndef BRICKOS_GS_POLICY_H
#define BRICKOS_GS_POLICY_H
#include "store.h"
// settings.json in the store's folder, which Settings > GameSwitcher writes: warning_percent and
// critical_percent (the battery levels), auto_resume, double_tap (two GUIDE taps switch to the
// previous game), guide_menu (a GUIDE tap opens the BrickOS menu and GUIDE+SELECT GameSwitcher)
// and history_limit (the most cards shown, 0 for no limit).
struct gs_policy { int warning,critical,auto_resume,double_tap,guide_menu,history_limit; };
struct gs_battery { int warned,critical_latched; };
enum gs_battery_action { GS_BATTERY_NONE,GS_BATTERY_WARN,GS_BATTERY_EXIT };
void gs_policy_defaults(struct gs_policy *policy);
int gs_policy_load(struct gs_store *store,struct gs_policy *policy);
// Sets one setting after checking it with the others. Every other member of the file stays,
// unknown keys included; a known key that was invalid is written with the value in use. The file
// is replaced through a temporary file and a rename. Returns 0, or -1 with errno: EINVAL for an
// unknown key or a value the rules refuse, another errno when the file cannot be read or written.
int gs_policy_set(struct gs_store *store,const char *key,int value);
// Applies history_limit after a new checkpoint (gs_history_trim). A failure is logged and leaves
// the history as it was; the checkpoint stays published.
void gs_policy_trim_history(struct gs_store *store);
enum gs_battery_action gs_battery_update(struct gs_battery *state,const struct gs_policy *policy,int capacity,int charging);
// The boot's target: the most recent committed, visible entry, only when the resume record
// (a game-aware shutdown's, read with gs_store_resume_take) names that entry and checkpoint.
// 0 without a record or when another checkpoint is newer; 1 with the entry; -1 when the named
// checkpoint can no longer be resumed. An invalid latest target never selects an older game, and
// a game of an excluded system never resumes (0).
int gs_boot_target(struct gs_store *store,const struct gs_resume *resume,struct gs_entry *entry);
int gs_toast(const char *message);
// GameSwitcher's OSD notices. The toast follows the system language; logs use the English text.
enum gs_notice {
    GS_NOTICE_SAVING, GS_NOTICE_LOADING, GS_NOTICE_POWER_COMPLETION_UNAVAILABLE, GS_NOTICE_SAVE_FAILED,
    GS_NOTICE_UNEXPECTED_REQUEST, GS_NOTICE_SESSION_UNAVAILABLE, GS_NOTICE_CLOSED_WITHOUT_SELECTION,
    GS_NOTICE_SELECTION_UNAVAILABLE, GS_NOTICE_SHUTDOWN_PROGRESS_UNAVAILABLE, GS_NOTICE_POWEROFF_START_FAILED,
    GS_NOTICE_POWEROFF_FAILED, GS_NOTICE_RELEASE_FAILED, GS_NOTICE_EXIT_TIMEOUT, GS_NOTICE_CHANNEL_FAILED,
    GS_NOTICE_LOAD_ENTER_FAILED, GS_NOTICE_LOAD_FAILED, GS_NOTICE_SELECTION_CHANGED, GS_NOTICE_POWER_SAVE_FAILED,
    GS_NOTICE_QUIT_SAVE_FAILED, GS_NOTICE_POWER_ENTER_FAILED, GS_NOTICE_STANDBY_CANCELLED, GS_NOTICE_POWER_TIMEOUT,
    GS_NOTICE_BRIDGE_NOT_READY, GS_NOTICE_LOAD_TIMEOUT, GS_NOTICE_RECOVERY_TERMINATION, GS_NOTICE_BATTERY_LOW,
    GS_NOTICE_BATTERY_CRITICAL, GS_NOTICE_BATTERY_SAVE_FAILED, GS_NOTICE_PLAY_TIME_FAILED,
    GS_NOTICE_INVALID_SETTINGS, GS_NOTICE_LAST_GAME_UNAVAILABLE, GS_NOTICE_COUNT
};
#define GS_SYSTEM_JSON "/mnt/UDISK/system.json"
// The most characters a notice may have to fit the toast box (measured on the official OSD).
#define GS_NOTICE_CHARACTERS 38
// The notice's text, Portuguese when `portuguese` is set; NULL for an unknown notice.
const char *gs_notice_text(enum gs_notice notice,int portuguese);
// 1 when the system.json at `path` names Portuguese, else 0 (also when unreadable).
int gs_notice_portuguese(const char *path);
// The toast record for `message`, with GameSwitcher's ID. Returns its length, or -1.
int gs_toast_record(char *out,size_t size,const char *message,int duration_ms);
// An ordinary 4 s notice; a progress notice that lasts the 15 s save budget; and a silent
// close of that progress notice, which replaces it by ID with the same text and 1 ms.
int gs_notice(enum gs_notice notice);
int gs_notice_progress(enum gs_notice notice);
int gs_notice_close(enum gs_notice notice);
// Opens progress for an external single poweroff worker, preserving osdd's flag semantics.
int gs_shutdown_progress(const char *directory,const char *transaction);
#endif
