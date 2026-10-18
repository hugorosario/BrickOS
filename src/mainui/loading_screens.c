#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L
#include "loading_screens.h"
#include "visual_files.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#define BACKGROUND "app_loading_bg.png"
#define FILE_NAME 32
// app_loading_bg.png and app_loading_00..99.png, with room for case variants on a host disk.
#define INSTALLED_MAX 256

#ifdef MAINUI_LOADING_SCREENS_FAULTS
int (*mainui_loading_screens_fault)(enum mainui_loading_step step, int file);
#define FAULT(step, file) (mainui_loading_screens_fault && mainui_loading_screens_fault(step, file))
#else
#define FAULT(step, file) 0
#endif

static int device_path(char *out, size_t size, const char *root, const char *path) {
    int n = snprintf(out, size, "%s%s", root ? root : "", path);
    return n >= 0 && (size_t)n < size ? 0 : -1;
}

static int joined(char *out, size_t size, const char *folder, const char *name, const char *suffix) {
    int n = snprintf(out, size, "%s%s%s", folder, name, suffix ? suffix : "");
    return n >= 0 && (size_t)n < size ? 0 : -1;
}

// Frame 0 is the background, as in the sizes array.
static void frame_name(char *out, size_t size, int frame) {
    if (frame == 0) snprintf(out, size, BACKGROUND);
    else snprintf(out, size, "app_loading_%02d.png", frame);
}

int mainui_loading_screen_file(const struct mainui_loading_screen *screen, int frame, char *out, size_t size) {
    if (!screen || !out || frame < 0 || frame > screen->frames) return -1;
    char name[FILE_NAME];
    frame_name(name, sizeof(name), frame);
    return joined(out, size, screen->folder, name, NULL);
}

// The names RetroArch reads: app_loading_bg.png, or app_loading_ with two digits and .png.
// FAT matches names in any case, so these do too.
static int loading_name(const char *name, size_t length) {
    if (length == strlen(BACKGROUND) && strncasecmp(name, BACKGROUND, length) == 0) return 1;
    return length == strlen("app_loading_00.png") && strncasecmp(name, "app_loading_", 12) == 0 &&
           isdigit((unsigned char)name[12]) && isdigit((unsigned char)name[13]) &&
           strncasecmp(name + 14, ".png", 4) == 0;
}

static int leftover_name(const char *name) {
    size_t length = strlen(name), suffix = strlen(".brickos-tmp");
    return length > suffix && (strcmp(name + length - suffix, ".brickos-tmp") == 0 ||
                               strcmp(name + length - suffix, ".brickos-old") == 0) &&
           loading_name(name, length - suffix);
}

// Reads the set in a folder (a host path ending in '/'): a 1024x768 background and frames 01
// onward up to the first missing number, each at most 1024x768. Returns 1 for a valid set.
static int read_set(const char *folder, int *frames, size_t sizes[MAINUI_LOADING_SCREEN_FRAMES + 1]) {
    char path[MAINUI_LOADING_SCREEN_PATH + FILE_NAME], name[FILE_NAME];
    uint32_t width = 0, height = 0;
    size_t size = 0;
    if (joined(path, sizeof(path), folder, BACKGROUND, NULL) < 0 ||
        !mainui_visual_png(path, &width, &height, &size) || width != 1024 || height != 768) return 0;
    sizes[0] = size;
    int count = 0;
    for (int frame = 1; frame <= MAINUI_LOADING_SCREEN_FRAMES; ++frame) {
        frame_name(name, sizeof(name), frame);
        if (joined(path, sizeof(path), folder, name, NULL) < 0) return 0;
        struct stat st;
        if (lstat(path, &st) < 0) {
            if (errno == ENOENT) break;
            return 0;
        }
        if (!mainui_visual_png(path, &width, &height, &size) || width > 1024 || height > 768) return 0;
        sizes[frame] = size;
        count = frame;
    }
    if (!count) return 0;
    *frames = count;
    return 1;
}

static int preset_name(const char *name) {
    if (!name[0] || name[0] == '.' || strlen(name) >= MAINUI_LOADING_SCREEN_NAME) return 0;
    for (const unsigned char *p = (const unsigned char *)name; *p; ++p)
        if (*p < 0x20 || *p == 0x7f || *p == '/') return 0;
    return 1;
}

static int screen_name_order(const void *left, const void *right) {
    const struct mainui_loading_screen *a = left, *b = right;
    return strcmp(a->name, b->name);
}

