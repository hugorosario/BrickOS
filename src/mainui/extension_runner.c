#define _POSIX_C_SOURCE 200809L
#include "extension_runner.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

extern char **environ;

static uint64_t monotonic_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

void mainui_ext_runner_init(struct mainui_ext_runner *r, const struct mainui_ext_runner_ops *ops) {
    memset(r, 0, sizeof(*r));
    if (ops) r->ops = *ops;
    r->next_id = 1;
    snprintf(r->language, sizeof(r->language), "en");
    for (int i = 0; i < MAINUI_EXT_RUNNER_JOBS; ++i) r->jobs[i].out_fd = r->jobs[i].in_fd = -1;
}

void mainui_ext_runner_set_language(struct mainui_ext_runner *r, const char *code) {
    snprintf(r->language, sizeof(r->language), "%s", code && code[0] ? code : "en");
}

void mainui_ext_runner_set_dry(struct mainui_ext_runner *r, FILE *record) {
    r->dry = 1;
    r->record = record;
}

struct mainui_ext_job *mainui_ext_runner_job(struct mainui_ext_runner *r, int id) {
    for (int i = 0; i < MAINUI_EXT_RUNNER_JOBS; ++i)
        if (id > 0 && r->jobs[i].id == id) return &r->jobs[i];
    return NULL;
}

int mainui_ext_runner_active(const struct mainui_ext_runner *r) {
    int n = 0;
    for (int i = 0; i < MAINUI_EXT_RUNNER_JOBS; ++i) n += r->jobs[i].id != 0;
    return n;
}

static int running(const struct mainui_ext_runner *r) {
    int n = 0;
    for (int i = 0; i < MAINUI_EXT_RUNNER_JOBS; ++i) n += r->jobs[i].id && r->jobs[i].pid > 0;
    return n;
}

int mainui_ext_runner_start(struct mainui_ext_runner *r, enum mainui_run_kind kind, int node, unsigned generation,
                        const char *dir, const char *command, const char *argument, const char *what, uint64_t now_ms) {
    char full[sizeof(r->jobs[0].command)];
    if (snprintf(full, sizeof(full), "%s%s%s", command, argument ? " " : "", argument ? argument : "") >= (int)sizeof(full))
        return -1;
    if (r->dry) {
        if (r->record) {
            fprintf(r->record, "extension %s %s %s\n", kind == MAINUI_RUN_LOAD ? "load" : "execute", what, full);
            fflush(r->record);
        }
        return 0;
    }
    struct mainui_ext_job *job = NULL;
    for (int i = 0; i < MAINUI_EXT_RUNNER_JOBS && !job; ++i)
        if (!r->jobs[i].id) job = &r->jobs[i];
    if (!job) return -1;
    memset(job, 0, sizeof(*job));
    job->id = r->next_id++;
    if (r->next_id <= 0) r->next_id = 1;
    job->kind = kind;
    job->node = node;
    job->generation = generation;
    job->out_fd = job->in_fd = -1;
    job->started_ms = now_ms;
    snprintf(job->what, sizeof(job->what), "%s", what);
    memcpy(job->command, full, sizeof(full));
    snprintf(job->dir, sizeof(job->dir), "%s", dir);
    return job->id;
}

void mainui_ext_runner_keep(struct mainui_ext_job *job, const char *text) {
    size_t n = strlen(text);
    if (!job->tail) {
        job->tail = malloc(MAINUI_EXT_RUNNER_TAIL);
        if (!job->tail) return;
        job->tail_length = 0;
    }
    if (n + 1 > MAINUI_EXT_RUNNER_TAIL) {
        text += n - (MAINUI_EXT_RUNNER_TAIL - 1);
        n = MAINUI_EXT_RUNNER_TAIL - 1;
    }
    // Drop the oldest lines to make room.
    if (job->tail_length + n + 1 > MAINUI_EXT_RUNNER_TAIL) {
        size_t drop = job->tail_length + n + 1 - MAINUI_EXT_RUNNER_TAIL;
        char *newline = memchr(job->tail + drop, '\n', job->tail_length - drop);
        if (newline) drop = (size_t)(newline - job->tail) + 1;
        if (drop > job->tail_length) drop = job->tail_length;
        memmove(job->tail, job->tail + drop, job->tail_length - drop);
        job->tail_length -= drop;
    }
    memcpy(job->tail + job->tail_length, text, n);
    job->tail_length += n;
    job->tail[job->tail_length] = 0;
}

static void close_fds(struct mainui_ext_job *job) {
    if (job->out_fd >= 0) close(job->out_fd);
    if (job->in_fd >= 0) close(job->in_fd);
    job->out_fd = job->in_fd = -1;
}

static void free_job(struct mainui_ext_job *job) {
    close_fds(job);
    free(job->tail);
    memset(job, 0, sizeof(*job));
    job->out_fd = job->in_fd = -1;
}

static void finish(struct mainui_ext_runner *r, struct mainui_ext_job *job) {
    if (r->ops.exit) r->ops.exit(r->ops.context, job);
    free_job(job);
}

