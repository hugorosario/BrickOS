#define _POSIX_C_SOURCE 200809L
#include "music.h"

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

int osdd_music_reader_init(struct osdd_music_reader *reader, const char *path) {
    if (reader == NULL || path == NULL || path[0] == 0) return -1;
    memset(reader, 0, sizeof(*reader));
    reader->path = path;
    reader->frame = malloc(OSDD_MUSIC_BYTES);
    reader->scratch = malloc(OSDD_MUSIC_BYTES);
    reader->scratch_second = malloc(OSDD_MUSIC_BYTES);
    if (reader->frame == NULL || reader->scratch == NULL || reader->scratch_second == NULL) {
        osdd_music_reader_close(reader);
        return -1;
    }
    return 0;
}

static int read_exact(int fd, unsigned char *target) {
    size_t offset = 0;
    while (offset < OSDD_MUSIC_BYTES) {
        ssize_t count = pread(fd, target + offset, OSDD_MUSIC_BYTES - offset, (off_t)offset);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) return -1;
        offset += (size_t)count;
    }
    return 0;
}

enum osdd_music_result osdd_music_reader_poll(struct osdd_music_reader *reader,
                                               char *error, size_t error_size) {
    if (error != NULL && error_size > 0) error[0] = 0;
    if (reader == NULL || reader->path == NULL || reader->frame == NULL ||
        reader->scratch == NULL || reader->scratch_second == NULL) {
        if (error != NULL && error_size > 0) snprintf(error, error_size, "invalid music reader");
        return OSDD_MUSIC_ERROR;
    }
    int fd = open(reader->path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        if (errno == ENOENT) {
            reader->has_frame = 0;
            return OSDD_MUSIC_ABSENT;
        }
        if (error != NULL && error_size > 0) snprintf(error, error_size, "open music frame: %s", strerror(errno));
        return OSDD_MUSIC_ERROR;
    }
    struct stat before, after;
    if (fstat(fd, &before) < 0) {
        if (error != NULL && error_size > 0) snprintf(error, error_size, "stat music frame: %s", strerror(errno));
        close(fd);
        return OSDD_MUSIC_ERROR;
    }
    if (!S_ISREG(before.st_mode) || before.st_size != OSDD_MUSIC_BYTES) {
        close(fd);
        return OSDD_MUSIC_RETRY;
    }
    if (read_exact(fd, reader->scratch) < 0) {
        close(fd);
        return OSDD_MUSIC_RETRY;
    }
    // A read equal to the kept frame needs no second pass: any later change is seen on
    // the next poll. A different read is kept only if a second read agrees with it.
    if (reader->has_frame && memcmp(reader->scratch, reader->frame, OSDD_MUSIC_BYTES) == 0) {
        close(fd);
        return OSDD_MUSIC_UNCHANGED;
    }
    if (reader->after_first_read != NULL) reader->after_first_read(reader->after_first_read_context);
    if (read_exact(fd, reader->scratch_second) < 0 || fstat(fd, &after) < 0) {
        close(fd);
        return OSDD_MUSIC_RETRY;
    }
    close(fd);
    if (before.st_dev != after.st_dev || before.st_ino != after.st_ino ||
        before.st_size != after.st_size ||
        memcmp(reader->scratch, reader->scratch_second, OSDD_MUSIC_BYTES) != 0)
        return OSDD_MUSIC_RETRY;
    memcpy(reader->frame, reader->scratch, OSDD_MUSIC_BYTES);
    reader->has_frame = 1;
    return OSDD_MUSIC_UPDATE;
}

void osdd_music_reader_close(struct osdd_music_reader *reader) {
    if (reader == NULL) return;
    free(reader->frame);
    free(reader->scratch);
    free(reader->scratch_second);
    reader->frame = NULL;
    reader->scratch = NULL;
    reader->scratch_second = NULL;
    reader->has_frame = 0;
}

void osdd_music_sender_init(struct osdd_music_sender *sender, const char *path) {
    if (sender == NULL) return;
    sender->path = path;
    sender->fd = -1;
}

int osdd_music_sender_open(struct osdd_music_sender *sender, char *error, size_t error_size) {
    if (error != NULL && error_size > 0) error[0] = 0;
    if (sender == NULL || sender->path == NULL || sender->path[0] == 0) {
        if (error != NULL && error_size > 0) snprintf(error, error_size, "invalid music FIFO");
        return -1;
    }
    if (sender->fd >= 0) return 0;
    // Without a reader, a non-blocking open for writing fails with ENXIO.
    int fd = open(sender->path, O_WRONLY | O_NONBLOCK | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) {
        if (error != NULL && error_size > 0)
            snprintf(error, error_size, "open music FIFO: %s", strerror(errno));
        return -1;
    }
    struct stat info;
    if (fstat(fd, &info) < 0 || !S_ISFIFO(info.st_mode)) {
        if (error != NULL && error_size > 0) snprintf(error, error_size, "music command path is not a FIFO");
        close(fd);
        return -1;
    }
    sender->fd = fd;
    return 0;
}

int osdd_music_send(struct osdd_music_sender *sender, enum osdd_music_command command,
                    char *error, size_t error_size) {
    static const char *const payloads[] = {
        "enable\n", "disable\n", "left\n", "right\n", "up\n", "down\n", "key_a\n"
    };
    if (error != NULL && error_size > 0) error[0] = 0;
    if (sender == NULL || command < OSDD_MUSIC_ENABLE || command > OSDD_MUSIC_KEY_A) {
        if (error != NULL && error_size > 0) snprintf(error, error_size, "invalid music command");
        return -1;
    }
    const char *payload = payloads[command];
    size_t length = strlen(payload);
    // One reopen covers a reader that closed and reopened the FIFO since the last send.
    for (int attempt = 0; attempt < 2; ++attempt) {
        if (osdd_music_sender_open(sender, error, error_size) < 0) return -1;
        ssize_t written;
        do {
            written = write(sender->fd, payload, length);
        } while (written < 0 && errno == EINTR);
        if (written == (ssize_t)length) return 0;
        int saved_errno = errno;
        if (written < 0 && saved_errno == EPIPE) {
            osdd_music_sender_close(sender);
            continue;
        }
        if (error != NULL && error_size > 0)
            snprintf(error, error_size, "write music FIFO: %s",
                     written < 0 ? strerror(saved_errno) : "short write");
        return -1;
    }
    if (error != NULL && error_size > 0) snprintf(error, error_size, "write music FIFO: no reader");
    return -1;
}

void osdd_music_sender_close(struct osdd_music_sender *sender) {
    if (sender == NULL || sender->fd < 0) return;
    close(sender->fd);
    sender->fd = -1;
}
