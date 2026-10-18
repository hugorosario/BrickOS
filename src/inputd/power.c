#define _GNU_SOURCE
#include "power.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static unsigned marker_sequence;

void inputd_power_engine_init(struct inputd_power_engine *engine, int initially_held) {
    if (engine == NULL) return;
    memset(engine, 0, sizeof(*engine));
    engine->held = initially_held != 0;
    engine->armed = !engine->held;
    engine->suppressed = engine->held;
    engine->state = engine->held ? INPUTD_POWER_SOURCE_DISARMED : INPUTD_POWER_IDLE;
}

static int update(struct inputd_power_engine *engine, uint64_t now_ms,
                  int held, int suppress, int marker_present,int short_press) {
    if (engine == NULL) return 0;
    held = held != 0;
    if (!held) {
        int action=0;
        if (short_press && engine->held && engine->armed && !engine->suppressed &&
            !suppress && !marker_present && !engine->marker_blocked && !engine->fired &&
            engine->sequence && now_ms>=engine->pressed_at_ms) {
            action=now_ms-engine->pressed_at_ms>INPUTD_POWER_HOLD_MS?
                   INPUTD_GAME_POWER_LONG:INPUTD_GAME_POWER_SHORT;
        }
        engine->held = 0;
        engine->armed = 1;
        engine->suppressed = 0;
        engine->fired = 0;
        engine->marker_blocked = 0;
        engine->state = action?INPUTD_POWER_REQUEST_RAISED:INPUTD_POWER_IDLE;
        return action;
    }
    if (!engine->held) {
        if (engine->sequence==UINT64_MAX) {
            engine->armed=0; engine->suppressed=1;
        } else ++engine->sequence;
        engine->held = 1;
        engine->fired = 0;
        engine->marker_blocked = 0;
        engine->suppressed = !engine->armed || suppress;
        engine->pressed_at_ms = now_ms;
        engine->state = engine->suppressed ? INPUTD_POWER_SOURCE_DISARMED : INPUTD_POWER_HELD;
    }
    if (!engine->armed) {
        engine->suppressed = 1;
        engine->state = INPUTD_POWER_SOURCE_DISARMED;
        return 0;
    }
    if (suppress) engine->suppressed = 1;
    if (marker_present) engine->marker_blocked = 1;
    if (engine->suppressed) {
        engine->state = INPUTD_POWER_SUPPRESSED;
        return 0;
    }
    if (engine->fired) return 0;
    if (now_ms < engine->pressed_at_ms || now_ms - engine->pressed_at_ms <= INPUTD_POWER_HOLD_MS)
        return 0;

    engine->fired = 1;
    if (engine->marker_blocked) {
        engine->state = INPUTD_POWER_MARKER_BUSY;
        return 0;
    }
    engine->state = INPUTD_POWER_REQUEST_RAISED;
    return 1;
}

int inputd_power_engine_update(struct inputd_power_engine *engine,uint64_t now_ms,
                               int held,int suppress,int marker_present) {
    return update(engine,now_ms,held,suppress,marker_present,0);
}
enum inputd_game_power_action inputd_power_engine_update_game(struct inputd_power_engine *engine,
                              uint64_t now_ms,int held,int suppress,int pending) {
    return (enum inputd_game_power_action)update(engine,now_ms,held,suppress,pending,1);
}
int inputd_power_transaction(const struct inputd_power_engine *engine,const char *session,char transaction[33]) {
    if (!engine || !session || !transaction || strlen(session)!=32 ||
        strspn(session,"0123456789abcdef")!=32 || !engine->sequence) return -1;
    return snprintf(transaction,33,"%.16s%016llx",session,(unsigned long long)engine->sequence)==32?0:-1;
}

void inputd_power_engine_publish_result(struct inputd_power_engine *engine, int result) {
    if (engine == NULL) return;
    if (result < 0) engine->state = INPUTD_POWER_PUBLISH_FAILED;
    else if (result == 0) engine->state = INPUTD_POWER_MARKER_BUSY;
}

const char *inputd_power_state_name(enum inputd_power_state state) {
    switch (state) {
    case INPUTD_POWER_IDLE: return "idle";
    case INPUTD_POWER_HELD: return "held";
    case INPUTD_POWER_SUPPRESSED: return "suppressed";
    case INPUTD_POWER_MARKER_BUSY: return "marker-busy";
    case INPUTD_POWER_REQUEST_RAISED: return "request-raised";
    case INPUTD_POWER_PUBLISH_FAILED: return "publish-failed";
    case INPUTD_POWER_SOURCE_DISARMED: return "source-disarmed";
    default: return "unknown";
    }
}

int inputd_power_should_grab(int io_test, int mainui_active, int held, int currently_grabbed) {
    if (io_test || !mainui_active) return 0;
    if (currently_grabbed) return 1;
    return !held;
}
int inputd_game_power_should_grab(int io_test,int mainui_active,int game_valid,
                                  int held,int grabbed,int game_press) {
    if (io_test) return 0;
    if (game_press && held && grabbed) return 1;
    return inputd_power_should_grab(0,mainui_active || game_valid,held,grabbed);
}

static int write_all(int fd, const char *text, size_t length) {
    size_t offset = 0;
    while (offset < length) {
        ssize_t count = write(fd, text + offset, length - offset);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) return -1;
        offset += (size_t)count;
    }
    return 0;
}

int inputd_power_publish_marker(const char *directory) {
    if (directory == NULL || directory[0] != '/') { errno = EINVAL; return -1; }
    if (mkdir(directory, 0755) < 0 && errno != EEXIST) return -1;
    int dirfd = open(directory, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (dirfd < 0) return -1;
    struct stat st;
    if (fstat(dirfd, &st) < 0) {
        int saved = errno;
        close(dirfd); errno = saved; return -1;
    }
    if (!S_ISDIR(st.st_mode)) {
        int saved = ENOTDIR;
        close(dirfd); errno = saved; return -1;
    }
    if (fstatat(dirfd, "power-hold", &st, AT_SYMLINK_NOFOLLOW) == 0) {
        close(dirfd);
        return 0;
    }
    if (errno != ENOENT) { int saved = errno; close(dirfd); errno = saved; return -1; }

    char temporary[64];
    int fd = -1;
    for (int attempt = 0; attempt < 8; ++attempt) {
        snprintf(temporary, sizeof(temporary), ".power-hold.tmp.%ld.%u",
                 (long)getpid(), ++marker_sequence);
        fd = openat(dirfd, temporary, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0644);
        if (fd >= 0 || errno != EEXIST) break;
    }
    if (fd < 0) { int saved = errno; close(dirfd); errno = saved; return -1; }
    static const char payload[] = "power-off request\n";
    int result = write_all(fd, payload, sizeof(payload) - 1);
    if (result == 0 && fsync(fd) < 0) result = -1;
    int saved = errno;
    if (close(fd) < 0 && result == 0) { result = -1; saved = errno; }
    if (result == 0 && linkat(dirfd, temporary, dirfd, "power-hold", 0) < 0) {
        saved = errno;
        result = saved == EEXIST ? 0 : -1;
    } else if (result == 0) {
        result = 1;
    }
    if (unlinkat(dirfd, temporary, 0) < 0 && result >= 0) { result = -1; saved = errno; }
    if (result == 1 && fsync(dirfd) < 0) { result = -1; saved = errno; }
    close(dirfd);
    if (result < 0) errno = saved;
    return result == 1 ? 1 : result == 0 ? 0 : -1;
}
