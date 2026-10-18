#ifndef BRICKOS_MAINUI_VISUAL_FILES_H
#define BRICKOS_MAINUI_VISUAL_FILES_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

// Shared by the boot visual and loading-screen selectors.
#define MAINUI_VISUAL_BYTES_MAX (16U * 1024U * 1024U)

// Reads exactly length bytes at offset. Returns 1 on success, 0 on a short read or an error.
int mainui_visual_read_at(int fd, unsigned char *bytes, size_t length, off_t offset);

// A regular file, not a link, of 1 byte to 16 MiB, starting with a PNG signature and IHDR of
// 1..4096 pixels each way. Returns 1 and fills the optional outputs, or 0.
int mainui_visual_png(const char *path, uint32_t *width, uint32_t *height, size_t *size);

// 1 when both regular files hold the same bytes, 0 otherwise (including when one cannot be read).
int mainui_visual_files_equal(const char *a, const char *b);

// Copies source, which must still be a regular file of expected bytes, to target (created or
// truncated, mode 0644) and flushes it. Returns 0 or -1; the caller removes a failed target.
int mainui_visual_copy(const char *source, const char *target, size_t expected);

#endif
