#!/bin/sh
# BrickOS GameSwitcher app
set -eu
sd=${BRICKOS_SD_ROOT:-/mnt/SDCARD}
exec sh "$sd/BrickOS/gameswitcher/entry.sh" apps
