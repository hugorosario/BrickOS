#define _GNU_SOURCE
#include "history.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int name_valid(const char *name, size_t length) {
    if (!length || length >= GS_SYSTEM_NAME || (length == 1 && *name == '.') ||
        (length == 2 && name[0] == '.' && name[1] == '.')) return 0;
    for (size_t i = 0; i < length; ++i)
        if (!strchr("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._-", name[i])) return 0;
    return 1;
}
int gs_excluded_load(struct gs_store *s, struct gs_excluded *excluded) {
    memset(excluded, 0, sizeof(*excluded));
    int fd = openat(s->directory, GS_EXCLUDED_FILE, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return errno == ENOENT ? 0 : -1;
    char text[8192]; struct stat st; ssize_t n = -1;
    if (!fstat(fd, &st) && S_ISREG(st.st_mode)) n = read(fd, text, sizeof(text) - 1);
    close(fd);
    if (n < 0) return -1;
    text[n] = 0;
    for (char *line = text; *line && excluded->count < GS_EXCLUDED_MAX;) {
        size_t length = strcspn(line, "\n");
        char *next = line + length + (line[length] == '\n');
        if (length && line[length - 1] == '\r') --length;
        if (name_valid(line, length)) {
            memcpy(excluded->names[excluded->count], line, length);
            excluded->names[excluded->count++][length] = 0;
        }
        line = next;
    }
    return 0;
}
int gs_excluded_entry(const struct gs_store *s, const struct gs_excluded *excluded, const struct gs_entry *e) {
    size_t root = strlen(s->sd);
    const char *launcher = e->identity.launcher;
    if (!excluded->count || strncmp(launcher, s->sd, root) || strncmp(launcher + root, "/Emus/", 6)) return 0;
    const char *system = launcher + root + 6, *slash = strchr(system, '/');
    if (!slash) return 0;
    for (size_t i = 0; i < excluded->count; ++i)
        if (strlen(excluded->names[i]) == (size_t)(slash - system) && !strncmp(excluded->names[i], system, (size_t)(slash - system)))
            return 1;
    return 0;
}
int gs_history_shown(const struct gs_store *s, const struct gs_excluded *excluded, const struct gs_entry *e) {
    return !e->hidden && *e->state && !gs_excluded_entry(s, excluded, e);
}
// A card that leaves: hidden, without its checkpoint. A later checkpoint of the game brings it back.
static void drop(struct gs_entry *e) {
    e->hidden = 1; e->state[0] = e->thumbnail[0] = e->transaction[0] = 0; e->state_hash = e->thumbnail_hash = 0;
}

struct trim { struct gs_store *store; struct gs_excluded excluded; unsigned limit; size_t removed; };
static int trim_edit(struct gs_index *index, void *context) {
    struct trim *t = context;
    t->removed = 0;
    // The newest first: the order of each card's last play, as GameSwitcher lists them.
    size_t *at = malloc((index->count ? index->count : 1) * sizeof(*at)), shown = 0;
    if (!at) return -1;
    for (size_t i = 0; i < index->count; ++i)
        if (gs_history_shown(t->store, &t->excluded, &index->entries[i])) at[shown++] = i;
    for (size_t i = 1; i < shown; ++i)
        for (size_t j = i; j > 0 && index->entries[at[j]].order > index->entries[at[j - 1]].order; --j) {
            size_t swap = at[j]; at[j] = at[j - 1]; at[j - 1] = swap;
        }
    for (size_t i = t->limit; i < shown; ++i) { drop(&index->entries[at[i]]); ++t->removed; }
    free(at); return t->removed > 0;
}
int gs_history_trim(struct gs_store *s, unsigned limit, size_t *removed) {
    if (removed) *removed = 0;
    if (!limit) return 0;
    struct trim *t = calloc(1, sizeof(*t));
    if (!t) return -1;
    t->store = s; t->limit = limit;
    int result = gs_excluded_load(s, &t->excluded);
    if (!result) result = gs_store_rewrite(s, trim_edit, t, 1);
    if (!result && removed) *removed = t->removed;
    free(t); return result;
}

static int clear_edit(struct gs_index *index, void *context) {
    size_t *cleared = context; int changed = 0;
    *cleared = 0;
    for (size_t i = 0; i < index->count; ++i) {
        struct gs_entry *e = &index->entries[i];
        if (!e->hidden && *e->state) ++*cleared;
        if (e->hidden && !*e->state && !*e->thumbnail) continue;
        drop(e); changed = 1;
    }
    return changed;
}
int gs_history_clear(struct gs_store *s, size_t *cleared) {
    size_t count = 0;
    int result = gs_store_rewrite(s, clear_edit, &count, 1);
    if (!result) result = gs_store_resume_remove(s);
    if (cleared) *cleared = result ? 0 : count;
    return result;
}

static int reset_edit(struct gs_index *index, void *context) {
    size_t *reset = context;
    *reset = 0;
    for (size_t i = 0; i < index->count; ++i)
        if (index->entries[i].play_ms) { index->entries[i].play_ms = 0; ++*reset; }
    return *reset > 0;
}
int gs_history_reset_time(struct gs_store *s, size_t *reset) {
    size_t count = 0;
    int result = gs_store_rewrite(s, reset_edit, &count, 0);
    if (reset) *reset = result ? 0 : count;
    return result;
}

// A checkpoint or thumbnail file name: 32 hex digits and .state or .bmp.
static int asset(const char *name) {
    size_t n = strlen(name);
    return n > 32 && strspn(name, "0123456789abcdef") == 32 && (!strcmp(name + 32, ".state") || !strcmp(name + 32, ".bmp"));
}
int gs_history_info(struct gs_store *s, struct gs_history_info *info) {
    memset(info, 0, sizeof(*info));
    struct gs_excluded excluded; struct gs_index index;
    if (gs_excluded_load(s, &excluded) || gs_store_load(s, &index)) return -1;
    for (size_t i = 0; i < index.count; ++i) {
        uint64_t t = index.entries[i].play_ms;
        info->play_ms = UINT64_MAX - info->play_ms < t ? UINT64_MAX : info->play_ms + t;
        if (gs_history_shown(s, &excluded, &index.entries[i])) ++info->games;
    }
    gs_index_close(&index);
    int fd = openat(s->directory, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    DIR *directory = fd < 0 ? NULL : fdopendir(fd);
    if (!directory) { if (fd >= 0) close(fd); return -1; }
    struct dirent *file; struct stat st;
    while ((file = readdir(directory)))
        if (asset(file->d_name) && !fstatat(s->directory, file->d_name, &st, AT_SYMLINK_NOFOLLOW) && S_ISREG(st.st_mode))
            info->bytes += (uint64_t)st.st_size;
    closedir(directory); return 0;
}
