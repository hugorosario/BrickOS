#ifndef BRICKOS_UNINSTALL_STAGE_H
#define BRICKOS_UNINSTALL_STAGE_H

#include "model.h"

// Confirming the uninstall. Device paths are below root, "" on the device.
#define UNINSTALL_START_SCRIPT "/mnt/SDCARD/System/starts/0-brickos-uninstall.sh"
#define UNINSTALL_DEFAULT_REBOOT "reboot"

// Stages the one-shot uninstall start script: copies uninstall.sh from source_dir, the app's
// folder, to the start script's name with .brickos-tmp, fsyncs it, sets BrickOS/uninstall-result
// aside, renames the script into place, then removes the previous result. On any failure the
// temporary file goes and the previous result comes back, so the card is as it was; the error
// says why.
enum uninstall_error uninstall_stage(const char *root, const char *source_dir);

// Runs sync, then the reboot command through /bin/sh, as the stock Settings reboot does.
// Returns 0 when the command ran and exited with status 0.
int uninstall_restart(const char *command);

// What activating Uninstall does: stages the script, calls staged(context) so the caller can
// show the restarting screen, then restarts with reboot_command (NULL: reboot). The reboot
// command never runs when staging fails. Returns UNINSTALL_ERROR_NONE once the reboot command
// has run, or the error.
enum uninstall_error uninstall_confirm(const char *root, const char *source_dir, const char *reboot_command,
                                       void (*staged)(void *), void *context);

#endif
