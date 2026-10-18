#define _POSIX_C_SOURCE 200809L
#include "visual_files.h"

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

int mainui_visual_read_at(int fd, unsigned char *bytes, size_t length, off_t offset) {
    size_t used = 0;
    while (used < length) {
        ssize_t n = pread(fd, bytes + used, length - used, offset + (off_t)used);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return 0;
        used += (size_t)n;
    }
    return 1;
}

int mainui_visual_png(const char *path, uint32_t *width_out, uint32_t *height_out, size_t *size_out) {
    static const unsigned char signature[8] = {137, 80, 78, 71, 13, 10, 26, 10};
    unsigned char header[24];
    struct stat st;
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return 0;
    int valid = 0;
    if (fstat(fd, &st) == 0 && S_ISREG(st.st_mode) && st.st_size > 0 &&
        (uint64_t)st.st_size <= MAINUI_VISUAL_BYTES_MAX && mainui_visual_read_at(fd, header, sizeof(header), 0) &&
        memcmp(header, signature, sizeof(signature)) == 0 && memcmp(header + 12, "IHDR", 4) == 0) {
        uint32_t width = ((uint32_t)header[16] << 24) | ((uint32_t)header[17] << 16) |
                         ((uint32_t)header[18] << 8) | header[19];
        uint32_t height = ((uint32_t)header[20] << 24) | ((uint32_t)header[21] << 16) |
                          ((uint32_t)header[22] << 8) | header[23];
        valid = width && height && width <= 4096 && height <= 4096;
        if (valid) {
            if (width_out) *width_out = width;
            if (height_out) *height_out = height;
            if (size_out) *size_out = (size_t)st.st_size;
        }
    }
    close(fd);
    return valid;
}

int mainui_visual_files_equal(const char *a, const char *b) {
    unsigned char left[8192], right[8192];
    int x = open(a, O_RDONLY | O_CLOEXEC | O_NOFOLLOW), y = open(b, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (x < 0 || y < 0) { if (x >= 0) close(x); if (y >= 0) close(y); return 0; }
    int same = 1;
    for (;;) {
        ssize_t nx = read(x, left, sizeof(left)), ny = read(y, right, sizeof(right));
        if (nx < 0 || ny < 0 || nx != ny || (nx > 0 && memcmp(left, right, (size_t)nx) != 0)) { same = 0; break; }
        if (nx == 0) break;
    }
    close(x); close(y);
    return same;
}

int mainui_visual_copy(const char *source, const char *target, size_t expected) {
    unsigned char bytes[8192];
    int in = open(source, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (in < 0) return -1;
    struct stat src;
    if (fstat(in, &src) < 0 || !S_ISREG(src.st_mode) || src.st_size <= 0 ||
        (uint64_t)src.st_size > MAINUI_VISUAL_BYTES_MAX || (size_t)src.st_size != expected) { close(in); return -1; }
    int out = open(target, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW, 0644);
    if (out < 0) { close(in); return -1; }
    int ok = 1;
    for (;;) {
        ssize_t n = read(in, bytes, sizeof(bytes));
        if (n < 0) { ok = 0; break; }
        if (n == 0) break;
        ssize_t done = 0;
        while (done < n) {
            ssize_t wrote = write(out, bytes + done, (size_t)(n - done));
            if (wrote <= 0) { ok = 0; break; }
            done += wrote;
        }
        if (!ok) break;
    }
    if (fchmod(out, 0644) < 0 || fsync(out) < 0) ok = 0;
    if (close(out) < 0) ok = 0;
    close(in);
    return ok ? 0 : -1;
}