// The environment for scripts, built before fork: the frontend's, with BRICKOS_LANGUAGE set.
static char **script_environment(struct mainui_ext_runner *r, char *language, size_t size) {
    size_t count = 0;
    for (char **e = environ; e && *e; ++e) ++count;
    char **env = malloc((count + 2) * sizeof(*env));
    if (!env) return NULL;
    size_t n = 0;
    for (char **e = environ; e && *e; ++e)
        if (strncmp(*e, "BRICKOS_LANGUAGE=", 17) != 0) env[n++] = *e;
    snprintf(language, size, "BRICKOS_LANGUAGE=%s", r->language);
    env[n++] = language;
    env[n] = NULL;
    return env;
}

static int spawn(struct mainui_ext_runner *r, struct mainui_ext_job *job) {
    int out[2], in[2];
    if (pipe(out) < 0) return -1;
    if (pipe(in) < 0) {
        close(out[0]);
        close(out[1]);
        return -1;
    }
    char language[64];
    char **env = script_environment(r, language, sizeof(language));
    long open_max = sysconf(_SC_OPEN_MAX);
    int last_fd = open_max > 0 && open_max < 4096 ? (int)open_max : 4096;
    char *argv[] = { "sh", "-c", job->command, NULL };
    pid_t pid = env ? fork() : -1;
    if (pid == 0) {
        // Only async-signal-safe calls until exec: the frontend has other threads.
        setpgid(0, 0);
        struct sigaction dfl;
        memset(&dfl, 0, sizeof(dfl));
        dfl.sa_handler = SIG_DFL;
        for (int s = 1; s < 32; ++s) sigaction(s, &dfl, NULL);
        sigset_t none;
        sigemptyset(&none);
        sigprocmask(SIG_SETMASK, &none, NULL);
        dup2(in[0], 0);
        dup2(out[1], 1);
        dup2(out[1], 2);
        for (int fd = 3; fd < last_fd; ++fd) close(fd);
        if (chdir(job->dir) < 0) {
            static const char message[] = "cannot enter the extension folder\n";
            ssize_t ignored = write(2, message, sizeof(message) - 1);
            (void)ignored;
            _exit(127);
        }
        execve("/bin/sh", argv, env);
        _exit(127);
    }
    free(env);
    close(out[1]);
    close(in[0]);
    if (pid < 0) {
        close(out[0]);
        close(in[1]);
        return -1;
    }
    // Also here, so the group exists before any signal the frontend sends it.
    setpgid(pid, pid);
    fcntl(out[0], F_SETFL, fcntl(out[0], F_GETFL) | O_NONBLOCK);
    fcntl(in[1], F_SETFL, fcntl(in[1], F_GETFL) | O_NONBLOCK);
    fcntl(out[0], F_SETFD, FD_CLOEXEC);
    fcntl(in[1], F_SETFD, FD_CLOEXEC);
    job->pid = pid;
    job->out_fd = out[0];
    job->in_fd = in[1];
    return 0;
}

static void emit_line(struct mainui_ext_runner *r, struct mainui_ext_job *job) {
    if (job->line_length && job->line[job->line_length - 1] == '\r') --job->line_length;
    job->line[job->line_length] = 0;
    job->line_length = 0;
    if (r->ops.line) r->ops.line(r->ops.context, job, job->line);
}

// Reads what the pipe holds; returns 1 when a line was handed over.
static int drain(struct mainui_ext_runner *r, struct mainui_ext_job *job) {
    int lines = 0;
    char buffer[4096];
    while (job->out_fd >= 0) {
        ssize_t n = read(job->out_fd, buffer, sizeof(buffer));
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        for (ssize_t i = 0; i < n; ++i) {
            if (buffer[i] == '\n') { emit_line(r, job); ++lines; continue; }
            job->line[job->line_length++] = buffer[i];
            if (job->line_length == sizeof(job->line) - 1) { emit_line(r, job); ++lines; }
        }
    }
    return lines > 0;
}

static void signal_group(struct mainui_ext_runner *r, struct mainui_ext_job *job, uint64_t now_ms) {
    if (job->pid <= 0 || job->term_ms) return;
    kill(-job->pid, SIGTERM);
    job->term_ms = now_ms ? now_ms : 1;
    for (int i = 0; i < MAINUI_EXT_RUNNER_JOBS; ++i)
        if (!r->ending[i].group) {
            r->ending[i].group = job->pid;
            r->ending[i].kill_ms = job->term_ms + MAINUI_EXT_RUNNER_KILL_MS;
            break;
        }
}

// SIGKILL for the groups whose 2 s have passed, or for all of them.
static void kill_due(struct mainui_ext_runner *r, uint64_t now_ms, int all) {
    for (int i = 0; i < MAINUI_EXT_RUNNER_JOBS; ++i) {
        if (!r->ending[i].group) continue;
        if (kill(-r->ending[i].group, 0) < 0 && errno == ESRCH) { r->ending[i].group = 0; continue; }
        if (all || now_ms >= r->ending[i].kill_ms) {
            kill(-r->ending[i].group, SIGKILL);
            r->ending[i].group = 0;
        }
    }
}

