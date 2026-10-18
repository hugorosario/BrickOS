#ifndef BRICKOS_LEDD_HEALTH_H
#define BRICKOS_LEDD_HEALTH_H

// The LED daemon's reports, which MainUI reads. The health file holds key=value lines and is
// replaced by rename: state, mode, reason, pid, uptime_ms, writes, restores, errors.

#define LEDD_PROCESS_NAME "brickos-ledd"
#define LEDD_HEALTH_PATH "/tmp/brickos-ledd.health"
#define LEDD_APPLIED_PATH "/tmp/brickos-ledd.applied"

// state=
#define LEDD_STATE_ACTIVE "active"      // its pattern shows
#define LEDD_STATE_IDLE "idle"          // nothing to show: the mode leaves the LEDs to the firmware
#define LEDD_STATE_YIELDING "yielding"  // another owner has the LEDs for now
#define LEDD_STATE_STOPPED "stopped"    // ended by a stop request
#define LEDD_STATE_FAILED "failed"      // the launcher gave up after repeated crashes

// reason=, for idle and yielding.
#define LEDD_REASON_DEFAULT "default"
#define LEDD_REASON_FOLLOW_THEME "follow-theme"
#define LEDD_REASON_LED_OFF "led-off"
#define LEDD_REASON_STANDBY "standby"
#define LEDD_REASON_LOW_BATTERY "low-battery"
#define LEDD_REASON_BRICKTOOLS "bricktools"
#define LEDD_REASON_NO_CHARGER "no-charger"
#define LEDD_REASON_NO_READING "no-reading"
#define LEDD_REASON_INVALID_CONFIG "invalid-config"

#endif
