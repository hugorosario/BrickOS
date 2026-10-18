#!/bin/sh
# BrickOS LED daemon start script
# Installed by scripts/ledd-install.sh and removed by its rollback. The stock firmware runs
# /mnt/SDCARD/System/starts/*.sh once per boot before its main loop, waiting for each script.
# The launcher returns once the daemon runs, or at once when it cannot start; without the
# daemon the LEDs behave as on a stock boot.
# Creating @INSTALL_DIR@/DISABLED turns the daemon off for the next boot.
dir=@INSTALL_DIR@
[ -f "$dir/DISABLED" ] && exit 0
[ -f "$dir/launch.sh" ] || exit 0
sh "$dir/launch.sh" </dev/null >/dev/null 2>&1
exit 0