// Reaps an exited script; returns 1 when it ended.
static int reap(struct mainui_ext_runner *r, struct mainui_ext_job *job, int block) {
    int status;
    pid_t got;
    do got = waitpid(job->pid, &status, block ? 0 : WNOHANG);
    while (got < 0 && errno == EINTR);
    if (got == 0) return 0;
    job->status = got < 0 ? 127 : WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
    drain(r, job);
    if (job->line_length) emit_line(r, job);
    finish(r, job);
    return 1;
}

int mainui_ext_runner_poll(struct mainui_ext_runner *r, uint64_t now_ms) {
    int changed = 0;
    // Start queued scripts: executes first, then loads, each in the order they came.
    for (int pass = 0; pass < 2; ++pass) {
        for (;;) {
            if (running(r) >= MAINUI_EXT_RUNNER_RUNNING) break;
            struct mainui_ext_job *next = NULL;
            for (int i = 0; i < MAINUI_EXT_RUNNER_JOBS; ++i) {
                struct mainui_ext_job *j = &r->jobs[i];
                if (!j->id || j->pid > 0 || (pass == 0) != (j->kind != MAINUI_RUN_LOAD)) continue;
                if (!next || j->id < next->id) next = j;
            }
            if (!next) break;
            next->started_ms = now_ms;
            next->deadline_ms = next->kind == MAINUI_RUN_LOAD ? now_ms + MAINUI_EXT_RUNNER_LOAD_MS
                              : next->kind == MAINUI_RUN_VALUE ? now_ms + MAINUI_EXT_RUNNER_VALUE_MS : 0;
            if (spawn(r, next) < 0) {
                next->status = 127;
                finish(r, next);
                changed = 1;
            }
        }
    }
    for (int i = 0; i < MAINUI_EXT_RUNNER_JOBS; ++i) {
        struct mainui_ext_job *job = &r->jobs[i];
        if (!job->id || job->pid <= 0) continue;
        changed |= drain(r, job);
        if (!job->id) continue;
        if (reap(r, job, 0)) { changed = 1; continue; }
        if (job->deadline_ms && now_ms >= job->deadline_ms && !job->term_ms) {
            job->timed_out = 1;
            signal_group(r, job, now_ms);
        }
    }
    kill_due(r, now_ms, 0);
    return changed;
}

int mainui_ext_runner_cancel(struct mainui_ext_runner *r, int id, uint64_t now_ms) {
    struct mainui_ext_job *job = mainui_ext_runner_job(r, id);
    if (!job) return -1;
    job->cancelled = 1;
    if (job->pid <= 0) {
        job->status = 128 + SIGTERM;
        finish(r, job);
        return 0;
    }
    signal_group(r, job, now_ms);
    return 0;
}

int mainui_ext_runner_answer(struct mainui_ext_runner *r, int id, char answer) {
    struct mainui_ext_job *job = mainui_ext_runner_job(r, id);
    if (!job || job->in_fd < 0) return -1;
    char text[2] = { answer, '\n' };
    // A script that already ended must not stop the frontend with SIGPIPE.
    void (*previous)(int) = signal(SIGPIPE, SIG_IGN);
    ssize_t n = write(job->in_fd, text, sizeof(text));
    signal(SIGPIPE, previous);
    return n == (ssize_t)sizeof(text) ? 0 : -1;
}

void mainui_ext_runner_stop_all(struct mainui_ext_runner *r) {
    uint64_t start = monotonic_ms();
    for (int i = 0; i < MAINUI_EXT_RUNNER_JOBS; ++i) {
        struct mainui_ext_job *job = &r->jobs[i];
        if (!job->id) continue;
        job->cut_short = 1;
        if (job->pid <= 0) {
            job->status = 128 + SIGTERM;
            finish(r, job);
        } else {
            signal_group(r, job, start);
        }
    }
    for (;;) {
        uint64_t now = monotonic_ms();
        int left = 0;
        for (int i = 0; i < MAINUI_EXT_RUNNER_JOBS; ++i) {
            struct mainui_ext_job *job = &r->jobs[i];
            if (!job->id) continue;
            drain(r, job);
            if (!reap(r, job, 0)) ++left;
        }
        for (int i = 0; i < MAINUI_EXT_RUNNER_JOBS; ++i)
            if (r->ending[i].group && !(kill(-r->ending[i].group, 0) < 0 && errno == ESRCH)) ++left;
        if (!left) break;
        if (now - start >= MAINUI_EXT_RUNNER_KILL_MS) {
            kill_due(r, now, 1);
            for (int i = 0; i < MAINUI_EXT_RUNNER_JOBS; ++i) {
                struct mainui_ext_job *job = &r->jobs[i];
                if (job->id && job->pid > 0) {
                    kill(-job->pid, SIGKILL);
                    reap(r, job, 1);
                }
            }
            break;
        }
        struct timespec pause = { 0, 20 * 1000000L };
        nanosleep(&pause, NULL);
    }
    memset(r->ending, 0, sizeof(r->ending));
}
