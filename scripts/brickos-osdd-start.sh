#!/bin/sh
# BrickOS OSD start script
# Installed by scripts/osdd-install.sh and removed by its rollback. The stock firmware
# runs /mnt/SDCARD/System/starts/*.sh once per boot before its main loop, waiting for
# each script. The launcher runs in the foreground and returns once BrickOS runs, or at
# once when BrickOS cannot start. While a process named trimui_osdd runs, the loop does
# not start another OSD; otherwise it starts the official one.
# Creating @INSTALL_DIR@/DISABLED turns BrickOS off for the next boot.
dir=@INSTALL_DIR@
[ -f "$dir/DISABLED" ] && exit 0
[ -f "$dir/launch.sh" ] || exit 0
sh "$dir/launch.sh" </dev/null >/dev/null 2>&1
exit 0
