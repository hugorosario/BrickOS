#!/bin/sh
# The frontend the firmware loop starts. The start script bind-mounts this file over
# /usr/trimui/bin/MainUI after binding the official MainUI to a private path, so the loop's
# `MainUI` runs it in every cycle.
#
# Each cycle runs the BrickOS frontend with a new run token. The frontend writes
# <token>.ready after its first frame and <token>.done when it ends cleanly (a launch handoff
# included). Then:
# - done: a clean run; the failure count is cleared and the loop goes on (a launch runs next);
# - no ready marker (missing executable, failure before the first frame): the official
#   MainUI runs for this cycle;
# - ready but no done (a crash or kill after the first frame): a failure; the next cycle tries
#   BrickOS again.
# After @MAX_FAILURES@ failures in one boot, or with the DISABLED latch, every later cycle
# runs the official MainUI. /tmp is empty at each boot, so a reboot tries BrickOS again.
dir=@INSTALL_DIR@
# The official MainUI at the private path the start script binds it to. /usr/trimui/bin/MainUI
# is this wrapper while the binds hold, so exec'ing it would only start the wrapper again.
official=@PRIVATE@
run=@RUN_DIR@
log=@BOOT_LOG@
max=@MAX_FAILURES@

note() { echo "$(cut -d ' ' -f 1 /proc/uptime) mainui-wrapper: $*" >> "$log"; }
official_mainui() {
    note "official MainUI: $1"
    exec "$official"
}
failure() {
    local count
    count=$(( $(cat "$run/failures" 2>/dev/null || echo 0) + 1 ))
    echo "$count" > "$run/failures"
    note "BrickOS failure $count of $max: $1"
    [ "$count" -ge "$max" ] && touch "$run/boot-disabled" && note "BrickOS off for this boot"
}

# `current` is the install manifest. Older manifests have three fields and retain the
# stock presentation. The installer only writes an explicit value for a new install.
read -r version elf_hash wrapper_hash presentation < "$dir/current" 2>/dev/null
presentation=${presentation:-stock}
case "$presentation" in
    stock) ;;
    material-you) ;;
    *) note "invalid presentation '$presentation'; using stock"; presentation=stock ;;
esac

mkdir -p "$run"
[ -e "$dir/DISABLED" ] && official_mainui "the DISABLED latch is set"
[ -e "$run/boot-disabled" ] && official_mainui "BrickOS is off for this boot"
elf="$dir/$version/MainUI"
if [ ! -x "$elf" ]; then
    failure "no executable"
    official_mainui "no BrickOS executable"
fi
# The updater only checks public release metadata. It downloads nothing without the Settings
# confirmation. The monitor's lock permits one process across frontend restarts and game return.
updater=/mnt/SDCARD/BrickOS/settings/Updates/brickos-update
if [ -x "$updater" ]; then
    "$updater" monitor </dev/null >>/tmp/brickos-update-monitor.log 2>&1 &
fi
# BrickOS GameSwitcher boot hook: entry guards one attempt per boot and falls back here.
if [ -f /mnt/SDCARD/BrickOS/gameswitcher/entry.sh ]; then
    sh /mnt/SDCARD/BrickOS/gameswitcher/entry.sh boot
fi
token=$(cat /proc/sys/kernel/random/uuid 2>/dev/null)
[ -n "$token" ] || token="$$-$(cut -d ' ' -f 1 /proc/uptime)"
pending_file="$run/pending"
pending=$(cat "$pending_file" 2>/dev/null)
case "$pending" in *[!A-Za-z0-9_.-]*|'') pending= ;; esac
# Markers of earlier runs never count for this one.
rm -f "$run"/*.ready "$run"/*.done
BRICKOS_MAINUI_RUN=$run BRICKOS_MAINUI_TOKEN=$token BRICKOS_MAINUI_PENDING=$pending BRICKOS_MAINUI_STOCK=$official \
    "$elf" --default-presentation "$presentation"
status=$?
if [ -e "$run/$token.done" ]; then
    sidecar=/tmp/brickos-mainui-return.json
    if [ -f "$sidecar" ] && grep -Fq "\"token\":\"$token\"" "$sidecar"; then
        printf '%s\n' "$token" > "$pending_file.tmp"
        mv -f "$pending_file.tmp" "$pending_file"
    elif [ -z "$pending" ] || [ ! -f "$sidecar" ] || ! grep -Fq "\"token\":\"$pending\"" "$sidecar"; then
        rm -f "$pending_file" "$pending_file.tmp"
    fi
    rm -f "$run/failures" "$run/$token.ready" "$run/$token.done"
    note "BrickOS ended cleanly with status $status"
    exit "$status"
fi
failure "status $status"
if [ ! -e "$run/$token.ready" ]; then
    official_mainui "BrickOS failed before its first frame"
fi
rm -f "$run/$token.ready"
note "BrickOS failed after its first frame"
exit "$status"
