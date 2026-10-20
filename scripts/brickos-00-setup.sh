#!/bin/sh
# BrickOS setup start script
# Part of a BrickOS release package. The firmware runs /mnt/SDCARD/System/starts/*.sh once per
# boot, in name order and waiting for each, before its daemon loop; this name sorts before the
# BrickOS component start scripts.
#
# With /mnt/SDCARD/BrickOS/enabled present, it first lets the installed updater install a
# verified online update (BrickOS/settings/Updates/boot.sh), then installs or updates BrickOS
# from the package the marker names: its online package in BrickOS/updates/packages/ when there
# is one, else BrickOS/package/<build>, else the only package on the card. Component start
# scripts that a setup created in this boot were not in this boot's list, so it runs them, and
# BrickOS runs in this boot; existing ones run from the list after this script. When the update
# was installed but its commit failed, the enabled build's setup is not run again in that boot.
# Without the marker it rolls BrickOS back with the copy of the setup in BrickOS/setup/ (or the
# package's setup without that copy), then removes the package and itself. When no package and no
# copy are left, it removes the BrickOS start scripts, the menu lines and the Uninstall BrickOS app
# it can identify by their marker, puts back GameSwitcher's launchers whose backup is still there,
# and removes GameSwitcher's app. A failed or refused setup leaves the stock boot to go on: the
# official daemons start as usual. Results go to /mnt/SDCARD/BrickOS/setup.log.
root=${BRICKOS_ROOT:-}
sd=$root/mnt/SDCARD
brickos=$sd/BrickOS
starts=$sd/System/starts
self=$starts/brickos-00-setup.sh
out=$root/tmp/brickos-00-setup.out

note() {
    [ -d "$brickos" ] || return 0
    echo "== $(date '+%Y-%m-%d %H:%M:%S %Z') boot script: $*" >> "$brickos/setup.log"
}
result() { sed -n 's/^result=//p' "$out" 2>/dev/null | tail -n 1; }

# The component start scripts before this boot's setups: one a setup creates now is not in the
# firmware's list for this boot, so this script runs it.
had_inputd=0; [ -f "$starts/brickos-inputd.sh" ] && had_inputd=1
had_osdd=0; [ -f "$starts/brickos-osdd.sh" ] && had_osdd=1
had_ledd=0; [ -f "$starts/brickos-ledd.sh" ] && had_ledd=1
had_mainui=0; [ -f "$starts/brickos-mainui.sh" ] && had_mainui=1
start_new() {
    [ "$had_inputd" = 0 ] && [ -f "$starts/brickos-inputd.sh" ] && sh "$starts/brickos-inputd.sh"
    [ "$had_osdd" = 0 ] && [ -f "$starts/brickos-osdd.sh" ] && sh "$starts/brickos-osdd.sh"
    [ "$had_ledd" = 0 ] && [ -f "$starts/brickos-ledd.sh" ] && sh "$starts/brickos-ledd.sh"
    [ "$had_mainui" = 0 ] && [ -f "$starts/brickos-mainui.sh" ] && sh "$starts/brickos-mainui.sh"
    return 0
}

# Online updates are already downloaded and confirmed. Activate them before any component is
# started; the regular setup below remains the installer and fallback for the selected build.
# activated: yes when the update was installed and committed (the marker names its build),
# kept when it was installed but its commit failed (update-boot.sh exits 3).
activated=
if [ -f "$brickos/enabled" ] && [ -f "$brickos/settings/Updates/boot.sh" ] &&
    [ -x "$brickos/settings/Updates/brickos-update" ]; then
    before=$(head -n 1 "$brickos/enabled" 2>/dev/null)
    sh "$brickos/settings/Updates/boot.sh"
    case $? in
        0) [ "$(head -n 1 "$brickos/enabled" 2>/dev/null)" = "$before" ] || activated=yes ;;
        3) activated=kept
            note "online update installed; its commit is retried at the next boot, and the enabled build is not set up again" ;;
        *) note "online update was not activated; continuing with the selected package" ;;
    esac
fi