int mainui_loading_screens_catalog(const char *root, struct mainui_loading_screen **items_out) {
    if (!items_out) return -1;
    *items_out = NULL;
    char directory[MAINUI_LOADING_SCREEN_PATH];
    if (device_path(directory, sizeof(directory), root, MAINUI_LOADING_SCREENS_DIRECTORY) < 0) return -1;
    DIR *stream = opendir(directory);
    if (!stream) return errno == ENOENT ? 0 : -1;
    struct mainui_loading_screen *items = NULL;
    size_t capacity = 0;
    int count = 0, failed = 0;
    for (;;) {
        errno = 0;
        struct dirent *entry = readdir(stream);
        if (!entry) { failed = errno != 0; break; }
        if (!preset_name(entry->d_name)) continue;
        char folder[MAINUI_LOADING_SCREEN_PATH], host[MAINUI_LOADING_SCREEN_PATH];
        struct stat st;
        if (joined(folder, sizeof(folder), MAINUI_LOADING_SCREENS_DIRECTORY, entry->d_name, "/") < 0 ||
            device_path(host, sizeof(host), root, folder) < 0) { failed = 1; break; }
        // Checked without the trailing slash, which would follow a link to a folder elsewhere.
        host[strlen(host) - 1] = 0;
        int folder_entry = lstat(host, &st) == 0 && S_ISDIR(st.st_mode);
        host[strlen(host)] = '/';
        if (!folder_entry) continue;
        struct mainui_loading_screen screen;
        memset(&screen, 0, sizeof(screen));
        if (!read_set(host, &screen.frames, screen.sizes)) continue;
        snprintf(screen.name, sizeof(screen.name), "%s", entry->d_name);
        snprintf(screen.folder, sizeof(screen.folder), "%s", folder);
        if ((size_t)count == capacity) {
            size_t next = capacity ? capacity * 2 : 8;
            struct mainui_loading_screen *grown = count < INT_MAX && next <= SIZE_MAX / sizeof(*items) ?
                                                  realloc(items, next * sizeof(*items)) : NULL;
            if (!grown) { failed = 1; break; }
            items = grown;
            capacity = next;
        }
        items[count++] = screen;
    }
    if (closedir(stream) < 0 || failed) {
        free(items);
        return -1;
    }
    if (count > 1) qsort(items, (size_t)count, sizeof(*items), screen_name_order);
    for (int i = 0; i < count; ++i) items[i].index = i;
    *items_out = items;
    return count;
}

// 1 when the entry exists, 0 when it does not, -1 when it cannot be checked.
static int present(const char *path, size_t *size) {
    struct stat st;
    if (lstat(path, &st) < 0) return errno == ENOENT ? 0 : -1;
    if (size) *size = S_ISREG(st.st_mode) ? (size_t)st.st_size : 0;
    return 1;
}

int mainui_loading_screens_current(const char *root, const struct mainui_loading_screen *items, int count) {
    if (count < 0 || (count > 0 && !items)) return -2;
    char skin[MAINUI_LOADING_SCREEN_PATH], path[MAINUI_LOADING_SCREEN_PATH + FILE_NAME], name[FILE_NAME];
    size_t sizes[MAINUI_LOADING_SCREEN_FRAMES + 1] = {0};
    if (device_path(skin, sizeof(skin), root, MAINUI_LOADING_SKIN_DIRECTORY) < 0) return -2;
    int frames = 0, background;
    if (joined(path, sizeof(path), skin, BACKGROUND, NULL) < 0 || (background = present(path, &sizes[0])) < 0) return -2;
    for (int frame = 1; frame <= MAINUI_LOADING_SCREEN_FRAMES; ++frame) {
        frame_name(name, sizeof(name), frame);
        if (joined(path, sizeof(path), skin, name, NULL) < 0) return -2;
        int found = present(path, &sizes[frame]);
        if (found < 0) return -2;
        if (!found) break;
        frames = frame;
    }
    if (!background && !frames) return MAINUI_LOADING_SCREEN_TRIMUI;
    if (!background || !frames) return -1;
    for (int i = 0; i < count; ++i) {
        const struct mainui_loading_screen *screen = &items[i];
        if (screen->frames != frames || memcmp(screen->sizes, sizes, sizeof(size_t) * (size_t)(frames + 1)) != 0)
            continue;
        int same = 1;
        for (int frame = 0; same && frame <= frames; ++frame) {
            char source[MAINUI_LOADING_SCREEN_PATH], host[MAINUI_LOADING_SCREEN_PATH];
            frame_name(name, sizeof(name), frame);
            same = mainui_loading_screen_file(screen, frame, source, sizeof(source)) == 0 &&
                   device_path(host, sizeof(host), root, source) == 0 &&
                   joined(path, sizeof(path), skin, name, NULL) == 0 && mainui_visual_files_equal(path, host);
        }
        if (same) return screen->index;
    }
    return -1;
}

