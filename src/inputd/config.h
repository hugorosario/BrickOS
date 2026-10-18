#ifndef BRICKOS_INPUTD_CONFIG_H
#define BRICKOS_INPUTD_CONFIG_H

#include <stddef.h>

#include "profile.h"

// The version-one input profile. The daemon only reads the file; without it the stock mapping
// applies. Field names match case-insensitively, unknown fields are errors, a later duplicate
// key replaces an earlier one, and every profile is validated, not only the active one.
#define INPUTD_CONFIG_PATH "/mnt/SDCARD/BrickOS/inputd/inputd.json"
#define INPUTD_CONFIG_MAX_PROFILES 16
#define INPUTD_CONFIG_NAME 64

struct inputd_dest {
    int type; // INPUTD_EV_KEY, INPUTD_EV_ABS, INPUTD_EV_SW, or 0 for "disabled"
    int code;
    int value;
};

struct inputd_config {
    char active_profile[INPUTD_CONFIG_NAME];
    int profiles;
    // Per source of inputd_sources: whether the active profile binds it, and to what.
    int bound[INPUTD_SOURCES];
    struct inputd_dest dest[INPUTD_SOURCES];
};

// The stock mapping: no profile, no bindings.
void inputd_config_stock(struct inputd_config *config);
// Returns 0, or -1 with an actionable message in error.
int inputd_config_parse(struct inputd_config *config, const char *text, size_t length, char *error, size_t size);
// Returns 1 when the file was loaded, 0 when it does not exist (stock mapping), -1 when invalid.
int inputd_config_load(struct inputd_config *config, const char *path, char *error, size_t size);
// Resolves a destination alias such as KEY_VOLUMEDOWN or HAT0X_LEFT. Returns 0 or -1.
int inputd_config_resolve(const char *alias, struct inputd_dest *dest);
// The source index for a profile source name, or -1.
int inputd_config_source(const char *name);

// The whole file, for writers: every profile in file order, with each bound source's alias as
// written. Parsing validates exactly as inputd_config_parse does, so a document that parses
// is one the daemon loads.
struct inputd_config_profile {
    char name[INPUTD_CONFIG_NAME];
    int bound[INPUTD_SOURCES];
    char alias[INPUTD_SOURCES][INPUTD_CONFIG_NAME];
};

struct inputd_config_doc {
    char active_profile[INPUTD_CONFIG_NAME];
    int count;
    struct inputd_config_profile profiles[INPUTD_CONFIG_MAX_PROFILES];
};

// Returns 0, or -1 with the same message inputd_config_parse gives.
int inputd_config_doc_parse(struct inputd_config_doc *doc, const char *text, size_t length, char *error, size_t size);
// Returns 1 when the file was read, 0 when it does not exist, -1 when unreadable or invalid.
int inputd_config_doc_load(struct inputd_config_doc *doc, const char *path, char *error, size_t size);
// A single active "brickos" profile without bindings.
void inputd_config_doc_new(struct inputd_config_doc *doc);
// Canonical version-one JSON: version, device, active_profile, then profiles in order with
// their bindings in source order, lower-case field names and two-space indents. Returns the
// length, or -1 when it does not fit in size bytes with its terminator.
int inputd_config_doc_format(const struct inputd_config_doc *doc, char *out, size_t size);
// The active profile's alias for a source, or NULL when it is unbound.
const char *inputd_config_doc_get(const struct inputd_config_doc *doc, int source);
// Binds or unbinds one source of the active profile. Set returns -1 for an unknown alias.
int inputd_config_doc_set(struct inputd_config_doc *doc, int source, const char *alias);
void inputd_config_doc_unset(struct inputd_config_doc *doc, int source);

// What the daemon last loaded, to notice a change: the file's presence, inode, size and
// modification time in nanoseconds. A rename replacement changes the inode even within the
// same second, and on a card with coarse timestamps.
struct inputd_config_stamp {
    int present;
    unsigned long long device, inode;
    long long size, seconds;
    long nanoseconds;
};

void inputd_config_stamp(const char *path, struct inputd_config_stamp *stamp);
int inputd_config_stamp_same(const struct inputd_config_stamp *a, const struct inputd_config_stamp *b);

#endif
