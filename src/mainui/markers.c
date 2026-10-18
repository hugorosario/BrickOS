#define _POSIX_C_SOURCE 200809L
#include "markers.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int mainui_marker_write_to(const char *run, const char *token, enum mainui_marker marker) {
    if (!run || !run[0] || !token || !token[0]) return 0;
    // A token names one file; it must not reach outside the run directory.
    if (strchr(token, '/') || strcmp(token, ".") == 0 || strcmp(token, "..") == 0) return -1;
    char path[512], temporary[520];
    const char *suffix = marker == MAINUI_MARKER_READY ? "ready" : "done";
    if (snprintf(path, sizeof(path), "%s/%s.%s", run, token, suffix) >= (int)sizeof(path)) return -1;
    snprintf(temporary, sizeof(temporary), "%s.tmp", path);
    int fd = open(temporary, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) return -1;
    char text[32];
    int length = snprintf(text, sizeof(text), "%d\n", (int)getpid());
    int ok = write(fd, text, (size_t)length) == length;
    if (close(fd) != 0) ok = 0;
    if (!ok || rename(temporary, path) != 0) { unlink(temporary); return -1; }
    return 1;
}

int mainui_marker_write(enum mainui_marker marker) {
    return mainui_marker_write_to(getenv("BRICKOS_MAINUI_RUN"), getenv("BRICKOS_MAINUI_TOKEN"), marker);
}