// The loading-screen images in the skin folder, by the name they have there.
struct installed {
    int count;
    char names[INSTALLED_MAX][FILE_NAME];
};

// Removes this module's own leftovers and lists the installed images. Returns 0, or -1.
static int scan_skin(const char *skin, struct installed *found) {
    found->count = 0;
    DIR *stream = opendir(skin);
    if (!stream) return -1;
    int failed = 0;
    for (;;) {
        errno = 0;
        struct dirent *entry = readdir(stream);
        if (!entry) { failed = errno != 0; break; }
        char path[MAINUI_LOADING_SCREEN_PATH + NAME_MAX + 1];
        struct stat st;
        if (joined(path, sizeof(path), skin, entry->d_name, NULL) < 0) { failed = 1; break; }
        if (leftover_name(entry->d_name)) {
            // A power cut can leave these; RetroArch ignores them, and this run starts clean.
            if (unlink(path) < 0 && errno != ENOENT) { failed = 1; break; }
            continue;
        }
        if (!loading_name(entry->d_name, strlen(entry->d_name))) continue;
        if (lstat(path, &st) < 0 || S_ISDIR(st.st_mode)) continue;
        if (found->count == INSTALLED_MAX) { failed = 1; break; }
        // loading_name() accepts only 18-byte names, so the copy and its terminator always fit.
        memcpy(found->names[found->count++], entry->d_name, sizeof("app_loading_00.png"));
    }
    if (closedir(stream) < 0) failed = 1;
    return failed ? -1 : 0;
}

// Renames each installed image to <name>.brickos-old. On a failure the ones already moved go
// back, and -1 is returned.
static int put_aside(const char *skin, const struct installed *found) {
    char from[MAINUI_LOADING_SCREEN_PATH + FILE_NAME], to[MAINUI_LOADING_SCREEN_PATH + FILE_NAME + 16];
    for (int i = 0; i < found->count; ++i) {
        if (joined(from, sizeof(from), skin, found->names[i], NULL) < 0 ||
            joined(to, sizeof(to), skin, found->names[i], ".brickos-old") < 0 ||
            FAULT(MAINUI_LOADING_PUT_ASIDE, i) || rename(from, to) < 0) {
            while (--i >= 0) {
                if (joined(from, sizeof(from), skin, found->names[i], NULL) == 0 &&
                    joined(to, sizeof(to), skin, found->names[i], ".brickos-old") == 0)
                    (void)rename(to, from);
            }
            return -1;
        }
    }
    return 0;
}

static void restore_aside(const char *skin, const struct installed *found) {
    char from[MAINUI_LOADING_SCREEN_PATH + FILE_NAME], to[MAINUI_LOADING_SCREEN_PATH + FILE_NAME + 16];
    for (int i = found->count - 1; i >= 0; --i)
        if (joined(from, sizeof(from), skin, found->names[i], NULL) == 0 &&
            joined(to, sizeof(to), skin, found->names[i], ".brickos-old") == 0)
            (void)rename(to, from);
}

static void delete_aside(const char *skin, const struct installed *found) {
    char path[MAINUI_LOADING_SCREEN_PATH + FILE_NAME + 16];
    for (int i = 0; i < found->count; ++i)
        if (joined(path, sizeof(path), skin, found->names[i], ".brickos-old") == 0) (void)unlink(path);
}

// trimui, trimui/res and trimui/res/skin under the root.
static int tree_path(char *out, size_t size, const char *root, int level) {
    static const char *const levels[3] = {"/mnt/SDCARD/trimui", "/mnt/SDCARD/trimui/res", "/mnt/SDCARD/trimui/res/skin"};
    return device_path(out, size, root, levels[level]);
}

static void remove_created(const char *root, const int created[3]) {
    char path[MAINUI_LOADING_SCREEN_PATH];
    for (int level = 2; level >= 0; --level)
        if (created[level] && tree_path(path, sizeof(path), root, level) == 0) (void)rmdir(path);
}

static void remove_staged(const char *skin, int staged) {
    char name[FILE_NAME], path[MAINUI_LOADING_SCREEN_PATH + FILE_NAME + 16];
    for (int frame = 0; frame < staged; ++frame) {
        frame_name(name, sizeof(name), frame);
        if (joined(path, sizeof(path), skin, name, ".brickos-tmp") == 0) (void)unlink(path);
    }
}

