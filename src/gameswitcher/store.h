#ifndef BRICKOS_GAMESWITCHER_STORE_H
#define BRICKOS_GAMESWITCHER_STORE_H
#include <stddef.h>
#include <stdint.h>

#define GS_PATH 1024
#define GS_ID 33
#define GS_ASSET 48
struct gs_identity {
    char rom[GS_PATH], launcher[GS_PATH], core[GS_PATH], executable[GS_PATH];
};
struct gs_entry {
    struct gs_identity identity;
    char id[GS_ID], transaction[GS_ID], state[GS_ASSET], thumbnail[GS_ASSET], time_session[GS_ID];
    uint64_t order, play_ms, state_hash, thumbnail_hash, session_ms;
    int hidden;
};
struct gs_index { struct gs_entry *entries; size_t count; uint64_t sequence; };
struct gs_store { int directory, lock; char sd[GS_PATH]; };
struct gs_stage {
    int directory, lock;
    struct gs_identity identity;
    char state[256], thumbnail[256], transaction[GS_ID];
    uint64_t deadline_ms;
};
struct gs_receipt { char transaction[GS_ID]; int complete; };
// The checkpoint a game-aware shutdown saved, which the next boot resumes: its entry and the
// transaction that published it.
struct gs_resume { char id[GS_ID], transaction[GS_ID]; };

// Canonicalize only eligible SD ROMs, Emus launchers, cores, and ra64.trimui.
// Returns -1 on missing/nonregular paths, escape from these roots, or standalone executable.
int gs_identity_make(const char *sd, const char *rom, const char *launcher,
                     const char *core, const char *executable, struct gs_identity *identity);
int gs_store_open(struct gs_store *store, const char *sd);
// A single validated child of BrickOS; permits isolated integration history.
int gs_store_open_namespace(struct gs_store *store, const char *sd, const char *name);
int gs_store_name_valid(const char *name);
void gs_store_close(struct gs_store *store);
int gs_store_load(struct gs_store *store, struct gs_index *index);
void gs_index_close(struct gs_index *index);
// Upsert canonical ROM identity, preserving its card/checkpoint/time and moving it to front.
// Core changes invalidate the old resume target without deleting its generation.
int gs_store_played(struct gs_store *store, const struct gs_identity *identity, char id[GS_ID]);
int gs_store_hide(struct gs_store *store, const char *id);
// Trusted live coordinator only. Cumulative active milliseconds for this session,
// idempotent for retries; never changes ordering, hidden state or checkpoint assets.
int gs_store_time(struct gs_store *store, const char *id, const char *session, uint64_t cumulative_ms);
int gs_entry_resumable(struct gs_store *store, const struct gs_entry *entry);
// Verified matching BMP, rewound FD owned by caller, or -1 for placeholder.
int gs_entry_thumbnail(struct gs_store *store, const struct gs_entry *entry);
// Slot-99 targets must be absent and in this ROM's core-state folder. Pins their parent FD.
int gs_stage_begin(struct gs_store *store, const struct gs_identity *identity,
                   const char *state, const char *thumbnail, struct gs_stage *stage);
void gs_stage_close(struct gs_stage *stage);
int gs_stage_state_ready(struct gs_stage *stage);
// Revalidates a prepared slot transferred by the owning bridge: exact pinned
// directory/lock inode and expected slot names, before coordinator publication.
int gs_stage_received(struct gs_store *store,const struct gs_identity *identity,struct gs_stage *stage);
// Copies a revalidated committed state into an already reserved absent slot 99.
// Returns the exact MEM payload for a deferred core-consumption observer.
int gs_stage_load(struct gs_store *store, const struct gs_entry *entry,
                  struct gs_stage *stage, unsigned char **memory, size_t *bytes);
// A transaction-matching producer receipt and complete new state/BMP are required.
// Immutable assets are flushed before an immutable index snapshot and CURRENT publish.
// Keeps current/prior index snapshots and their assets. Errors retain old checkpoints.
// History removal never collects state files; only a later successful checkpoint does.
int gs_store_publish(struct gs_store *store, const struct gs_identity *identity,
                     struct gs_stage *stage, const struct gs_receipt *receipt);
// The resume record, RESUME in the store folder: one line with the entry id and the transaction.
// Writing goes through a temporary file, fsync, rename and a directory fsync, as the index does.
int gs_store_resume_write(struct gs_store *store, const struct gs_resume *resume);
// Reads the record and removes it durably: 1 with a valid record, 0 without one, -1 when it
// cannot be read or removed. A record that does not parse is removed, counts as none, and sets
// *malformed when malformed is not NULL.
int gs_store_resume_take(struct gs_store *store, struct gs_resume *resume, int *malformed);
// Removes the record, as when the shutdown that wrote it failed: 0 once it is gone.
int gs_store_resume_remove(struct gs_store *store);
// Loads the index under the exclusive lock and hands it, in file order, to edit, which returns 1
// when it changed it, 0 when not, and -1 with errno set to stop. A change is saved. With collect,
// it is saved twice: the prior snapshot then no longer refers to the state and thumbnail files
// the edit dropped, and those files are deleted.
int gs_store_rewrite(struct gs_store *store, int (*edit)(struct gs_index *index, void *context),
                     void *context, int collect);
#endif
