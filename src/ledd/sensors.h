#ifndef BRICKOS_LEDD_SENSORS_H
#define BRICKOS_LEDD_SENSORS_H

#include "policy.h"

// What the LED daemon reads besides the LED driver, every path under root ("" on the device),
// so host tests run against a fixture tree.

#define LEDD_POWER_DIR "/sys/class/power_supply"
#define LEDD_CPU_FREQ "/sys/devices/system/cpu/cpufreq/policy0/cpuinfo_cur_freq"
#define LEDD_TEMPERATURE "/sys/class/thermal/thermal_zone0/temp"
#define LEDD_SUSPEND_MARKER "/tmp/system_suspend"
#define LEDD_DISPLAY_STATE "/sys/devices/virtual/disp/disp/attr/sys"
#define LEDD_SYSTEM_JSON "/mnt/UDISK/system.json"
#define LEDD_BRICKTOOLS_DAEMON "led_daemon.sh"

// The first supply of type Battery: capacity and status.
void ledd_sensors_battery(const char *root, struct ledd_readings *readings, int capacity);
// The first supply of type USB or Mains: online.
void ledd_sensors_charger(const char *root, struct ledd_readings *readings);
void ledd_sensors_cpu(const char *root, struct ledd_readings *readings);
void ledd_sensors_temperature(const char *root, struct ledd_readings *readings);
// Standby: keymon's suspend marker together with backlight 0. Returns 1, 0, or 0 when unreadable.
int ledd_sensors_standby(const char *root);
// 1 while a process runs BrickTools' LED daemon, found by its command line under root/proc.
int ledd_sensors_bricktools(const char *root);
// ledpolicy from system.json: 1, 0, or -1 when it cannot be read.
int ledd_sensors_system_policy(const char *root);

#endif
