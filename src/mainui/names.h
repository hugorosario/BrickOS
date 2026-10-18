#ifndef BRICKOS_MAINUI_NAMES_H
#define BRICKOS_MAINUI_NAMES_H

// The installed stock dictionary owns returned strings until close. Tests may
// substitute lookup with a synthetic dictionary and leave handle NULL.
struct mainui_names {
    void *handle;
    const char *(*lookup)(const char *language, const char *basename);
};
// Loads only <root>/usr/trimui/lib/libgamename.so. Returns 1 when usable,
// 0 when absent/incompatible, -1 for invalid arguments/path length.
int mainui_names_open(struct mainui_names *names, const char *root);
void mainui_names_close(struct mainui_names *names);
// Stock lookup: copy at most 127 filename bytes, then remove the last extension.
// NULL means no dictionary/name. The caller must not free a returned string.
const char *mainui_names_get(const struct mainui_names *names, const char *language, const char *filename);
// SQL/history labels are already basenames: preserve them exactly for dictionary lookup.
const char *mainui_names_label(const struct mainui_names *names, const char *language, const char *basename);

#endif
