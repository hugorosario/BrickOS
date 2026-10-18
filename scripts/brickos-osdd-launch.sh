#!/bin/sh
# Starts the installed BrickOS OSD from the boot hook. Both BrickOS and a fallback
# official OSD run from the official directory with the official libraries, which the
# official daemon needs.
#
# Run by the start script before the firmware's main loop, it checks the installed image
# and returns only once BrickOS runs, so the loop's `runifnecessary trimui_osdd` finds
# it and starts no other OSD. When the image is missing or does not match its recorded
# hash, it starts nothing: the loop then starts the official OSD in its usual order,
# after musicserver, exactly as on a stock boot. A detached copy, started with
# --supervise, stays as BrickOS's parent and starts the official OSD if BrickOS exits
# during its first seconds, later dies, or stops advancing its loop count.
dir=@INSTALL_DIR@
official_dir=@OFFICIAL_DIR@
health=@HEALTH@
log=@BOOT_LOG@
ready=/tmp/brickos-osdd-launch.ready
note() { echo "$(cut -d ' ' -f 1 /proc/uptime) $*" >> "$log"; }
official() {
    note "starting the official OSD: $*"
    exec "$official_dir/trimui_osdd"
}
export LD_LIBRARY_PATH=@OFFICIAL_LIBS@
cd "$official_dir" || exit 1

if [ "${1:-}" = --supervise ]; then
    elf=$2
    version=$3
    rm -f "$health"
    "$elf" --health "$health" >> "$log" 2>&1 &
    pid=$!
    # The loop looks for the process name, which the ELF has once exec is done.
    for attempt in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20; do
        [ "$(cat "/proc/$pid/comm" 2>/dev/null)" = trimui_osdd ] && break
        kill -0 "$pid" 2>/dev/null || break
        sleep 0.05
    done
    echo "$pid" > "$ready.tmp" && mv "$ready.tmp" "$ready"
    sleep @START_WAIT_S@
    if ! kill -0 "$pid" 2>/dev/null; then
        wait "$pid"
        official "BrickOS OSD $version exited with status $? during start"
    fi
    note "BrickOS OSD $version running pid=$pid"
    # BrickOS rewrites its health file every second with a new loop count. Eight checks
    # without a new count, a missing file included, is about 4 s of running time: the daemon
    # has stalled and is killed. A suspend, during which neither process runs, does not count.
    last=
    same=0
    stalled=
    while kill -0 "$pid" 2>/dev/null; do
        sleep 0.5
        loops=$(sed -n 's/^loops=//p' "$health" 2>/dev/null)
        if [ "$loops" = "$last" ]; then same=$((same + 1)); else same=0; fi
        last=$loops
        if [ "$same" -ge 8 ]; then
            stalled=1
            kill -KILL "$pid" 2>/dev/null
            break
        fi
    done
    wait "$pid"
    status=$?
    # A daemon that dies or stalls with its panel open leaves the panel signals, and the
    # stale inputd grab keeps the controller from MainUI, so the firmware would not reach its
    # next cycle to start another OSD. After a stall or an abnormal exit the signals are
    # removed and the official OSD starts; a stop by SIGTERM exits with 0.
    if [ "$status" -eq 0 ] && [ -z "$stalled" ]; then
        note "BrickOS OSD $version stopped"
        exit 0
    fi
    rm -f @PANEL_SIGNALS@
    reason="exited with status $status"
    [ -n "$stalled" ] && reason="stopped advancing its loop count (loops=${last:-none}) and was killed"
    if pidof trimui_osdd >/dev/null 2>&1; then
        note "BrickOS OSD $version $reason; another OSD is running"
        exit 0
    fi
    official "BrickOS OSD $version $reason"
fi

skip() {
    note "not starting BrickOS: $*; the firmware starts the official OSD"
    exit 0
}
if pidof trimui_osdd >/dev/null 2>&1; then
    note "an OSD is already running; nothing started"
    exit 0
fi
read -r version expected < "$dir/current" 2>/dev/null || skip "no current BrickOS version"
case "$version" in
    ''|*[!0-9a-f]*) skip "invalid BrickOS version '$version'" ;;
esac
elf="$dir/$version/trimui_osdd"
[ -f "$elf" ] || skip "missing $elf"
actual=$(sha256sum "$elf" 2>/dev/null | cut -d ' ' -f 1)
[ -n "$expected" ] && [ "$actual" = "$expected" ] || skip "hash mismatch for $elf"
rm -f "$ready"
setsid sh "$0" --supervise "$elf" "$version" </dev/null >/dev/null 2>&1 &
for attempt in $(seq 1 60); do
    [ -f "$ready" ] && break
    sleep 0.05
done
rm -f "$ready"
exit 0
