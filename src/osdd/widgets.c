#define _POSIX_C_SOURCE 200809L
#include "widgets.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

void osdd_widget_runner_init(struct osdd_widget_runner *runner, const char *root) {
    if (runner == NULL) return;
    memset(runner, 0, sizeof(*runner));
    runner->root = root;
    runner->pid = -1;
    runner->timeout_ms = OSDD_WIDGET_TIMEOUT_MS;
}

static int plain_name(const char *name, size_t capacity) {
    if (name == NULL || name[0] == 0 || strlen(name) >= capacity || strstr(name, "..") != NULL)
        return 0;
    for (const char *c = name; *c != 0; ++c)
        if (*c == '/' || (unsigned char)*c < 32) return 0;
    return 1;
}

int osdd_widget_queue(struct osdd_widget_runner *runner, const char *directory,
                      const char *script, const char *argument) {
    if (runner == NULL || runner->count == OSDD_WIDGET_QUEUE ||
        !plain_name(directory, sizeof(runner->queue[0].directory)) ||
        !plain_name(script, sizeof(runner->queue[0].script)) ||
        (argument != NULL && argument[0] != 0 && !plain_name(argument, sizeof(runner->queue[0].argument))))
        return -1;
    struct osdd_widget_job *job = &runner->queue[(runner->head + runner->count) % OSDD_WIDGET_QUEUE];
    snprintf(job->directory, sizeof(job->directory), "%s", directory);
    snprintf(job->script, sizeof(job->script), "%s", script);
    snprintf(job->argument, sizeof(job->argument), "%s", argument == NULL ? "" : argument);
    ++runner->count;
    return 0;
}

static void start_next(struct osdd_widget_runner *runner, uint64_t now_ms) {
    while (runner->pid < 0 && runner->count > 0) {
        runner->current = runner->queue[runner->head];
        runner->head = (runner->head + 1) % OSDD_WIDGET_QUEUE;
        --runner->count;
        char directory[256], script[320];
        snprintf(directory, sizeof(directory), "%s/%s", runner->root, runner->current.directory);
        snprintf(script, sizeof(script), "%s/%s", directory, runner->current.script);
        if (access(script, R_OK) != 0) {
            fprintf(stderr, "widget script missing: %s\n", script);
            continue;
        }
        pid_t pid = fork();
        if (pid < 0) {
            fprintf(stderr, "widget script %s: %s\n", script, strerror(errno));
            return;
        }
        if (pid == 0) {
            // The daemon ignores SIGPIPE for the music FIFO; scripts get the default back.
            signal(SIGPIPE, SIG_DFL);
            int null = open("/dev/null", O_RDWR);
            if (null >= 0) { dup2(null, 0); dup2(null, 1); dup2(null, 2); }
            if (chdir(directory) < 0) _exit(126);
            if (runner->current.argument[0] != 0)
                execl("/bin/sh", "sh", script, runner->current.argument, (char *)NULL);
            else
                execl("/bin/sh", "sh", script, (char *)NULL);
            _exit(127);
        }
        runner->pid = pid;
        runner->started_ms = now_ms;
    }
}

int osdd_widget_run(struct osdd_widget_runner *runner, uint64_t now_ms) {
    if (runner == NULL) return 0;
    int ended = 0;
    if (runner->pid > 0) {
        int status;
        pid_t done = waitpid(runner->pid, &status, WNOHANG);
        if (done == runner->pid) {
            runner->pid = -1;
            ended = 1;
        } else if (now_ms - runner->started_ms >= runner->timeout_ms) {
            fprintf(stderr, "widget script timed out: %s/%s\n", runner->current.directory,
                    runner->current.script);
            kill(runner->pid, SIGKILL);
            waitpid(runner->pid, &status, 0);
            runner->pid = -1;
            ++runner->timeouts;
            ended = 1;
        }
    }
    start_next(runner, now_ms);
    return ended;
}

int osdd_widget_idle(const struct osdd_widget_runner *runner) {
    return runner == NULL || (runner->pid < 0 && runner->count == 0);
}

void osdd_widget_runner_close(struct osdd_widget_runner *runner) {
    if (runner == NULL || runner->pid <= 0) return;
    for (int attempt = 0; attempt < 20; ++attempt) {
        if (waitpid(runner->pid, NULL, WNOHANG) == runner->pid) {
            runner->pid = -1;
            return;
        }
        struct timespec pause = {.tv_sec = 0, .tv_nsec = 50000000};
        nanosleep(&pause, NULL);
    }
    kill(runner->pid, SIGKILL);
    waitpid(runner->pid, NULL, 0);
    runner->pid = -1;
}
