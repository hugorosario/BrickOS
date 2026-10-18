#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L
#include "stage.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#define PATH_BYTES 1024

static enum uninstall_error error_of(int error) {
    switch (error) {
    case ENOSPC:
#ifdef EDQUOT
    case EDQUOT:
#endif
        return UNINSTALL_ERROR_CARD_FULL;
    case EROFS:
    case EACCES:
    case EPERM:
        return UNINSTALL_ERROR_READ_ONLY;
    default:
        return UNINSTALL_ERROR_WRITE;
    }
}

static int joined(char *out, const char *a, const char *b, const char *c) {
    return snprintf(out, PATH_BYTES, "%s%s%s", a, b, c) < PATH_BYTES ? 0 : -1;
}

// Copies every byte of in to out; returns 0, or -1 with errno set.
static int copy(int in, int out) {
    char buffer[8192];
    for (;;) {
        ssize_t got = read(in, buffer, sizeof(buffer));
        if (got < 0 && errno == EINTR) continue;
        if (got < 0) return -1;
        if (got == 0) return 0;
        for (ssize_t done = 0; done < got;) {
            ssize_t put = write(out, buffer + done, (size_t)(got - done));
            if (put < 0 && errno == EINTR) continue;
            if (put < 0) return -1;
            done += put;
        }
    }
}

// Flushes a folder's entries, so a rename in it survives a power cut.
static void sync_folder(const char *file) {
    char folder[PATH_BYTES];
    snprintf(folder, sizeof(folder), "%s", file);
    char *slash = strrchr(folder, '/');
    if (!slash) return;
    *slash = 0;
    int fd = open(folder, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return;
    (void)fsync(fd);
    close(fd);
}

enum uninstall_error uninstall_stage(const char *root, const char *source_dir) {
    char source[PATH_BYTES], target[PATH_BYTES], temporary[PATH_BYTES], result[PATH_BYTES], aside[PATH_BYTES];
    if (!root) root = "";
    if (joined(source, source_dir, "/uninstall.sh", "") < 0 || joined(target, root, UNINSTALL_START_SCRIPT, "") < 0 ||
        joined(temporary, target, ".brickos-tmp", "") < 0 || joined(result, root, UNINSTALL_RESULT_FILE, "") < 0 ||
        joined(aside, result, ".brickos-old", "") < 0)
        return UNINSTALL_ERROR_WRITE;
    int in = open(source, O_RDONLY | O_CLOEXEC);
    if (in < 0) return UNINSTALL_ERROR_SCRIPT_MISSING;
    int out = open(temporary, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0755);
    if (out < 0) {
        enum uninstall_error error = error_of(errno);
        close(in);
        return error;
    }
    int failed = copy(in, out) < 0 || fsync(out) < 0;
    int error = errno;
    close(in);
    if (close(out) < 0 && !failed) { failed = 1; error = errno; }
    if (failed) {
        unlink(temporary);
        return error_of(error);
    }
    // The previous result is set aside first, so a failed rename can put it back.
    int had_result = rename(result, aside) == 0;
    if (!had_result && errno != ENOENT) {
        error = errno;
        unlink(temporary);
        return error_of(error);
    }
    if (rename(temporary, target) < 0) {
        error = errno;
        unlink(temporary);
        if (had_result) (void)rename(aside, result);
        return error_of(error);
    }
    if (had_result) unlink(aside);
    sync_folder(target);
    return UNINSTALL_ERROR_NONE;
}

int uninstall_restart(const char *command) {
    sync();
    pid_t child = fork();
    if (child < 0) return -1;
    if (child == 0) {
        execl("/bin/sh", "sh", "-c", command ? command : UNINSTALL_DEFAULT_REBOOT, (char *)NULL);
        _exit(127);
    }
    int status = 0;
    while (waitpid(child, &status, 0) < 0)
        if (errno != EINTR) return -1;
    return WIFEXITED(status) && WEXITSTATUS(status) == 0 ? 0 : -1;
}

enum uninstall_error uninstall_confirm(const char *root, const char *source_dir, const char *reboot_command,
                                       void (*staged)(void *), void *context) {
    enum uninstall_error error = uninstall_stage(root, source_dir);
    if (error != UNINSTALL_ERROR_NONE) return error;
    if (staged) staged(context);
    return uninstall_restart(reboot_command ? reboot_command : UNINSTALL_DEFAULT_REBOOT) == 0 ? UNINSTALL_ERROR_NONE
                                                                                             : UNINSTALL_ERROR_RESTART;
}
