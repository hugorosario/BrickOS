#ifndef BRICKOS_INPUTD_POWER_SOURCE_H
#define BRICKOS_INPUTD_POWER_SOURCE_H

#include <stddef.h>

// The physical POWER key is the EV_KEY/KEY_POWER switch exposed by the PMIC's
// axp2202-pek evdev node. Identify it by device name and capability, never eventN.
struct inputd_power_source {
    int fd;
    int grabbed;
    char path[64];
};

void inputd_power_source_init(struct inputd_power_source *source);
// Returns 1 when the source is found, 0 while it is absent, -1 on an enumeration error.
// Initially-held input is reported so the engine can wait for a release before arming.
int inputd_power_source_open(struct inputd_power_source *source, int *initially_held, int grab,
                             char *error, size_t error_size);
// Change exclusive ownership of the physical POWER source.
int inputd_power_source_set_grab(struct inputd_power_source *source, int grab,
                                 char *error, size_t error_size);
// Drains events. Returns 1 if state changed, 0 if unchanged, -1 if lost.
int inputd_power_source_read(struct inputd_power_source *source, int *held,
                             char *error, size_t error_size);
void inputd_power_source_close(struct inputd_power_source *source);

// Returns 1 while a live MainUI process is running, 0 when none is present,
// or -1 when proc_root cannot be scanned. The caller should fail open to stock input.
int inputd_power_mainui_active(const char *proc_root);

// Returns 1 while a live BrickOS OSD daemon runs, the only consumer of the power-hold
// marker: a trimui_osdd whose executable lies in /mnt/SDCARD/BrickOS/osd/, or in a
// /tmp/brickos-osdd-session-* or /tmp/brickos-e2e-* directory. 0 when none runs, -1 when
// proc_root cannot be scanned.
int inputd_power_brickos_osd_active(const char *proc_root);

#endif
