#ifndef BRICKOS_OSDD_MUSIC_H
#define BRICKOS_OSDD_MUSIC_H

#include <stddef.h>
#include <stdint.h>

// musicserver's widget canvas: 540x260, 32 bpp, rows without padding. The official
// daemon copies its bytes into the BGRA8888 panel unchanged, so byte order is B, G, R, A.
#define OSDD_MUSIC_WIDTH 540
#define OSDD_MUSIC_HEIGHT 260
#define OSDD_MUSIC_STRIDE (OSDD_MUSIC_WIDTH * 4)
#define OSDD_MUSIC_BYTES (OSDD_MUSIC_STRIDE * OSDD_MUSIC_HEIGHT)

enum osdd_music_result {
    OSDD_MUSIC_ERROR = -1,
    OSDD_MUSIC_UNCHANGED = 0,
    OSDD_MUSIC_UPDATE = 1,
    OSDD_MUSIC_RETRY = 2,
    OSDD_MUSIC_ABSENT = 3
};

struct osdd_music_reader {
    const char *path;
    unsigned char *frame;
    unsigned char *scratch;
    unsigned char *scratch_second;
    int has_frame;
    void (*after_first_read)(void *context); // Test hook before a changed frame's second read.
    void *after_first_read_context;
};

enum osdd_music_command {
    OSDD_MUSIC_ENABLE,
    OSDD_MUSIC_DISABLE,
    OSDD_MUSIC_LEFT,
    OSDD_MUSIC_RIGHT,
    OSDD_MUSIC_UP,
    OSDD_MUSIC_DOWN,
    OSDD_MUSIC_KEY_A
};

// The official daemon holds the command FIFO open for writing while it runs.
struct osdd_music_sender {
    const char *path;
    int fd;
};

int osdd_music_reader_init(struct osdd_music_reader *reader, const char *path);
enum osdd_music_result osdd_music_reader_poll(struct osdd_music_reader *reader,
                                               char *error, size_t error_size);
void osdd_music_reader_close(struct osdd_music_reader *reader);

void osdd_music_sender_init(struct osdd_music_sender *sender, const char *path);
// Opens the FIFO if a reader has it open; otherwise the next send retries.
int osdd_music_sender_open(struct osdd_music_sender *sender, char *error, size_t error_size);
// Requires SIGPIPE to be ignored, so a vanished reader reports EPIPE.
int osdd_music_send(struct osdd_music_sender *sender, enum osdd_music_command command,
                    char *error, size_t error_size);
void osdd_music_sender_close(struct osdd_music_sender *sender);

#endif