# The package folders on the card, one per line.
packages() { for dir in "$brickos"/package/*/; do [ -f "$dir/setup.sh" ] && echo "${dir%/}"; done; }

# Rolls back without the package: the installed copy of the setup if there is one, otherwise
# what can be identified by marker.
rollback_without_package() {
    local file
    if [ -f "$brickos/setup/setup.sh" ]; then
        sh "$brickos/setup/setup.sh" rollback --boot > "$out" 2>&1
        [ "$(result)" = rolled-back ]
        return
    fi
    for file in "$starts/brickos-inputd.sh" "$starts/brickos-osdd.sh" "$starts/brickos-ledd.sh" "$starts/brickos-mainui.sh"; do
        grep -qx -e '# BrickOS input daemon start script' -e '# BrickOS OSD start script' \
            -e '# BrickOS LED daemon start script' -e '# BrickOS frontend start script' "$file" 2>/dev/null &&
            rm -f "$file"
    done
    # A GameSwitcher launcher gets its stock bytes back from its backup; without the backup it
    # stays, and the log names it.
    for file in "$brickos"/gameswitcher/launchers/*.record; do
        [ -f "$file" ] || continue
        launcher=$root$(sed -n 's/^launcher=//p' "$file")
        grep -qx '# BrickOS GameSwitcher launcher' "$launcher" 2>/dev/null || continue
        cp "${file%.record}.backup" "$launcher.brickos-tmp" 2>/dev/null && mv "$launcher.brickos-tmp" "$launcher" ||
            { rm -f "$launcher.brickos-tmp"; note "GameSwitcher's launcher ${launcher#"$root"} has no backup left; restore it by hand"; }
    done
    grep -qx '# BrickOS GameSwitcher app' "$sd/Apps/GameSwitcher/launch.sh" 2>/dev/null && rm -rf "$sd/Apps/GameSwitcher"
    for file in "$sd"/Emus/*/launch*.sh; do
        grep -q '# BrickOS in-game menu$' "$file" 2>/dev/null || continue
        sed 's|^LD_LIBRARY_PATH=/mnt/SDCARD/BrickOS/tmenu/[0-9a-f]*:\$LD_LIBRARY_PATH \(HOME=.*\) # BrickOS in-game menu$|\1|' \
            "$file" > "$file.brickos-tmp" && mv "$file.brickos-tmp" "$file"
    done
    grep -qx '# BrickOS uninstall app' "$sd/Apps/BrickOSUninstall/launch.sh" 2>/dev/null &&
        rm -rf "$sd/Apps/BrickOSUninstall"
    note "no package and no installed setup; removed the start scripts, menu lines and app marked as BrickOS's"
    return 0
}

mkdir -p "$root/tmp"
if [ -f "$brickos/enabled" ]; then
    build=$(head -n 1 "$brickos/enabled" 2>/dev/null | tr -cd '0-9a-f')
    package=
    online=$brickos/updates/packages/$build/BrickOS/package/$build
    if [ -n "$build" ] && [ -f "$online/setup.sh" ]; then
        package=$online
    elif [ -n "$build" ] && [ -f "$brickos/package/$build/setup.sh" ]; then
        package=$brickos/package/$build
    elif [ "$(packages | wc -l)" = 1 ]; then
        package=$(packages)
    fi
    if [ "$activated" = kept ]; then
        # The update's build is installed and the marker still names the previous one: setting
        # that one up again would undo the update, so this boot keeps the update.
        start_new
    elif [ -n "$package" ]; then
        sh "$package/setup.sh" install --boot > "$out" 2>&1
        case "$(result)" in
            installed|updated) start_new ;;
            # The online update's setup created the start scripts before this run.
            up-to-date) [ "$activated" = yes ] && start_new ;;
        esac
    elif [ -z "$(packages)" ]; then
        # No package at all: BrickOS is removed.
        rollback_without_package && rm -rf "$brickos/package" && rm -f "$self"
    else
        note "the marker names build '$build', which is not on the card, and several packages are; nothing changed"
    fi
else
    package=$(packages | head -n 1)
    if [ -f "$brickos/setup/setup.sh" ]; then
        rollback_without_package && rm -rf "$brickos/package" && rm -f "$self"
    elif [ -n "$package" ]; then
        sh "$package/setup.sh" rollback --boot > "$out" 2>&1
        [ "$(result)" = rolled-back ] && rm -rf "$brickos/package" && rm -f "$self"
    else
        rollback_without_package && rm -rf "$brickos/package" && rm -f "$self"
    fi
fi
sync
exit 0
