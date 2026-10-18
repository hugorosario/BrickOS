#ifndef BRICKOS_OSDD_TOAST_H
#define BRICKOS_OSDD_TOAST_H

#include "look.h"

#include <stddef.h>
#include <stdint.h>
#include <sys/stat.h>

#define OSDD_TOAST_MAX_BYTES 8192

struct osdd_profile;

struct osdd_toast {
    // The Stock++ theme profile active when consumed, or NULL for stock defaults/BrickOS.
    // The parser leaves it NULL; it is not part of the record.
    const struct osdd_profile *profile;
    // Internal frontend snapshot, never part of the producer protocol.
    int material;
    struct osdd_look look;
    char type[48];
    char id[192];
    char message[2048];
    char font[256];
    char bg[512];
    char icon[512];
    char fontcolor[32];
    int duration;
    int size;
    int x;
    int y;
    int w;
    int h;
    int fontsize;
};

enum osdd_poll_result {
    OSDD_POLL_ERROR = -1,
    OSDD_POLL_UNCHANGED = 0, // No record to consume.
    OSDD_POLL_UPDATE = 1,    // A valid record was consumed.
    OSDD_POLL_DISCARDED = 2  // A malformed or incomplete record was consumed.
};

// Consumes records from one producer path as the official daemon does: each file is
// read once and then deleted.
struct osdd_toast_reader {
    const char *path;
    void (*after_read)(void *context); // Test hook between the read and the delete.
    void *after_read_context;
    struct stat kept; // A consumed record that could not be deleted is not replayed.
    int has_kept;
};

int osdd_toast_parse(const unsigned char *data, size_t length,
                     struct osdd_toast *result, char *error, size_t error_size);
enum osdd_poll_result osdd_toast_reader_poll(struct osdd_toast_reader *reader,
                                              struct osdd_toast *result,
                                              char *error, size_t error_size);

#endif