int mainui_loading_screens_apply(const char *root, const struct mainui_loading_screen *items, int count, int index) {
    if (!items || count <= 0) return -1;
    const struct mainui_loading_screen *selected = NULL;
    for (int i = 0; i < count; ++i) if (items[i].index == index) { selected = &items[i]; break; }
    if (!selected) return -1;
    char source_folder[MAINUI_LOADING_SCREEN_PATH], skin[MAINUI_LOADING_SCREEN_PATH];
    size_t sizes[MAINUI_LOADING_SCREEN_FRAMES + 1];
    int frames = 0;
    // The preset may have changed since it was listed: install what it holds now, if still valid.
    if (device_path(source_folder, sizeof(source_folder), root, selected->folder) < 0 ||
        device_path(skin, sizeof(skin), root, MAINUI_LOADING_SKIN_DIRECTORY) < 0 ||
        !read_set(source_folder, &frames, sizes)) return -1;

    int created[3] = {0, 0, 0};
    for (int level = 0; level < 3; ++level) {
        char path[MAINUI_LOADING_SCREEN_PATH];
        struct stat st;
        if (tree_path(path, sizeof(path), root, level) < 0) { remove_created(root, created); return -1; }
        if (mkdir(path, 0755) == 0) created[level] = 1;
        else if (errno != EEXIST || lstat(path, &st) < 0 || !S_ISDIR(st.st_mode)) {
            remove_created(root, created);
            return -1;
        }
    }
    struct installed *found = malloc(sizeof(*found));
    if (!found || scan_skin(skin, found) < 0) {
        free(found);
        remove_created(root, created);
        return -1;
    }

    char name[FILE_NAME], source[MAINUI_LOADING_SCREEN_PATH + FILE_NAME];
    char temporary[MAINUI_LOADING_SCREEN_PATH + FILE_NAME + 16], target[MAINUI_LOADING_SCREEN_PATH + FILE_NAME];
    int staged = 0, placed = 0, ok = 1;
    for (int frame = 0; ok && frame <= frames; ++frame) {
        frame_name(name, sizeof(name), frame);
        ok = joined(source, sizeof(source), source_folder, name, NULL) == 0 &&
             joined(temporary, sizeof(temporary), skin, name, ".brickos-tmp") == 0;
        if (ok) staged = frame + 1;
        ok = ok && !FAULT(MAINUI_LOADING_COPY, frame) && mainui_visual_copy(source, temporary, sizes[frame]) == 0 &&
             !FAULT(MAINUI_LOADING_COMPARE, frame) && mainui_visual_files_equal(source, temporary);
    }
    if (ok && put_aside(skin, found) < 0) ok = 0;
    else if (ok) {
        for (int frame = 0; ok && frame <= frames; ++frame) {
            frame_name(name, sizeof(name), frame);
            ok = joined(temporary, sizeof(temporary), skin, name, ".brickos-tmp") == 0 &&
                 joined(target, sizeof(target), skin, name, NULL) == 0 &&
                 !FAULT(MAINUI_LOADING_SWAP, frame) && rename(temporary, target) == 0;
            if (ok) placed = frame + 1;
        }
        if (!ok) {
            // Undo the swap, then bring the previous set back.
            for (int frame = placed - 1; frame >= 0; --frame) {
                frame_name(name, sizeof(name), frame);
                if (joined(temporary, sizeof(temporary), skin, name, ".brickos-tmp") == 0 &&
                    joined(target, sizeof(target), skin, name, NULL) == 0)
                    (void)rename(target, temporary);
            }
            restore_aside(skin, found);
        } else {
            delete_aside(skin, found);
        }
    }
    if (!ok) {
        remove_staged(skin, staged);
        remove_created(root, created);
    }
    free(found);
    sync();
    return ok ? 0 : -1;
}

int mainui_loading_screens_restore_default(const char *root) {
    char skin[MAINUI_LOADING_SCREEN_PATH];
    if (device_path(skin, sizeof(skin), root, MAINUI_LOADING_SKIN_DIRECTORY) < 0) return -1;
    struct installed *found = malloc(sizeof(*found));
    if (!found) return -1;
    struct stat st;
    int result = 0;
    if (lstat(skin, &st) == 0) {
        if (scan_skin(skin, found) < 0 || put_aside(skin, found) < 0) result = -1;
        else delete_aside(skin, found);
    } else if (errno != ENOENT) {
        result = -1;
    }
    free(found);
    if (result == 0) {
        // rmdir refuses a folder that still holds anything, so other files are never at risk.
        char path[MAINUI_LOADING_SCREEN_PATH];
        for (int level = 2; level >= 0; --level)
            if (tree_path(path, sizeof(path), root, level) == 0) (void)rmdir(path);
    }
    sync();
    return result;
}
