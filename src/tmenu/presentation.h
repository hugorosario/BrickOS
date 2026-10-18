#ifndef BRICKOS_TMENU_PRESENTATION_H
#define BRICKOS_TMENU_PRESENTATION_H

#define TMENU_MAINUI_CURRENT "/mnt/SDCARD/BrickOS/mainui/current"
// Read anew at each menu open. Returns 1 for BrickOS, 0 for Stock++.
int tmenu_material_selected(const char *preferences, const char *current, const char *override);

// EmulationStation's program. EmulationStation writes the OSD theme profile and leaves it behind,
// so the profile counts only while this program runs.
#define TMENU_EMULATIONSTATION "/mnt/SDCARD/Apps/EmulationStation/emulationstation"
// 1 when a process in the process tree at proc (normally /proc) runs EmulationStation's program;
// 0 otherwise, or when the tree cannot be read.
int tmenu_emulationstation_running(const char *proc);

#endif
