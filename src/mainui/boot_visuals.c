#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L
#include "boot_visuals.h"
#include "json.h"
#include "visual_files.h"

#include <errno.h>
#include <dirent.h>
#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#define BOOT_MOUNT "/mnt/emmcblk0p1"
#define BOOT_DEVICE "/dev/mmcblk0p1"

static int device_path(char *out, size_t size, const char *root, const char *path) {
    int n = snprintf(out, size, "%s%s", root ? root : "", path);
    return n >= 0 && (size_t)n < size ? 0 : -1;
}

static const char *catalog_directory(enum mainui_boot_visual_kind kind) {
    return kind == MAINUI_BOOTLOGO ? "/mnt/SDCARD/BrickOS/bootlogos/" :
                                     "/mnt/SDCARD/BrickOS/splashscreens/";
}

static int expected_extension(const char *name, enum mainui_boot_visual_kind kind) {
    const char *extension = kind == MAINUI_BOOTLOGO ? ".bmp" : ".png";
    size_t name_length = strlen(name), extension_length = strlen(extension);
    return name_length > extension_length &&
           strcasecmp(name + name_length - extension_length, extension) == 0;
}

static int visual_name_order(const void *left, const void *right) {
    const struct mainui_boot_visual *a = left, *b = right;
    return strcmp(a->name, b->name);
}

static uint16_t little16(const unsigned char *bytes) {
    return (uint16_t)bytes[0] | ((uint16_t)bytes[1] << 8);
}

static uint32_t little32(const unsigned char *bytes) {
    return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8) |
           ((uint32_t)bytes[2] << 16) | ((uint32_t)bytes[3] << 24);
}

static int valid_bitmap(const char *path, enum mainui_boot_visual_kind kind, size_t *size_out) {
    if (kind != MAINUI_BOOTLOGO) return mainui_visual_png(path, NULL, NULL, size_out);
    unsigned char header[54];
    struct stat st;
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return 0;
    if (fstat(fd, &st) < 0 || !S_ISREG(st.st_mode) || st.st_size <= 0 ||
        (uint64_t)st.st_size > MAINUI_VISUAL_BYTES_MAX) { close(fd); return 0; }
    int valid = 0;
    if (!mainui_visual_read_at(fd, header, sizeof(header), 0) || header[0] != 'B' || header[1] != 'M')
        goto done;
    uint32_t dib = little32(header + 14), pixel_offset = little32(header + 10);
    int32_t width = (int32_t)little32(header + 18), height = (int32_t)little32(header + 22);
    unsigned planes = little16(header + 26), bits = little16(header + 28);
    uint32_t compression = little32(header + 30);
    if (dib < 40 || width <= 0 || height == 0 || height == INT32_MIN || width > 4096 ||
        (height < 0 ? -height : height) > 4096 || planes != 1 || (bits != 24 && bits != 32) ||
        (compression != 0 && !(compression == 3 && bits == 32))) goto done;
    uint64_t header_end = 14ULL + dib;
    if (compression == 3) {
        // BI_BITFIELDS stores masks after a 40-byte header or inside larger V2+ headers.
        unsigned char masks[12];
        if ((dib != 40 && dib < 52) || !mainui_visual_read_at(fd, masks, sizeof(masks), 54)) goto done;
        uint32_t red = little32(masks), green = little32(masks + 4), blue = little32(masks + 8);
        if (!red || !green || !blue || (red & green) || (red & blue) || (green & blue)) goto done;
        if (header_end < 66) header_end = 66;
    }
    if (pixel_offset < header_end || pixel_offset > (uint64_t)st.st_size) goto done;
    uint64_t row_bits = (uint64_t)width * bits;
    uint64_t row_bytes = ((row_bits + 31) / 32) * 4;
    uint64_t image_bytes = row_bytes * (uint64_t)(height < 0 ? -height : height);
    if (image_bytes > (uint64_t)st.st_size - pixel_offset) goto done;
    valid = 1;
done:
    close(fd);
    if (valid && size_out) *size_out = (size_t)st.st_size;
    return valid;
}

