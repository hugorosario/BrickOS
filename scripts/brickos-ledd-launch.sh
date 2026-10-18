#!/bin/sh
# Starts the installed BrickOS LED daemon from the boot hook.
#
# It checks the installed image against its recorded hash and starts nothing when it is missing
# or differs, or when a daemon or its supervisor already runs: the LEDs then behave as on a
# stock boot. Otherwise a detached copy, started with --supervise, stays as the daemon's parent.
# A stop by SIGTERM exits with 0 and ends supervision. Any other exit restarts the daemon after
# @RESTART_DELAY_S@ s, at most three times per boot (the count lives in /tmp); after the fourth
# failure the supervisor ends any effect the daemon left running, reports `failed` in the
# health file, and leaves the LEDs to the firmware.
dir=@INSTALL_DIR@
root=@ROOT@
health=@HEALTH@
failures=@FAILURES@
log=@BOOT_LOG@
ready=$root/tmp/brickos-ledd-launch.ready
note() { echo "$(cut -d ' ' -f 1 /proc/uptime) $*" >> "$log"; }
supervisors() {
    for proc in /proc/[0-9]*; do
        # The error redirection comes first: a process can exit before its cmdline opens.
        case "$(tr "\0" " " 2>/dev/null < "$proc/cmdline")" in
            *"$dir/launch.sh --supervise "*) echo "${proc#/proc/}" ;;
        esac
    done
}

if [ "${1:-}" = --supervise ]; then
    elf=$2
    version=$3
    announced=0
    while :; do
        "$elf" ${root:+--root "$root"} >> "$log" 2>&1 &
        pid=$!
        if [ "$announced" = 0 ]; then
            echo "$pid" > "$ready.tmp" && mv "$ready.tmp" "$ready"
            announced=1
        fi
        note "LED daemon $version running pid=$pid"
        wait "$pid"
        status=$?
        if [ "$status" -eq 0 ]; then
            note "LED daemon $version stopped"
            exit 0
        fi
        count=$(($(cat "$failures" 2>/dev/null || echo 0) + 1))
        echo "$count" > "$failures"
        if [ "$count" -gt 3 ]; then
            note "LED daemon $version exited with status $status, failure $count this boot; not restarted"
            "$elf" ${root:+--root "$root"} --end-effects >> "$log" 2>&1
            printf 'state=failed\nmode=\nreason=crashed\npid=0\n' > "$health.tmp" && mv "$health.tmp" "$health"
            exit 1
        fi
        note "LED daemon $version exited with status $status; restart $count of 3 in @RESTART_DELAY_S@ s"
        sleep @RESTART_DELAY_S@
    done
fi

skip() {
    note "not starting the LED daemon: $*; the LEDs stay with the firmware"
    exit 0
}
if [ -n "$(pidof brickos-ledd 2>/dev/null)" ] || [ -n "$(supervisors)" ]; then
    note "an LED daemon is already running; nothing started"
    exit 0
fi
read -r version expected < "$dir/current" 2>/dev/null || skip "no current version"
case "$version" in
    ''|*[!0-9a-f]*) skip "invalid version '$version'" ;;
esac
elf="$dir/$version/brickos-ledd"
[ -f "$elf" ] || skip "missing $elf"
actual=$(sha256sum "$elf" 2>/dev/null | cut -d ' ' -f 1)
[ -n "$expected" ] && [ "$actual" = "$expected" ] || skip "hash mismatch for $elf"
rm -f "$ready"
setsid sh "$0" --supervise "$elf" "$version" </dev/null >/dev/null 2>&1 &
i=0
while [ "$i" -lt 30 ]; do
    [ -f "$ready" ] && break
    sleep 0.1
    i=$((i + 1))
done
rm -f "$ready"
exit 0
