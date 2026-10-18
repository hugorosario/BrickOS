#ifndef BRICKOS_UNINSTALL_MODEL_H
#define BRICKOS_UNINSTALL_MODEL_H

#include <stddef.h>

// The Uninstall BrickOS app's screen: which screen shows, the focused choice, the keys, the
// language and every text, without SDL, so host tests can drive it. The app opens on the
// confirmation screen with Cancel focused. Left and right move the focus, A activates the
// focused choice and B cancels. Activating Uninstall asks the caller to stage the uninstall
// start script (stage.h); the caller then shows the restarting screen, or the error screen when
// staging or the restart failed, where A or B returns to MainUI. Device paths are read below a
// root, "" on the device.

#define UNINSTALL_SYSTEM_JSON "/mnt/UDISK/system.json"
#define UNINSTALL_RESULT_FILE "/mnt/SDCARD/BrickOS/uninstall-result"
#define UNINSTALL_REASON_BYTES 512

enum uninstall_screen { UNINSTALL_SCREEN_CONFIRM, UNINSTALL_SCREEN_ERROR, UNINSTALL_SCREEN_RESTARTING };
enum uninstall_choice { UNINSTALL_CHOICE_CANCEL, UNINSTALL_CHOICE_UNINSTALL };
enum uninstall_key { UNINSTALL_KEY_LEFT, UNINSTALL_KEY_RIGHT, UNINSTALL_KEY_A, UNINSTALL_KEY_B };
// What a key asks the caller to do: nothing more than redraw, exit to MainUI with status 0, or
// stage the uninstall and restart.
enum uninstall_action { UNINSTALL_ACTION_NONE, UNINSTALL_ACTION_EXIT, UNINSTALL_ACTION_UNINSTALL };
enum uninstall_language { UNINSTALL_ENGLISH, UNINSTALL_PORTUGUESE };
// Why staging or the restart failed. Every error but RESTART leaves the card as it was.
enum uninstall_error {
    UNINSTALL_ERROR_NONE,
    UNINSTALL_ERROR_SCRIPT_MISSING,  // the app's uninstall.sh cannot be read
    UNINSTALL_ERROR_CARD_FULL,
    UNINSTALL_ERROR_READ_ONLY,
    UNINSTALL_ERROR_WRITE,           // any other write failure
    UNINSTALL_ERROR_RESTART,         // staged, but the reboot command failed
};

enum uninstall_text {
    // The confirmation screen.
    UNINSTALL_TEXT_TITLE,
    UNINSTALL_TEXT_NOTICE,            // the last uninstall did not finish: "%s" is its reason
    UNINSTALL_TEXT_NOTICE_NO_REASON,
    UNINSTALL_TEXT_REMOVES,
    UNINSTALL_TEXT_REMOVES_COMPONENTS,
    UNINSTALL_TEXT_REMOVES_FOLDER,
    UNINSTALL_TEXT_KEEPS,
    UNINSTALL_TEXT_KEEPS_DATA,
    UNINSTALL_TEXT_KEEPS_IMAGES,
    UNINSTALL_TEXT_RESTART,
    UNINSTALL_TEXT_CANCEL,
    UNINSTALL_TEXT_UNINSTALL,
    UNINSTALL_TEXT_HINT,
    // The error screen.
    UNINSTALL_TEXT_ERROR_TITLE,
    UNINSTALL_TEXT_ERROR_SCRIPT_MISSING,
    UNINSTALL_TEXT_ERROR_CARD_FULL,
    UNINSTALL_TEXT_ERROR_READ_ONLY,
    UNINSTALL_TEXT_ERROR_WRITE,
    UNINSTALL_TEXT_ERROR_RESTART,
    UNINSTALL_TEXT_ERROR_UNCHANGED,   // after a staging error
    UNINSTALL_TEXT_ERROR_NEXT_START,  // after a restart error: the script stays staged
    UNINSTALL_TEXT_BACK,
    // The restarting screen.
    UNINSTALL_TEXT_RESTARTING,
    UNINSTALL_TEXT_RESTARTING_DETAIL,
    UNINSTALL_TEXT_COUNT
};

struct uninstall_model {
    enum uninstall_screen screen;
    enum uninstall_choice focus;
    enum uninstall_language language;
    int previous_failed;                  // BrickOS/uninstall-result says the last run failed
    char reason[UNINSTALL_REASON_BYTES];  // its reason, one line, possibly empty
    enum uninstall_error error;           // on the error screen
};

// The confirmation screen with Cancel focused, in a language, without a notice.
void uninstall_model_init(struct uninstall_model *m, enum uninstall_language language);
// The model as the app opens below root: the language from system.json and the notice from
// BrickOS/uninstall-result.
void uninstall_model_load(struct uninstall_model *m, const char *root);

// Portuguese for a Portuguese language file (pt.lang, or pt_BR.lang and the like), English for
// every other one, an empty name included.
enum uninstall_language uninstall_language_of(const char *language_file);
// The "language" value of system.json below root. Returns 0, or -1 (out is then empty).
int uninstall_read_language(const char *root, char *out, size_t size);
// Reads BrickOS/uninstall-result below root. Returns 1 when it says result=failed, with its
// reason in reason (possibly empty), else 0.
int uninstall_read_result(const char *root, char *reason, size_t size);

enum uninstall_action uninstall_model_key(struct uninstall_model *m, enum uninstall_key key);
// The script is staged: the restarting screen.
void uninstall_model_staged(struct uninstall_model *m);
// Staging or the restart failed: the error screen.
void uninstall_model_failed(struct uninstall_model *m, enum uninstall_error error);

// A text in the model's language.
const char *uninstall_text(const struct uninstall_model *m, enum uninstall_text text);
// The notice line, with the reason, in the model's language; empty without a failed run.
void uninstall_notice(const struct uninstall_model *m, char *out, size_t size);
// The error screen's message for the model's error.
const char *uninstall_error_text(const struct uninstall_model *m);

#endif
