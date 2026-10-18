#!/bin/sh
# BrickOS uninstall app
# The Uninstall BrickOS app in /mnt/SDCARD/Apps/BrickOSUninstall/. The line above marks this
# folder as BrickOS's: the setup installs and updates it from the release package, removes it
# on rollback, and refuses a folder of this name without that line. The program shows the
# confirmation screen; cancelling exits to MainUI with nothing changed, and confirming stages
# the uninstall start script and restarts the Brick.
cd "$(dirname "$0")" || exit 0
exec ./brickos-uninstall
