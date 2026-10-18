#ifndef BRICKOS_MAINUI_EXTENSION_RUNNER_H
#define BRICKOS_MAINUI_EXTENSION_RUNNER_H

#include <stdint.h>
#include <stdio.h>
#include <sys/types.h>

// Runs the scripts of settings extensions without blocking the frontend (docs/settings-extensions.md).
// Each script is `/bin/sh -c <command>` in its folder, in a process group of its own, with
// standard output and error on one pipe and standard input on another, and BRICKOS_LANGUAGE in
// its environment. The frontend polls the runner from its tick: complete output lines and exits
// come back through callbacks. Loads end after 5 s and value executes after 30 s; a command runs
// until it exits or is cancelled. Ending a script sends SIGTERM to its group and, 2 s later,
// SIGKILL to whatever of the group remains. At most four scripts run at once; executes start
// ahead of queued loads.

#define MAINUI_EXT_RUNNER_RUNNING 4
#define MAINUI_EXT_RUNNER_JOBS 64
#define MAINUI_EXT_RUNNER_LOAD_MS 5000
#define MAINUI_EXT_RUNNER_VALUE_MS 30000
#define MAINUI_EXT_RUNNER_KILL_MS 2000
#define MAINUI_EXT_RUNNER_TAIL (64 * 1024)
#define MAINUI_EXT_RUNNER_LINE 4096

enum mainui_run_kind {
    MAINUI_RUN_LOAD,   // a row's load: 5 s
    MAINUI_RUN_VALUE,  // a toggle's, select's or range's execute: 30 s
    MAINUI_RUN_CMD,    // a command's execute: no limit, cancellable
};

struct mainui_ext_job {
    int id;                      // 0 for a free slot
    enum mainui_run_kind kind;
    int node;                    // the registry node the script belongs to
    unsigned generation;         // the registry generation it was started for
    char what[256];              // "<file> <row>", for the log and the diagnostic record
    char command[2048];          // with the value argument appended
    char dir[1024];
    pid_t pid;                   // 0 while queued
    int out_fd, in_fd;
    uint64_t started_ms, deadline_ms, term_ms;  // deadline 0: none; term_ms: when SIGTERM was sent
    int cancelled, timed_out, cut_short;        // why it was ended
    int status;                  // after the exit: the exit code, or 128 + the signal
    char line[MAINUI_EXT_RUNNER_LINE];
    size_t line_length;
    char *tail;                  // visible output the frontend kept, the newest 64 KiB
    size_t tail_length;
};

struct mainui_ext_runner_ops {
    void *context;
    // A complete line of output, without its newline.
    void (*line)(void *context, struct mainui_ext_job *job, const char *line);
    // The script ended. The job is freed when this returns.
    void (*exit)(void *context, struct mainui_ext_job *job);
};

struct mainui_ext_runner {
    struct mainui_ext_runner_ops ops;
    struct mainui_ext_job jobs[MAINUI_EXT_RUNNER_JOBS];
    struct { pid_t group; uint64_t kill_ms; } ending[MAINUI_EXT_RUNNER_JOBS];  // SIGKILL due for these groups
    int next_id;
    char language[32];
    int dry;
    FILE *record;
};

void mainui_ext_runner_init(struct mainui_ext_runner *r, const struct mainui_ext_runner_ops *ops);
void mainui_ext_runner_set_language(struct mainui_ext_runner *r, const char *code);
// A dry runner starts nothing and writes "extension load|execute <what> <command>" to record.
void mainui_ext_runner_set_dry(struct mainui_ext_runner *r, FILE *record);
// Queues a script; argument, when not NULL, is appended after a space. Returns the job's id, 0
// in a dry run, or -1 when every slot is taken.
int mainui_ext_runner_start(struct mainui_ext_runner *r, enum mainui_run_kind kind, int node, unsigned generation,
                        const char *dir, const char *command, const char *argument, const char *what, uint64_t now_ms);
// Starts queued scripts, reads output, enforces time limits and reaps exits. Returns 1 when a
// callback ran.
int mainui_ext_runner_poll(struct mainui_ext_runner *r, uint64_t now_ms);
// Ends a job: a queued one is dropped at once (with its exit callback), a running one gets
// SIGTERM. Returns 0, or -1 when there is no such job.
int mainui_ext_runner_cancel(struct mainui_ext_runner *r, int id, uint64_t now_ms);
// Writes the answer to a confirm: prompt ('A' or 'B', then a newline) to the job's stdin.
int mainui_ext_runner_answer(struct mainui_ext_runner *r, int id, char answer);
// Keeps visible output in the job's tail for the output view.
void mainui_ext_runner_keep(struct mainui_ext_job *job, const char *text);
struct mainui_ext_job *mainui_ext_runner_job(struct mainui_ext_runner *r, int id);
// Jobs queued or running.
int mainui_ext_runner_active(const struct mainui_ext_runner *r);
// Ends every job before the frontend hands over: SIGTERM to each running group, up to 2 s for
// them to go, then SIGKILL. Each job's exit callback runs with cut_short set.
void mainui_ext_runner_stop_all(struct mainui_ext_runner *r);

#endif