int mainui_boot_visuals_catalog(const char *root, enum mainui_boot_visual_kind kind,
                                struct mainui_boot_visual **items_out) {
    if (!items_out || (kind != MAINUI_BOOTLOGO && kind != MAINUI_SPLASHSCREEN)) return -1;
    *items_out = NULL;
    char directory[MAINUI_BOOT_VISUAL_PATH];
    if (device_path(directory, sizeof(directory), root, catalog_directory(kind)) < 0) return -1;
    DIR *stream = opendir(directory);
    if (!stream) return errno == ENOENT ? 0 : -1;

    struct mainui_boot_visual *items = NULL;
    size_t capacity = 0;
    int count = 0, read_error = 0;
    struct dirent *entry;
    for (;;) {
        errno = 0;
        entry = readdir(stream);
        if (!entry) { read_error = errno; break; }
        if (!expected_extension(entry->d_name, kind)) continue;
        char path[MAINUI_BOOT_VISUAL_PATH], source[MAINUI_BOOT_VISUAL_PATH];
        size_t bytes = 0;
        int n = snprintf(source, sizeof(source), "%s%s", catalog_directory(kind), entry->d_name);
        if (n < 0 || (size_t)n >= sizeof(source) || device_path(path, sizeof(path), root, source) < 0) {
            closedir(stream);
            free(items);
            return -1;
        }
        if (!valid_bitmap(path, kind, &bytes)) continue;
        if (count == INT_MAX) {
            closedir(stream);
            free(items);
            return -1;
        }
        if ((size_t)count == capacity) {
            size_t next = capacity ? capacity * 2 : 8;
            if (next < capacity || next > SIZE_MAX / sizeof(*items)) {
                closedir(stream);
                free(items);
                return -1;
            }
            struct mainui_boot_visual *grown = realloc(items, next * sizeof(*items));
            if (!grown) {
                closedir(stream);
                free(items);
                return -1;
            }
            items = grown;
            capacity = next;
        }
        items[count].index = count;
        snprintf(items[count].name, sizeof(items[count].name), "%s", entry->d_name);
        snprintf(items[count].path, sizeof(items[count].path), "%s", source);
        items[count].size = bytes;
        ++count;
    }
    if (closedir(stream) < 0 || read_error != 0) {
        free(items);
        return -1;
    }
    if (count > 1) qsort(items, (size_t)count, sizeof(*items), visual_name_order);
    for (int i = 0; i < count; ++i) items[i].index = i;
    *items_out = items;
    return count;
}

static int has_option(const char *options, const char *wanted) {
    size_t length = strlen(wanted);
    const char *p = options;
    while (p && *p) {
        const char *end = strchr(p, ',');
        size_t n = end ? (size_t)(end - p) : strlen(p);
        if (n == length && memcmp(p, wanted, n) == 0) return 1;
        p = end ? end + 1 : NULL;
    }
    return 0;
}

static int mount_state(int *read_only) {
    FILE *f = fopen("/proc/mounts", "r");
    if (!f) return -1;
    char source[256], target[256], type[64], options[256];
    int found = 0;
    while (fscanf(f, "%255s %255s %63s %255s %*d %*d\n", source, target, type, options) == 4) {
        if (strcmp(target, BOOT_MOUNT) != 0) continue;
        found = strcmp(source, BOOT_DEVICE) == 0 && strcmp(type, "vfat") == 0 ? 1 : -1;
        if (found == 1) *read_only = has_option(options, "ro");
        break;
    }
    fclose(f);
    return found;
}

static int begin_boot_mount(int write_access, int *mounted_here) {
    int read_only = 0;
    *mounted_here = 0;
    int state = mount_state(&read_only);
    if (state < 0) return -1;
    if (state == 1) return write_access && read_only ? -1 : 0;
    struct stat st;
    if (stat(BOOT_MOUNT, &st) < 0 && errno == ENOENT && mkdir(BOOT_MOUNT, 0755) < 0 && errno != EEXIST) return -1;
    if (mount(BOOT_DEVICE, BOOT_MOUNT, "vfat", write_access ? 0 : MS_RDONLY, "") < 0) return -1;
    *mounted_here = 1;
    return 0;
}

static int active_path(char *out, size_t size, const char *root, enum mainui_boot_visual_kind kind) {
    return device_path(out, size, root, kind == MAINUI_BOOTLOGO ? BOOT_MOUNT "/bootlogo.bmp" : "/etc/splash.png");
}

