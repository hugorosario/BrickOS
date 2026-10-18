#define _GNU_SOURCE
#include "store.h"
#include "../mainui/json.h"
#include <errno.h>
#include <dirent.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define GS_INDEX_LIMIT 4096
#define GS_STATE_LIMIT (64u * 1024u * 1024u)
static int fail(int error) { errno = error; return -1; }
static uint64_t now_ms(void) {
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now)) return UINT64_MAX;
    return (uint64_t)now.tv_sec * 1000 + (uint64_t)now.tv_nsec / 1000000;
}
static int child(const char *path, const char *root) {
    size_t n = strlen(root); return !strncmp(path, root, n) && path[n] == '/' && path[n + 1];
}
static int regular(int fd, struct stat *st) {
    return fd >= 0 && fstat(fd, st) == 0 && S_ISREG(st->st_mode);
}
static int read_full(int fd, void *data, size_t size) {
    unsigned char *p = data;
    while (size) { ssize_t n = read(fd, p, size); if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return fail(EIO);
        p += n; size -= (size_t)n; }
    return 0;
}
static int write_full(int fd, const void *data, size_t size) {
    const unsigned char *p = data;
    while (size) { ssize_t n = write(fd, p, size); if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return fail(EIO);
        p += n; size -= (size_t)n; }
    return 0;
}
static int token(char out[GS_ID]) {
    unsigned char bytes[16]; int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    int result = read_full(fd, bytes, sizeof(bytes)); close(fd);
    if (result) return result;
    for (size_t i = 0; i < sizeof(bytes); ++i) snprintf(out + i * 2, 3, "%02x", bytes[i]);
    return 0;
}
static int id_valid(const char *id) {
    return strlen(id) == 32 && strspn(id, "0123456789abcdef") == 32;
}
static int asset_valid(const char *asset, const char *suffix) {
    char id[GS_ID];
    if (!*asset) return 1;
    if (strlen(asset) != 32 + strlen(suffix)) return 0;
    memcpy(id, asset, 32); id[32] = 0;
    return id_valid(id) && !strcmp(asset + 32, suffix);
}
static int canonical(const char *path, char out[GS_PATH]) {
    char *resolved = realpath(path, NULL); struct stat st;
    if (!resolved) return -1;
    int ok = strlen(resolved) < GS_PATH && stat(resolved, &st) == 0 && S_ISREG(st.st_mode);
    if (ok) strcpy(out, resolved);
    free(resolved); return ok ? 0 : fail(EINVAL);
}
static int arm64_elf(const char *path, int core) {
    unsigned char header[64] = {0}; int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return 0;
    int ok = read_full(fd, header, sizeof(header)) == 0;
    close(fd);
    unsigned type = (unsigned)header[16] | (unsigned)header[17] << 8;
    return ok && !memcmp(header, "\177ELF", 4) && header[4] == 2 && header[5] == 1 &&
           header[6] == 1 && header[18] == 183 && header[19] == 0 &&
           (core ? type == 3 : type == 2 || type == 3);
}
int gs_identity_make(const char *sd, const char *rom, const char *launcher,
                     const char *core, const char *executable, struct gs_identity *out) {
    if (!sd || !rom || !launcher || !core || !executable || !out) return fail(EINVAL);
    char *root = realpath(sd, NULL); char expected[GS_PATH];
    if (!root) return -1;
    int result = canonical(rom, out->rom) || canonical(launcher, out->launcher) ||
                 canonical(core, out->core) || canonical(executable, out->executable);
    const char *tails[] = {"/Roms", "/Emus", "/RetroArch/.retroarch/cores"};
    const char *paths[] = {out->rom, out->launcher, out->core};
    for (size_t i = 0; !result && i < 3; ++i) {
        int n = snprintf(expected, sizeof(expected), "%s%s", root, tails[i]);
        if (n < 0 || n >= GS_PATH || !child(paths[i], expected)) result = 1;
    }
    if (!result) {
        int n = snprintf(expected, sizeof(expected), "%s/RetroArch/ra64.trimui", root);
        if (n < 0 || n >= GS_PATH || strcmp(out->executable, expected)) result = 1;
    }
    if (!result && (!arm64_elf(out->executable, 0) || !arm64_elf(out->core, 1))) result = 1;
    free(root); return result ? fail(EINVAL) : 0;
}
static int same_identity(const struct gs_identity *a, const struct gs_identity *b) {
    return !strcmp(a->rom, b->rom) && !strcmp(a->launcher, b->launcher) &&
           !strcmp(a->core, b->core) && !strcmp(a->executable, b->executable);
}
static int validate_identity(struct gs_store *s, const struct gs_identity *identity) {
    struct gs_identity checked;
    return gs_identity_make(s->sd, identity->rom, identity->launcher, identity->core,
                            identity->executable, &checked) == 0 && same_identity(identity, &checked) ? 0 : fail(EINVAL);
}
static int subdir(int parent, const char *name) {
    if (mkdirat(parent, name, 0755) < 0 && errno != EEXIST) return -1;
    int fd = openat(parent, name, O_DIRECTORY | O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd >= 0 && fsync(parent) < 0) { close(fd); return -1; }
    return fd;
}
int gs_store_name_valid(const char *name) {
    return name && *name && strlen(name)<64 && strcmp(name,".") && strcmp(name,"..") &&
           strspn(name,"abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._-")==strlen(name);
}
int gs_store_open(struct gs_store *s, const char *sd) {
    return gs_store_open_namespace(s,sd,"gameswitcher");
}
int gs_store_open_namespace(struct gs_store *s, const char *sd, const char *name) {
    memset(s, 0, sizeof(*s)); s->directory = s->lock = -1;
    if (!gs_store_name_valid(name)) return fail(EINVAL);
    char *root = realpath(sd, NULL);
    if (!root || strlen(root) >= sizeof(s->sd)) { free(root); return fail(EINVAL); }
    strcpy(s->sd, root); free(root);
    int parent = open(s->sd, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (parent < 0) return -1;
    int brick = subdir(parent, "BrickOS"); close(parent);
    if (brick < 0) return -1;
    s->directory = subdir(brick, name); close(brick);
    if (s->directory < 0) return -1;
    s->lock = openat(s->directory, ".lock", O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
    struct stat st;
    if (!regular(s->lock, &st)) { gs_store_close(s); return fail(EINVAL); }
    return 0;
}
void gs_store_close(struct gs_store *s) {
    if (s->lock >= 0) close(s->lock);
    if (s->directory >= 0) close(s->directory);
    s->lock = s->directory = -1;
}
void gs_index_close(struct gs_index *index) { free(index->entries); memset(index, 0, sizeof(*index)); }
static int json_string(const struct mainui_json *json, const char *key, char *out, size_t size) {
    const struct mainui_json_member *m = mainui_json_get(json, key);
    if (!m || m->type != MAINUI_JSON_STRING || m->text_length >= size ||
        strlen(m->text) != m->text_length) return -1;
    strcpy(out, m->text); return 0;
}
static int json_u64(const struct mainui_json *json, const char *key, uint64_t *out, int base) {
    char text[32], *end;
    if (json_string(json, key, text, sizeof(text)) || !*text ||
        strspn(text, base == 16 ? "0123456789abcdef" : "0123456789") != strlen(text)) return -1;
    errno = 0; unsigned long long n = strtoull(text, &end, base);
    if (errno || *end) return -1;
    *out = (uint64_t)n; return 0;
}
static int parse_line(struct mainui_json *json, const char *line) {
    if (mainui_json_parse(json, line, strlen(line))) return -1;
    for (size_t i = 0; i < json->count; ++i)
        for (size_t j = 0; j < i; ++j)
            if (!strcasecmp(json->members[i].key, json->members[j].key)) return -1;
    return 0;
}
static int load_pointer(struct gs_store *s, struct gs_index *index, const char *which) {
    memset(index, 0, sizeof(*index));
    int pointer = openat(s->directory, which, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (pointer < 0) return errno == ENOENT ? 0 : -1;
    struct stat st; char name[GS_ASSET] = {0};
    if (!regular(pointer, &st) || st.st_size != 38 || read_full(pointer, name, 38) ||
        !asset_valid(name, ".jsonl")) { close(pointer); return fail(EINVAL); }
    close(pointer);
    int fd = openat(s->directory, name, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (!regular(fd, &st) || st.st_size > 32 * 1024 * 1024) { if (fd >= 0) close(fd); return fail(EINVAL); }
    FILE *file = fdopen(fd, "r");
    struct mainui_json *json = malloc(sizeof(*json)); char line[16384], format[64];
    int result = -1;
    if (!file || !json) { if (file) fclose(file); else close(fd); free(json); return -1; }
    if (!fgets(line, sizeof(line), file) || !strchr(line, '\n') || parse_line(json, line) ||
        json_string(json, "format", format, sizeof(format)) || strcmp(format, "BrickOS GameSwitcher 1") ||
        json_u64(json, "sequence", &index->sequence, 10)) goto done;
    while (fgets(line, sizeof(line), file)) {
        struct gs_entry e = {0}; uint64_t hidden;
        if (index->count >= GS_INDEX_LIMIT || !strchr(line, '\n') || parse_line(json, line) ||
            json_string(json, "id", e.id, sizeof(e.id)) || !id_valid(e.id) ||
            json_string(json, "rom", e.identity.rom, GS_PATH) ||
            json_string(json, "launcher", e.identity.launcher, GS_PATH) ||
            json_string(json, "core", e.identity.core, GS_PATH) ||
            json_string(json, "executable", e.identity.executable, GS_PATH) ||
            json_string(json, "transaction", e.transaction, sizeof(e.transaction)) ||
            (*e.transaction && !id_valid(e.transaction)) ||
            json_string(json, "state", e.state, sizeof(e.state)) || !asset_valid(e.state, ".state") ||
            json_string(json, "thumbnail", e.thumbnail, sizeof(e.thumbnail)) || !asset_valid(e.thumbnail, ".bmp") ||
            json_u64(json, "order", &e.order, 10) || e.order > index->sequence ||
            json_u64(json, "play_ms", &e.play_ms, 10) ||
            json_u64(json, "state_hash", &e.state_hash, 16) ||
            json_u64(json, "thumbnail_hash", &e.thumbnail_hash, 16) ||
            json_u64(json, "hidden", &hidden, 10) || hidden > 1) goto done;
        // Older snapshots predate accounting. A partial/malformed extension is rejected.
        if (mainui_json_get(json, "time_session") || mainui_json_get(json, "session_ms")) {
            if (json_string(json, "time_session", e.time_session, sizeof(e.time_session)) ||
                (*e.time_session && !id_valid(e.time_session)) ||
                json_u64(json, "session_ms", &e.session_ms, 10) ||
                (!*e.time_session && e.session_ms)) goto done;
        }
        e.hidden = (int)hidden;
        for (size_t i = 0; i < index->count; ++i)
            if (!strcmp(e.id, index->entries[i].id) || !strcmp(e.identity.rom, index->entries[i].identity.rom)) goto done;
        struct gs_entry *entries = realloc(index->entries, (index->count + 1) * sizeof(e));
        if (!entries) goto done;
        index->entries = entries; index->entries[index->count++] = e;
    }
    if (ferror(file)) goto done;
    result = 0;
done:
    fclose(file); free(json);
    if (result) { gs_index_close(index); return fail(EINVAL); }
    return 0;
}
static int load_unlocked(struct gs_store *s, struct gs_index *index) {
    return load_pointer(s, index, "CURRENT");
}
static int newest_first(const void *left, const void *right) {
    const struct gs_entry *a = left, *b = right;
    if (a->order != b->order) return a->order > b->order ? -1 : 1;
    return strcmp(a->id, b->id);
}
int gs_store_load(struct gs_store *s, struct gs_index *index) {
    if (flock(s->lock, LOCK_SH)) return -1;
    int result = load_unlocked(s, index);
    if (!result && index->count > 1) qsort(index->entries, index->count, sizeof(*index->entries), newest_first);
    flock(s->lock, LOCK_UN); return result;
}
static int string_field(FILE *file, const char *key, const char *value) {
    char escaped[GS_PATH * 6 + 3];
    if (mainui_json_escape(escaped, sizeof(escaped), value, strlen(value)) < 0) return -1;
    return fprintf(file, "\"%s\":%s,", key, escaped) > 0 ? 0 : -1;
}
#ifdef GS_TESTING
extern int gs_test_fault(const char *phase);
#define FAULT(phase) gs_test_fault(phase)
#else
#define FAULT(phase) 0
#endif
static int read_pointer(struct gs_store *s, const char *which, char name[GS_ASSET]) {
    name[0] = 0;
    int fd = openat(s->directory, which, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return errno == ENOENT ? 0 : -1;
    struct stat st;
    int result = regular(fd, &st) && st.st_size == 38 && read_full(fd, name, 38) == 0 ? 0 : -1;
    close(fd); name[38] = 0;
    return !result && asset_valid(name, ".jsonl") ? 0 : fail(EINVAL);
}
static int keep_previous(struct gs_store *s, const char *nonce) {
    char old[GS_ASSET], temp[GS_ASSET];
    if (read_pointer(s, "CURRENT", old)) return -1;
    if (!*old) return 0;
    snprintf(temp, sizeof(temp), "%s.previous", nonce);
    int fd = openat(s->directory, temp, O_CREAT | O_EXCL | O_WRONLY | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0) return -1;
    int result = write_full(fd, old, strlen(old));
    if (!result && fsync(fd)) result = -1;
    if (close(fd)) result = -1;
    if (!result && renameat(s->directory, temp, s->directory, "PREVIOUS")) result = -1;
    if (!result && (fsync(s->directory) || syncfs(s->directory))) result = -1;
    if (result) unlinkat(s->directory, temp, 0);
    return result;
}
static int referenced(const struct gs_index *index, const char *name) {
    for (size_t i = 0; i < index->count; ++i)
        if (!strcmp(index->entries[i].state, name) || !strcmp(index->entries[i].thumbnail, name)) return 1;
    return 0;
}
static void collect_unlocked(struct gs_store *s, int assets) {
    char current[GS_ASSET], previous[GS_ASSET];
    struct gs_index a = {0}, b = {0};
    if (read_pointer(s, "CURRENT", current) || read_pointer(s, "PREVIOUS", previous) ||
        load_pointer(s, &a, "CURRENT") || load_pointer(s, &b, "PREVIOUS")) goto done;
    int fd = openat(s->directory, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0) goto done;
    DIR *directory = fdopendir(fd);
    if (!directory) { close(fd); goto done; }
    struct dirent *file;
    while ((file = readdir(directory))) {
        const char *name = file->d_name;
        if (!*name || !strcmp(name, current) || !strcmp(name, previous) ||
            referenced(&a, name) || referenced(&b, name)) continue;
        if (!assets && (asset_valid(name, ".state") || asset_valid(name, ".bmp"))) continue;
        if (!asset_valid(name, ".state") && !asset_valid(name, ".bmp") &&
            !asset_valid(name, ".jsonl") && !asset_valid(name, ".current") && !asset_valid(name, ".previous")) continue;
        struct stat st;
        if (!fstatat(s->directory, name, &st, AT_SYMLINK_NOFOLLOW) && S_ISREG(st.st_mode))
            (void)unlinkat(s->directory, name, 0);
    }
    closedir(directory); (void)fsync(s->directory);
done:
    gs_index_close(&a); gs_index_close(&b);
}
static int save_unlocked(struct gs_store *s, const struct gs_index *index, uint64_t deadline, int assets) {
    char nonce[GS_ID], name[GS_ASSET], temporary[GS_ASSET];
    if (token(nonce)) return -1;
    snprintf(name, sizeof(name), "%s.jsonl", nonce);
    snprintf(temporary, sizeof(temporary), "%s.current", nonce);
    int fd = openat(s->directory, name, O_CREAT | O_EXCL | O_WRONLY | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0) return -1;
    FILE *file = fdopen(fd, "w"); int result = -1;
    if (!file) { close(fd); return -1; }
    if (fprintf(file, "{\"format\":\"BrickOS GameSwitcher 1\",\"sequence\":\"%" PRIu64 "\"}\n", index->sequence) < 0) goto close_index;
    for (size_t i = 0; i < index->count; ++i) {
        const struct gs_entry *e = &index->entries[i];
        if (fputc('{', file) == EOF || string_field(file, "id", e->id) ||
            string_field(file, "rom", e->identity.rom) || string_field(file, "launcher", e->identity.launcher) ||
            string_field(file, "core", e->identity.core) || string_field(file, "executable", e->identity.executable) ||
            string_field(file, "transaction", e->transaction) || string_field(file, "state", e->state) ||
            string_field(file, "thumbnail", e->thumbnail) || string_field(file, "time_session", e->time_session) ||
            fprintf(file, "\"order\":\"%" PRIu64 "\",\"play_ms\":\"%" PRIu64 "\","
                    "\"state_hash\":\"%" PRIx64 "\",\"thumbnail_hash\":\"%" PRIx64 "\",\"hidden\":\"%d\","
                    "\"session_ms\":\"%" PRIu64 "\"}\n",
                    e->order, e->play_ms, e->state_hash, e->thumbnail_hash, e->hidden, e->session_ms) < 0) goto close_index;
    }
    if (fflush(file) || fsync(fd)) goto close_index;
    result = 0;
close_index:
    if (fclose(file)) result = -1;
    if (result || fsync(s->directory) || syncfs(s->directory) || FAULT("index-flushed")) return -1;
    if (keep_previous(s, nonce)) return -1;
    fd = openat(s->directory, temporary, O_CREAT | O_EXCL | O_WRONLY | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0) return -1;
    result = write_full(fd, name, strlen(name));
    if (!result && fsync(fd)) result = -1;
    if (close(fd)) result = -1;
    if (!result && ((deadline && now_ms() >= deadline) || FAULT("before-publication") ||
                    renameat(s->directory, temporary, s->directory, "CURRENT"))) result = -1;
    if (result) unlinkat(s->directory, temporary, 0);
    // The immutable prior snapshot remains recoverable even on a post-rename flush failure.
    if (!result && (fsync(s->directory) || syncfs(s->directory))) result = -1;
    if (!result) collect_unlocked(s, assets);
    return result;
}
static struct gs_entry *upsert(struct gs_index *index, const struct gs_identity *identity) {
    if (index->sequence == UINT64_MAX) { errno = EOVERFLOW; return NULL; }
    struct gs_entry *e = NULL;
    for (size_t i = 0; i < index->count; ++i)
        if (!strcmp(index->entries[i].identity.rom, identity->rom)) { e = &index->entries[i]; break; }
    if (!e) {
        if (index->count >= GS_INDEX_LIMIT) { errno = ENOSPC; return NULL; }
        struct gs_entry *entries = realloc(index->entries, (index->count + 1) * sizeof(*e));
        if (!entries) return NULL;
        index->entries = entries; e = &entries[index->count++]; memset(e, 0, sizeof(*e));
        if (token(e->id)) return NULL;
    } else if (strcmp(e->identity.core, identity->core)) {
        e->state[0] = e->thumbnail[0] = e->transaction[0] = 0;
        e->state_hash = e->thumbnail_hash = 0;
    }
    e->identity = *identity; e->hidden = 0; e->order = ++index->sequence; return e;
}
int gs_store_played(struct gs_store *s, const struct gs_identity *identity, char id[GS_ID]) {
    if (validate_identity(s, identity) || flock(s->lock, LOCK_EX)) return -1;
    struct gs_index index; int result = load_unlocked(s, &index);
    if (!result) {
        struct gs_entry *e = upsert(&index, identity);
        result = e ? save_unlocked(s, &index, 0, 0) : -1;
        if (!result && id) strcpy(id, e->id);
    }
    gs_index_close(&index); flock(s->lock, LOCK_UN); return result;
}
#define RESUME_BYTES (32 + 1 + 32 + 1)
int gs_store_resume_write(struct gs_store *s, const struct gs_resume *r) {
    if (!id_valid(r->id) || !id_valid(r->transaction)) return fail(EINVAL);
    char nonce[GS_ID], temp[GS_ASSET], line[RESUME_BYTES + 1];
    if (token(nonce)) return -1;
    snprintf(temp, sizeof(temp), "%s.resume", nonce);
    snprintf(line, sizeof(line), "%s %s\n", r->id, r->transaction);
    if (flock(s->lock, LOCK_EX)) return -1;
    int fd = openat(s->directory, temp, O_CREAT | O_EXCL | O_WRONLY | O_CLOEXEC | O_NOFOLLOW, 0600);
    int result = fd < 0 ? -1 : write_full(fd, line, RESUME_BYTES);
    if (!result && fsync(fd)) result = -1;
    if (fd >= 0 && close(fd)) result = -1;
    if (!result && renameat(s->directory, temp, s->directory, "RESUME")) result = -1;
    if (!result && (fsync(s->directory) || syncfs(s->directory))) result = -1;
    if (result && fd >= 0) unlinkat(s->directory, temp, 0);
    flock(s->lock, LOCK_UN); return result;
}
static int resume_unlink(struct gs_store *s) {
    if (unlinkat(s->directory, "RESUME", 0) && errno != ENOENT) return -1;
    return fsync(s->directory) || syncfs(s->directory) ? -1 : 0;
}
int gs_store_resume_take(struct gs_store *s, struct gs_resume *r, int *malformed) {
    memset(r, 0, sizeof(*r));
    if (malformed) *malformed = 0;
    if (flock(s->lock, LOCK_EX)) return -1;
    int fd = openat(s->directory, "RESUME", O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) { int absent = errno == ENOENT; flock(s->lock, LOCK_UN); return absent ? 0 : -1; }
    struct stat st; char line[RESUME_BYTES + 1] = {0};
    int valid = regular(fd, &st) && st.st_size == RESUME_BYTES && !read_full(fd, line, RESUME_BYTES) &&
                line[32] == ' ' && line[RESUME_BYTES - 1] == '\n';
    close(fd);
    if (valid) {
        memcpy(r->id, line, 32); memcpy(r->transaction, line + 33, 32);
        valid = id_valid(r->id) && id_valid(r->transaction);
    }
    if (!valid) { memset(r, 0, sizeof(*r)); if (malformed) *malformed = 1; }
    // The record acts once: it is gone before the boot resumes anything.
    int result = resume_unlink(s);
    flock(s->lock, LOCK_UN);
    return result ? -1 : valid;
}
int gs_store_resume_remove(struct gs_store *s) {
    if (flock(s->lock, LOCK_EX)) return -1;
    int result = resume_unlink(s);
    flock(s->lock, LOCK_UN); return result;
}
int gs_store_hide(struct gs_store *s, const char *id) {
    if (!id_valid(id)) return fail(EINVAL);
    if (flock(s->lock, LOCK_EX)) return -1;
    struct gs_index index; int result = load_unlocked(s, &index), found = 0;
    if (!result) for (size_t i = 0; i < index.count; ++i)
        if (!strcmp(index.entries[i].id, id)) { index.entries[i].hidden = 1; found = 1; break; }
    if (!result) result = found ? save_unlocked(s, &index, 0, 0) : fail(ENOENT);
    gs_index_close(&index); flock(s->lock, LOCK_UN); return result;
}
int gs_store_time(struct gs_store *s, const char *id, const char *session, uint64_t cumulative) {
    if (!id_valid(id) || !id_valid(session)) return fail(EINVAL);
    if (flock(s->lock, LOCK_EX)) return -1;
    struct gs_index index;
    int result = load_unlocked(s, &index), found = 0;
    if (!result) for (size_t i = 0; i < index.count; ++i) {
        struct gs_entry *e = &index.entries[i];
        if (strcmp(e->id, id)) continue;
        found = 1;
        uint64_t previous = !strcmp(e->time_session, session) ? e->session_ms : 0;
        if (cumulative < previous || UINT64_MAX - e->play_ms < cumulative - previous) { result = fail(EOVERFLOW); break; }
        if (!strcmp(e->time_session, session) && cumulative == previous) break;
        e->play_ms += cumulative - previous; e->session_ms = cumulative;
        strcpy(e->time_session, session);
        result = save_unlocked(s, &index, 0, 0); break;
    }
    if (!result && !found) result = fail(ENOENT);
    gs_index_close(&index); flock(s->lock, LOCK_UN); return result;
}
static uint32_t le32(const unsigned char *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static int checked_file(int fd, int bitmap, uint64_t *hash) {
    struct stat st; unsigned char head[54];
    if (!regular(fd, &st) || st.st_size < (bitmap ? 54 : 24) || st.st_size > GS_STATE_LIMIT ||
        pread(fd, head, bitmap ? 54 : 8, 0) != (bitmap ? 54 : 8)) return fail(EINVAL);
    if (bitmap) {
        uint32_t width = le32(head + 18), raw_height = le32(head + 22);
        int32_t signed_height = (int32_t)raw_height;
        uint32_t height = signed_height < 0 ? (uint32_t)(-(int64_t)signed_height) : raw_height;
        unsigned bits = (unsigned)head[28] | (unsigned)head[29] << 8;
        uint32_t offset = le32(head + 10), compression = le32(head + 30), dib = le32(head + 14);
        uint64_t row = (((uint64_t)width * bits + 31) / 32) * 4;
        if (memcmp(head, "BM", 2) || le32(head + 2) != (uint64_t)st.st_size ||
            dib < 40 || (uint64_t)dib + 14 > offset || head[26] != 1 || head[27] != 0 ||
            (bits != 24 && bits != 32) || (compression != 0 && compression != 3) ||
            !width || width > 1024 || !height || height > 768 ||
            (uint64_t)offset + row * height > (uint64_t)st.st_size) return fail(EINVAL);
    } else {
        if (memcmp(head, "RASTATE\1", 8)) return fail(EINVAL);
        uint64_t at = 8; int memories = 0, ended = 0;
        while (at + 8 <= (uint64_t)st.st_size) {
            if (pread(fd, head, 8, (off_t)at) != 8) return fail(EIO);
            uint32_t size = le32(head + 4);
            if (!memcmp(head, "END ", 4)) { ended = !size && at + 8 == (uint64_t)st.st_size; break; }
            uint64_t aligned = ((uint64_t)size + 7) & ~(uint64_t)7;
            if (aligned > (uint64_t)st.st_size - at - 8) return fail(EINVAL);
            if (!memcmp(head, "MEM ", 4)) { if (!size) return fail(EINVAL); ++memories; }
            at += 8 + aligned;
        }
        if (memories != 1 || !ended) return fail(EINVAL);
    }
    if (lseek(fd, 0, SEEK_SET) < 0) return -1;
    uint64_t value = UINT64_C(14695981039346656037); unsigned char bytes[32768]; ssize_t n;
    while ((n = read(fd, bytes, sizeof(bytes))) != 0) {
        if (n < 0) { if (errno == EINTR) continue; return -1; }
        for (ssize_t i = 0; i < n; ++i) { value ^= bytes[i]; value *= UINT64_C(1099511628211); }
    }
    *hash = value; return 0;
}
int gs_entry_thumbnail(struct gs_store *s, const struct gs_entry *entry) {
    if (!entry || !*entry->thumbnail || !asset_valid(entry->thumbnail, ".bmp")) return fail(EINVAL);
    int fd = openat(s->directory, entry->thumbnail, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    uint64_t hash = 0;
    if (fd < 0) return -1;
    if (checked_file(fd, 1, &hash) || hash != entry->thumbnail_hash || lseek(fd, 0, SEEK_SET) < 0) {
        close(fd); return fail(EINVAL);
    }
    return fd;
}
int gs_entry_resumable(struct gs_store *s, const struct gs_entry *entry) {
    if (!entry || entry->hidden || !*entry->state || !asset_valid(entry->state, ".state") ||
        validate_identity(s, &entry->identity)) return fail(EINVAL);
    int fd = openat(s->directory, entry->state, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    uint64_t hash = 0; int result = checked_file(fd, 0, &hash);
    if (fd >= 0) close(fd);
    return !result && hash == entry->state_hash ? 0 : fail(EINVAL);
}
int gs_stage_begin(struct gs_store *s, const struct gs_identity *identity,
                   const char *state, const char *thumbnail, struct gs_stage *stage) {
    memset(stage, 0, sizeof(*stage)); stage->directory = stage->lock = -1;
    if (validate_identity(s, identity) || !state || !thumbnail) return fail(EINVAL);
    const char *slash = strrchr(identity->rom, '/'), *core = strrchr(identity->core, '/');
    const char *ss = strrchr(state, '/'), *ts = strrchr(thumbnail, '/');
    if (!slash || !core || !ss || !ts) return fail(EINVAL);
    const char *rom_ext = strrchr(slash + 1, '.'), *core_ext = strrchr(core + 1, '.');
    size_t rom_n = rom_ext ? (size_t)(rom_ext - slash - 1) : strlen(slash + 1);
    size_t core_n = core_ext ? (size_t)(core_ext - core - 1) : strlen(core + 1);
    char parent[GS_PATH], state_parent[GS_PATH], thumb_parent[GS_PATH];
    int n = snprintf(parent, sizeof(parent), "%.*s/.%.*s", (int)(slash - identity->rom), identity->rom,
                     (int)core_n, core + 1);
    if (n < 0 || n >= GS_PATH || ss - state >= GS_PATH || ts - thumbnail >= GS_PATH) return fail(EINVAL);
    memcpy(state_parent, state, (size_t)(ss - state)); state_parent[ss - state] = 0;
    memcpy(thumb_parent, thumbnail, (size_t)(ts - thumbnail)); thumb_parent[ts - thumbnail] = 0;
    char *sp = realpath(state_parent, NULL), *tp = realpath(thumb_parent, NULL);
    int ok = sp && tp && !strcmp(sp, parent) && !strcmp(tp, parent); free(sp); free(tp);
    if (!ok || rom_n + 10 >= sizeof(stage->state)) return fail(EINVAL);
    snprintf(stage->state, sizeof(stage->state), "%.*s.99.state", (int)rom_n, slash + 1);
    snprintf(stage->thumbnail, sizeof(stage->thumbnail), "%.*s.99.png", (int)rom_n, slash + 1);
    if (strcmp(ss + 1, stage->state) || strcmp(ts + 1, stage->thumbnail)) return fail(EINVAL);
    // Keep a distinct open description for each transaction. Serializes callbacks as
    // well as publication, including two clients that preflight before either writes.
    stage->lock = openat(s->directory, ".staging-lock", O_CREAT | O_RDWR | O_CLOEXEC | O_NOFOLLOW, 0600);
    struct stat lock_stat;
    if (stage->lock < 0 || !regular(stage->lock, &lock_stat) || flock(stage->lock, LOCK_EX | LOCK_NB)) {
        gs_stage_close(stage); return fail(EBUSY);
    }
    stage->directory = open(parent, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (stage->directory < 0) { gs_stage_close(stage); return -1; }
    struct stat st;
    if (fstatat(stage->directory, stage->state, &st, AT_SYMLINK_NOFOLLOW) == 0 || errno != ENOENT ||
        fstatat(stage->directory, stage->thumbnail, &st, AT_SYMLINK_NOFOLLOW) == 0 || errno != ENOENT ||
        token(stage->transaction)) { gs_stage_close(stage); return fail(EEXIST); }
    stage->identity = *identity;
    uint64_t started = now_ms();
    if (started > UINT64_MAX - 15000) { gs_stage_close(stage); return fail(EIO); }
    stage->deadline_ms = started + 15000;
    return 0;
}
void gs_stage_close(struct gs_stage *stage) {
    if (stage->directory >= 0) close(stage->directory);
    if (stage->lock >= 0) close(stage->lock);
    stage->directory = stage->lock = -1;
}
int gs_stage_received(struct gs_store *s,const struct gs_identity *identity,struct gs_stage *stage) {
    if (validate_identity(s,identity) || stage->directory<0 || stage->lock<0 ||
        !id_valid(stage->transaction) || !memchr(stage->state,0,sizeof(stage->state)) ||
        !memchr(stage->thumbnail,0,sizeof(stage->thumbnail)) || now_ms()>=stage->deadline_ms ||
        stage->deadline_ms-now_ms()>15000) return fail(EINVAL);
    const char *rom=strrchr(identity->rom,'/'),*core=strrchr(identity->core,'/');
    if (!rom || !core) return fail(EINVAL);
    const char *rom_ext=strrchr(rom+1,'.'),*core_ext=strrchr(core+1,'.');
    size_t rn=rom_ext?(size_t)(rom_ext-rom-1):strlen(rom+1);
    size_t cn=core_ext?(size_t)(core_ext-core-1):strlen(core+1);
    char parent[GS_PATH],state[256],thumb[256];
    int n=snprintf(parent,sizeof(parent),"%.*s/.%.*s",(int)(rom-identity->rom),identity->rom,(int)cn,core+1);
    if (n<0 || n>=GS_PATH || rn+10>=sizeof(state)) return fail(EINVAL);
    snprintf(state,sizeof(state),"%.*s.99.state",(int)rn,rom+1);
    snprintf(thumb,sizeof(thumb),"%.*s.99.png",(int)rn,rom+1);
    struct stat expected,actual;
    if (strcmp(state,stage->state) || strcmp(thumb,stage->thumbnail) ||
        lstat(parent,&expected) || !S_ISDIR(expected.st_mode) || fstat(stage->directory,&actual) ||
        actual.st_dev!=expected.st_dev || actual.st_ino!=expected.st_ino ||
        fstatat(s->directory,".staging-lock",&expected,AT_SYMLINK_NOFOLLOW) || !S_ISREG(expected.st_mode) ||
        fstat(stage->lock,&actual) || actual.st_dev!=expected.st_dev || actual.st_ino!=expected.st_ino ||
        flock(stage->lock,LOCK_EX|LOCK_NB)) return fail(EINVAL);
    stage->identity=*identity; return gs_stage_state_ready(stage);
}
int gs_stage_state_ready(struct gs_stage *stage) {
    if (stage->directory < 0 || stage->lock < 0 || now_ms() >= stage->deadline_ms) return fail(EINVAL);
    int fd = openat(stage->directory, stage->state, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    uint64_t hash = 0;
    int result = checked_file(fd, 0, &hash);
    if (fd >= 0) close(fd);
    return result;
}
static int copy_asset(struct gs_store *s, struct gs_stage *stage, const char *source,
                      const char *destination, int bitmap, uint64_t *hash) {
    int from = openat(stage->directory, source, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (from < 0) return -1;
    struct stat before, after; int result = -1, to = -1;
    if (!regular(from, &before) || checked_file(from, bitmap, hash) || lseek(from, 0, SEEK_SET) < 0) goto done;
    to = openat(s->directory, destination, O_CREAT | O_EXCL | O_WRONLY | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (to < 0) goto done;
    unsigned char bytes[32768]; ssize_t n; uint64_t copied = UINT64_C(14695981039346656037), count = 0;
    while ((n = read(from, bytes, sizeof(bytes))) != 0) {
        if (n < 0) { if (errno == EINTR) continue; goto done; }
        count += (uint64_t)n;
        if (count > (uint64_t)before.st_size) goto done;
        if (write_full(to, bytes, (size_t)n)) goto done;
        for (ssize_t i = 0; i < n; ++i) { copied ^= bytes[i]; copied *= UINT64_C(1099511628211); }
    }
    if (count != (uint64_t)before.st_size || fstat(from, &after) || before.st_size != after.st_size ||
        before.st_mtim.tv_sec != after.st_mtim.tv_sec || before.st_mtim.tv_nsec != after.st_mtim.tv_nsec ||
        copied != *hash || fsync(to)) goto done;
    result = 0;
done:
    if (to >= 0 && close(to)) result = -1;
    close(from); return result;
}
// Moves a staged asset into the store. Both lie on the SD's one filesystem, so a rename replaces
// a second full write, the slow part on the sync-mounted card. The staged bytes are validated,
// hashed and flushed first; another filesystem falls back to a copy.
static int move_asset(struct gs_store *s, struct gs_stage *stage, const char *source,
                      const char *destination, int bitmap, uint64_t *hash) {
    int from = openat(stage->directory, source, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (from < 0) return -1;
    struct stat before, after, existing; int result = -1;
    if (!regular(from, &before) || checked_file(from, bitmap, hash) || fstat(from, &after) ||
        before.st_size != after.st_size || before.st_mtim.tv_sec != after.st_mtim.tv_sec ||
        before.st_mtim.tv_nsec != after.st_mtim.tv_nsec || fsync(from)) goto done;
    // Generation names are fresh; never replace an existing file.
    if (!fstatat(s->directory, destination, &existing, AT_SYMLINK_NOFOLLOW) || errno != ENOENT) goto done;
    if (renameat(stage->directory, source, s->directory, destination)) {
        if (errno != EXDEV) goto done;
        close(from);
        return copy_asset(s, stage, source, destination, bitmap, hash);
    }
    result = 0;
done:
    close(from); return result;
}
int gs_stage_load(struct gs_store *s, const struct gs_entry *entry,
                  struct gs_stage *stage, unsigned char **memory, size_t *bytes) {
    *memory = NULL; *bytes = 0;
    if (stage->directory < 0 || stage->lock < 0 || now_ms() >= stage->deadline_ms ||
        !same_identity(&entry->identity, &stage->identity) || gs_entry_resumable(s, entry)) return fail(EINVAL);
    int from = openat(s->directory, entry->state, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    uint64_t hash = 0; int to = -1, result = -1;
    if (from < 0) return -1;
    if (checked_file(from, 0, &hash) || hash != entry->state_hash || lseek(from, 0, SEEK_SET) < 0) goto done;
    to = openat(stage->directory, stage->state, O_CREAT | O_EXCL | O_RDWR | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (to < 0) goto done;
    unsigned char data[32768]; ssize_t n; uint64_t count = 0;
    while ((n = read(from, data, sizeof(data))) != 0) {
        if (n < 0) { if (errno == EINTR) continue; goto done; }
        count += (uint64_t)n;
        if (count > GS_STATE_LIMIT || write_full(to, data, (size_t)n)) goto done;
    }
    if (fsync(to) || fsync(stage->directory) || checked_file(to, 0, &hash) || hash != entry->state_hash) goto done;
    uint64_t at = 8;
    while (at + 8 <= count) {
        unsigned char block[8];
        if (pread(to, block, 8, (off_t)at) != 8) goto done;
        uint32_t size = le32(block + 4);
        if (!memcmp(block, "MEM ", 4)) {
            *memory = malloc(size);
            if (!*memory || pread(to, *memory, size, (off_t)at + 8) != size) goto done;
            *bytes = size; result = now_ms() < stage->deadline_ms ? 0 : -1; break;
        }
        at += 8 + (((uint64_t)size + 7) & ~(uint64_t)7);
    }
done:
    if (to >= 0) close(to);
    close(from);
    if (result) {
        // Only remove a destination this call successfully created.
        if (to >= 0) unlinkat(stage->directory, stage->state, 0);
        free(*memory); *memory = NULL; *bytes = 0;
    }
    return result;
}
int gs_store_publish(struct gs_store *s, const struct gs_identity *identity,
                     struct gs_stage *stage, const struct gs_receipt *receipt) {
    if (!receipt || !receipt->complete || stage->directory < 0 ||
        now_ms() >= stage->deadline_ms ||
        !id_valid(stage->transaction) || strcmp(stage->transaction, receipt->transaction) ||
        !same_identity(identity, &stage->identity) ||
        validate_identity(s, identity)) return fail(EINVAL);
    if (flock(s->lock, LOCK_EX)) return -1;
    struct gs_index index; int result = load_unlocked(s, &index);
    if (result) goto done;
    for (size_t i = 0; i < index.count; ++i)
        if (!strcmp(index.entries[i].transaction, stage->transaction)) {
            result = same_identity(&index.entries[i].identity, identity) ? 0 : fail(EINVAL); goto done;
        }
    struct gs_entry *entry = upsert(&index, identity); char generation[GS_ID];
    if (!entry || token(generation)) { result = -1; goto done; }
    snprintf(entry->state, sizeof(entry->state), "%s.state", generation);
    snprintf(entry->thumbnail, sizeof(entry->thumbnail), "%s.bmp", generation);
    strcpy(entry->transaction, stage->transaction);
    result = move_asset(s, stage, stage->state, entry->state, 0, &entry->state_hash);
    if (!result && FAULT("state-flushed")) result = -1;
    if (!result) result = move_asset(s, stage, stage->thumbnail, entry->thumbnail, 1, &entry->thumbnail_hash);
    // Both directories changed: the staging slot lost its files and the store gained them.
    if (!result && (fsync(stage->directory) || fsync(s->directory) || syncfs(s->directory) ||
                    FAULT("assets-flushed"))) result = -1;
    if (!result) result = save_unlocked(s, &index, stage->deadline_ms, 1);
done:
    gs_index_close(&index); flock(s->lock, LOCK_UN); return result;
}
