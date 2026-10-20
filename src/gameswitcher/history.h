#ifndef BRICKOS_GAMESWITCHER_HISTORY_H
#define BRICKOS_GAMESWITCHER_HISTORY_H
#include "store.h"

// The systems the player keeps out of GameSwitcher in Settings > GameSwitcher: their Emus folder
// names, one per line, in excluded-systems in the store's folder. The setup never writes it. A
// launch through their launchers takes the BrickOS menu, and their cards keep their checkpoints
// but do not show, resume at boot, take a double tap or count toward the history limit.
#define GS_EXCLUDED_FILE "excluded-systems"
#define GS_EXCLUDED_MAX 64
#define GS_SYSTEM_NAME 64
struct gs_excluded { size_t count; char names[GS_EXCLUDED_MAX][GS_SYSTEM_NAME]; };
// A missing file is an empty list; lines that are not plain folder names are skipped. Returns 0,
// or -1 when the file cannot be read.
int gs_excluded_load(struct gs_store *store, struct gs_excluded *excluded);
// 1 when the entry's launcher lies in an excluded system's Emus folder.
int gs_excluded_entry(const struct gs_store *store, const struct gs_excluded *excluded, const struct gs_entry *entry);
// A card GameSwitcher shows: not hidden, with a checkpoint, and not of an excluded system.
int gs_history_shown(const struct gs_store *store, const struct gs_excluded *excluded, const struct gs_entry *entry);

// Keeps the newest `limit` cards shown, 0 being no limit. Older shown cards leave GameSwitcher
// and their checkpoint and thumbnail files are deleted; their play time stays. Excluded systems'
// cards are neither counted nor touched. *removed, when not NULL, gets the number that left.
int gs_history_trim(struct gs_store *store, unsigned limit, size_t *removed);
// Hides every card and deletes every checkpoint and thumbnail, keeping play time, and removes a
// resume record. *cleared, when not NULL, gets the number of visible cards with a checkpoint it
// hid, an excluded system's included.
int gs_history_clear(struct gs_store *store, size_t *cleared);
// Sets every game's play time to zero. *reset, when not NULL, gets the number of games changed.
int gs_history_reset_time(struct gs_store *store, size_t *reset);
// The cards shown, the bytes of the checkpoint and thumbnail files in the store's folder, and the
// play time of every game, shown or not, as GameSwitcher's total does.
struct gs_history_info { size_t games; uint64_t bytes, play_ms; };
int gs_history_info(struct gs_store *store, struct gs_history_info *info);
#endif