int mainui_boot_visuals_current(const char *root, enum mainui_boot_visual_kind kind,
                                const struct mainui_boot_visual *items, int count) {
    if (!items || count < 0 || (kind != MAINUI_BOOTLOGO && kind != MAINUI_SPLASHSCREEN)) return -2;
    if (count == 0) return -1;
    int mounted_here = 0;
    if ((!root || !root[0]) && kind == MAINUI_BOOTLOGO && begin_boot_mount(0, &mounted_here) < 0) return -2;
    char active[MAINUI_BOOT_VISUAL_PATH];
    int result = -1;
    if (active_path(active, sizeof(active), root, kind) == 0) {
        for (int i = 0; i < count; ++i) {
            char source[MAINUI_BOOT_VISUAL_PATH];
            if (device_path(source, sizeof(source), root, items[i].path) == 0 && mainui_visual_files_equal(active, source)) {
                result = items[i].index;
                break;
            }
        }
    } else result = -2;
    if (mounted_here && umount(BOOT_MOUNT) < 0) return -2;
    return result;
}

int mainui_boot_visuals_apply(const char *root, enum mainui_boot_visual_kind kind,
                              const struct mainui_boot_visual *items, int count, int index) {
    if (!items || count < 0 || (kind != MAINUI_BOOTLOGO && kind != MAINUI_SPLASHSCREEN)) return -1;
    const struct mainui_boot_visual *selected = NULL;
    for (int i = 0; i < count; ++i) if (items[i].index == index) { selected = &items[i]; break; }
    if (!selected) return -1;
    char source[MAINUI_BOOT_VISUAL_PATH], target[MAINUI_BOOT_VISUAL_PATH], temp[MAINUI_BOOT_VISUAL_PATH];
    size_t size = 0;
    if (device_path(source, sizeof(source), root, selected->path) < 0 ||
        !valid_bitmap(source, kind, &size) || size != selected->size ||
        active_path(target, sizeof(target), root, kind) < 0 ||
        snprintf(temp, sizeof(temp), "%s.brickos-tmp", target) >= (int)sizeof(temp)) return -1;
    int mounted_here = 0;
    if ((!root || !root[0]) && kind == MAINUI_BOOTLOGO && begin_boot_mount(1, &mounted_here) < 0) return -1;
    int ok = 0;
    if (mainui_visual_copy(source, temp, size) == 0 && mainui_visual_files_equal(source, temp) && rename(temp, target) == 0) ok = 1;
    if (!ok) unlink(temp);
    sync();
    if (mounted_here) (void)umount(BOOT_MOUNT);
    return ok ? 0 : -1;
}

int mainui_boot_visuals_pick(const struct mainui_boot_visual *items, int count, int active, uint32_t random) {
    if (!items || count <= 0) return -1;
    int skip = -1;
    for (int i = 0; i < count; ++i) if (items[i].index == active) { skip = i; break; }
    int candidates = count - (skip >= 0);
    if (candidates <= 0) return -1;
    int left = (int)(random % (uint32_t)candidates);
    for (int i = 0; i < count; ++i) {
        if (i == skip) continue;
        if (left-- == 0) return items[i].index;
    }
    return -1;
}

#define MODES_BYTES 65536
static const char *const mode_keys[2] = {"bootlogo", "splashscreen"};

// The modes file's text and its parsed members. Returns 1 when it is missing, 0 when it holds a
// flat object, and -1 otherwise.
static int read_modes(const char *root, char *text, struct mainui_json *document) {
    char path[MAINUI_BOOT_VISUAL_PATH];
    if (device_path(path, sizeof(path), root, MAINUI_BOOT_VISUALS_MODES) < 0) return -1;
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return errno == ENOENT ? 1 : -1;
    struct stat st;
    size_t used = 0;
    int ok = fstat(fd, &st) == 0 && S_ISREG(st.st_mode) && st.st_size >= 0 && st.st_size < MODES_BYTES;
    while (ok) {
        ssize_t n = read(fd, text + used, MODES_BYTES - 1 - used);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) { ok = n == 0; break; }
        used += (size_t)n;
        if (used == MODES_BYTES - 1) ok = 0;
    }
    close(fd);
    if (!ok) return -1;
    text[used] = 0;
    return mainui_json_parse(document, text, used) == 0 ? 0 : -1;
}

