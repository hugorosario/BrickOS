#!/bin/sh
# Starts the installed BrickOS input daemon from the boot hook. BrickOS, and the official
# daemon when it takes over, run from the official directory with the official libraries.
#
# Run by the start script before the firmware's main loop, it checks the installed image and
# returns once BrickOS has sampled the controls and created TRIMUI Player1, so the loop's
# `runifnecessary "inputd"` check finds it and starts no other producer. When the image is
# missing or does not match its recorded hash, it starts nothing, and the loop starts the
# official daemon exactly as on a stock boot. A detached copy, started with --supervise,
# stays as BrickOS's parent. When BrickOS does not become ready at boot it lets the firmware
# start the official daemon; when BrickOS later exits for any reason or stops advancing its
# scan count, it starts the official daemon itself. MainUI and the OSD reopen the new
# controller by themselves.
#
# This script runs as "sh", so the loop's pgrep for "inputd" never matches it.
dir=@INSTALL_DIR@
official=@OFFICIAL@
health=@HEALTH@
devices=@DEVICES@
log=@BOOT_LOG@
ready=/tmp/brickos-inputd-launch.ready
note() { echo "$(cut -d ' ' -f 1 /proc/uptime) $*" >> "$log"; }
players() { grep -c '^N: Name="TRIMUI Player1"' "$devices" 2>/dev/null; }
official() {
    local i=0
    note "starting the official daemon: $*"
    # The kernel removes BrickOS's controller when its process ends.
    while [ "$i" -lt 30 ] && [ "$(players)" != 0 ]; do sleep 0.1; i=$((i + 1)); done
    exec "$official"
}
export LD_LIBRARY_PATH=@OFFICIAL_LIBS@
cd "$(dirname "$official")" || exit 1

if [ "${1:-}" = --supervise ]; then
    elf=$2
    version=$3
    rm -f "$health"
    # GameSwitcher's sessions claim POWER through this endpoint directory while a game runs.
    "$elf" --run --health "$health" --config "$dir/inputd.json" --game-power /tmp/brickos-gameswitcher-power >> "$log" 2>&1 &
    pid=$!
    i=0
    while [ "$i" -lt @START_WAIT_TENTHS@ ]; do
        grep -q '^state=ready' "$health" 2>/dev/null && [ "$(players)" = 1 ] && break
        kill -0 "$pid" 2>/dev/null || break
        sleep 0.1
        i=$((i + 1))
    done
    if ! grep -q '^state=ready' "$health" 2>/dev/null || [ "$(players)" != 1 ]; then
        # At boot the firmware loop starts the official daemon once this launcher returns,
        # so nothing is started here: starting it too would leave two producers.
        kill -KILL "$pid" 2>/dev/null
        wait "$pid"
        i=0
        while [ "$i" -lt 30 ] && [ "$(players)" != 0 ]; do sleep 0.1; i=$((i + 1)); done
        note "BrickOS $version did not become ready; the firmware starts the official daemon"
        echo none > "$ready.tmp" && mv "$ready.tmp" "$ready"
        exit 0
    fi
    echo "$pid" > "$ready.tmp" && mv "$ready.tmp" "$ready"
    note "BrickOS $version running pid=$pid"
    last=
    same=0
    while kill -0 "$pid" 2>/dev/null; do
        sleep 0.5
        scans=$(sed -n 's/^scans=//p' "$health" 2>/dev/null)
        if [ -n "$scans" ] && [ "$scans" = "$last" ]; then same=$((same + 1)); else same=0; fi
        last=$scans
        # Eight checks without a new scan is about 4 s of running time; a suspend, during
        # which neither process runs, does not count.
        if [ "$same" -ge 8 ]; then
            kill -KILL "$pid" 2>/dev/null
            wait "$pid"
            official "BrickOS $version stopped scanning"
        fi
    done
    wait "$pid"
    status=$?
    # Any exit, a clean one included, hands over to the official daemon: while MainUI runs,
    # the firmware loop would not start a producer. Rollback stops this supervisor first.
    if pidof trimui_inputd >/dev/null 2>&1; then
        note "BrickOS $version exited with status $status; another producer is running"
        exit 0
    fi
    official "BrickOS $version exited with status $status"
fi

skip() {
    note "not starting BrickOS: $*; the firmware starts the official daemon"
    exit 0
}
# The entry that makes the SD trimui tree active, if any: anything other than a res directory.
# A tree that holds only res/, where RetroArch finds its loading screen, has no programs for the
# firmware's SD branch, so the firmware falls through to /usr as with no tree.
sd_tree_entry() {
    local entry
    [ -d "$1" ] || return 1
    for entry in "$1"/* "$1"/.[!.]* "$1"/..?*; do
        [ -e "$entry" ] || [ -L "$entry" ] || continue
        [ "${entry##*/}" = res ] && [ -d "$entry" ] && continue
        echo "${entry##*/}"
        return 0
    done
    return 1
}
if pidof trimui_inputd >/dev/null 2>&1; then
    note "a producer is already running; nothing started"
    exit 0
fi
# With an active SD trimui tree the firmware takes its SD branch, which starts that tree's own
# producer; BrickOS stays out of it.
entry=$(sd_tree_entry @SD_TREE@) && skip "@SD_TREE@ holds $entry"
read -r version expected < "$dir/current" 2>/dev/null || skip "no current BrickOS version"
case "$version" in
    ''|*[!0-9a-f]*) skip "invalid BrickOS version '$version'" ;;
esac
elf="$dir/$version/trimui_inputd"
[ -f "$elf" ] || skip "missing $elf"
actual=$(sha256sum "$elf" 2>/dev/null | cut -d ' ' -f 1)
[ -n "$expected" ] && [ "$actual" = "$expected" ] || skip "hash mismatch for $elf"
rm -f "$ready"
setsid sh "$0" --supervise "$elf" "$version" </dev/null >/dev/null 2>&1 &
i=0
while [ "$i" -lt 60 ]; do
    [ -f "$ready" ] && break
    sleep 0.1
    i=$((i + 1))
done
rm -f "$ready"
exit 0