int mainui_boot_visuals_modes_load(const char *root, int random[2]) {
    if (!random) return -1;
    random[0] = random[1] = 0;
    char *text = malloc(MODES_BYTES);
    struct mainui_json *document = malloc(sizeof(*document));
    int result = text && document ? read_modes(root, text, document) : -1;
    if (result == 0) {
        for (int i = 0; i < 2; ++i) {
            const struct mainui_json_member *mode = mainui_json_last(document, mode_keys[i]);
            random[i] = mode && mode->type == MAINUI_JSON_STRING && strcmp(mode->text, "random") == 0;
        }
    }
    free(text);
    free(document);
    return result < 0 ? -1 : 0;
}

static int append_text(char *out, size_t size, size_t *length, const char *text, size_t n) {
    if (n > size - *length) return -1;
    memcpy(out + *length, text, n);
    *length += n;
    return 0;
}

static int append_string(char *out, size_t size, size_t *length, const char *text, size_t n) {
    int written = mainui_json_escape(out + *length, size - *length, text, n);
    if (written < 0) return -1;
    *length += (size_t)written;
    return 0;
}

static int make_parents(const char *path) {
    char parent[MAINUI_BOOT_VISUAL_PATH];
    snprintf(parent, sizeof(parent), "%s", path);
    for (char *p = parent + 1; *p; ++p) {
        if (*p != '/') continue;
        *p = 0;
        if (mkdir(parent, 0755) < 0 && errno != EEXIST) return -1;
        *p = '/';
    }
    return 0;
}

int mainui_boot_visuals_modes_save(const char *root, const int random[2]) {
    char path[MAINUI_BOOT_VISUAL_PATH], temporary[MAINUI_BOOT_VISUAL_PATH + 16];
    if (!random || device_path(path, sizeof(path), root, MAINUI_BOOT_VISUALS_MODES) < 0 ||
        snprintf(temporary, sizeof(temporary), "%s.tmp.XXXXXX", path) >= (int)sizeof(temporary) || make_parents(path) < 0)
        return -1;
    struct stat destination;
    if (lstat(path, &destination) == 0 ? !S_ISREG(destination.st_mode) : errno != ENOENT) return -1;
    char *text = malloc(MODES_BYTES), *out = malloc(MODES_BYTES);
    struct mainui_json *document = malloc(sizeof(*document));
    int result = -1;
    if (!text || !out || !document) goto done;
    // Members other than the two modes are kept, in their order, after them.
    if (read_modes(root, text, document) != 0) document->count = 0;
    size_t length = 0;
    int failed = append_text(out, MODES_BYTES, &length, "{", 1);
    for (int i = 0; i < 2; ++i) {
        const char *value = random[i] ? "random" : "fixed";
        failed |= append_text(out, MODES_BYTES, &length, i ? ",\n\t" : "\n\t", i ? 3 : 2);
        failed |= append_string(out, MODES_BYTES, &length, mode_keys[i], strlen(mode_keys[i]));
        failed |= append_text(out, MODES_BYTES, &length, ": ", 2);
        failed |= append_string(out, MODES_BYTES, &length, value, strlen(value));
    }
    for (size_t i = 0; i < document->count; ++i) {
        const struct mainui_json_member *member = &document->members[i];
        if (strcasecmp(member->key, mode_keys[0]) == 0 || strcasecmp(member->key, mode_keys[1]) == 0) continue;
        failed |= append_text(out, MODES_BYTES, &length, ",\n\t", 3);
        failed |= append_string(out, MODES_BYTES, &length, member->key, strlen(member->key));
        failed |= append_text(out, MODES_BYTES, &length, ": ", 2);
        if (member->type == MAINUI_JSON_STRING) failed |= append_string(out, MODES_BYTES, &length, member->text, member->text_length);
        else failed |= append_text(out, MODES_BYTES, &length, member->text, member->text_length);
    }
    failed |= append_text(out, MODES_BYTES, &length, "\n}\n", 3);
    if (failed) goto done;
    int fd = mkstemp(temporary);
    if (fd < 0) goto done;
    (void)fcntl(fd, F_SETFD, FD_CLOEXEC);
    size_t written = 0;
    int ok = fchmod(fd, 0644) == 0;
    while (ok && written < length) {
        ssize_t n = write(fd, out + written, length - written);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) ok = 0;
        else written += (size_t)n;
    }
    if (ok && fsync(fd) < 0) ok = 0;
    if (close(fd) < 0) ok = 0;
    if (ok && rename(temporary, path) == 0) {
        sync();
        result = 0;
    } else {
        unlink(temporary);
    }
done:
    free(text);
    free(out);
    free(document);
    return result;
}
