#!/bin/sh
# GameSwitcher's entry for the launchers it took, its app and MainUI's boot hook, installed by
# the release package's setup as BrickOS/gameswitcher/entry.sh. BrickOS's MainUI runs pick, the
# collection over its own screen, which names a chosen checkpoint; it then hands the init
# "resume ID STATE", which starts that checkpoint. A session needs the runtime that
# runtime/current names, matching its manifest, and BrickOS's input daemon ready with game POWER.
# Otherwise, and under netplay, a launch runs the launcher's stock bytes with the BrickOS menu
# library the menu component installed, as a launcher with the menu line does.
set -u
sd=${BRICKOS_SD_ROOT:-/mnt/SDCARD}
base=$sd/BrickOS/gameswitcher
mode=${1:-}; shift || exit 2
runtime() {
    [ ! -e "$base/DISABLED" ] && [ -f "$base/runtime/current" ] || return 1
    read -r version expected < "$base/runtime/current" || return 1
    case "$version" in ''|*[!0-9a-f]*) return 1 ;; esac
    [ "${#version}" = 12 ] && [ "${#expected}" = 64 ] || return 1
    case "$expected" in "$version"*) ;; *) return 1 ;; esac
    directory=$base/runtime/$version
    [ "$(readlink -f "$directory")" = "$directory" ] || return 1
    [ "$(sha256sum "$directory/MANIFEST.sha256" | cut -d ' ' -f 1)" = "$expected" ] || return 1
    awk 'NF!=2 || length($1)!=64 || $1~/[^0-9a-f]/ {exit 1}
         $2!="GameSwitcher" && $2!="gs-session" && $2!="libtmenu.so" {exit 1}
         seen[$2]++ {exit 1} END {if(NR!=3) exit 1}' "$directory/MANIFEST.sha256" || return 1
    for file in GameSwitcher gs-session libtmenu.so; do
        [ -f "$directory/$file" ] && [ "$(readlink -f "$directory/$file")" = "$directory/$file" ] || return 1
    done
    (cd "$directory" && sha256sum -c MANIFEST.sha256 >/dev/null)
}
# BrickOS's input daemon, from its install or an end-to-end session's candidate, ready and
# serving game POWER.
healthy() {
    health=/tmp/brickos-inputd.health
    pid=$(sed -n 's/^pid=//p' "$health" 2>/dev/null)
    case "$pid" in ''|*[!0-9]*) return 1 ;; esac
    grep -qx 'state=ready' "$health" && grep -qx 'game_power=1' "$health" || return 1
    case "$(readlink "/proc/$pid/exe")" in "$sd/BrickOS/inputd/"*|/tmp/brickos-e2e-*/pkg/trimui_inputd) return 0 ;; esac
    return 1
}
# The folder of the BrickOS menu library the menu component installed, if any.
menu_library() {
    read -r build < "$sd/BrickOS/tmenu/current" 2>/dev/null || return 1
    case "$build" in ''|*[!0-9a-f]*) return 1 ;; esac
    [ -f "$sd/BrickOS/tmenu/$build/libtmenu.so" ] && echo "$sd/BrickOS/tmenu/$build"
}
fallback() {
    if [ "$mode" = apps ] || [ "$mode" = pick ]; then echo 'GameSwitcher is unavailable; games start without it' >&2; exit 1; fi
    # A checkpoint resumes only through a session.
    [ "$mode" != resume ] || exit 1
    [ "$mode" = launch ] && [ $# = 4 ] || exit 0
    # This backup was written by the setup with the launcher's stock bytes. No saved checkpoint
    # supplies a command or additional argument.
    [ -f "$4" ] && [ "$(readlink -f "$4")" = "$4" ] || exit 1
    # The stock RetroArch command inherits this, so the game loads the BrickOS menu.
    if library=$(menu_library); then export LD_LIBRARY_PATH="$library:${LD_LIBRARY_PATH:-}"; fi
    exec sh "$4" "$2"
}
exec_session() {
    # Opt-in physical acceptance trace. Production keeps the caller's normal
    # output and performs no verbose SD logging unless this fixed marker exists.
    trace=$base/acceptance-trace
    if [ -f "$base/TRACE" ] && [ ! -L "$base/TRACE" ] && [ ! -L "$trace" ]; then
        mkdir -p "$trace"
        if [ "$(readlink -f "$trace")" = "$trace" ]; then
            boot=$(cat /proc/sys/kernel/random/boot_id)
            attempt=$(cat /proc/sys/kernel/random/uuid)
            case "$boot$attempt" in ''|*[!0-9a-f-]*) ;;
            *)
                logfile=$trace/$boot-$attempt.log
                printf 'GameSwitcher activation version=%s boot=%s mode=%s\n' "$version" "$boot" "$mode" >> "$logfile"
                exec "$directory/gs-session" "$@" >> "$logfile" 2>&1 ;;
            esac
        fi
    fi
    exec "$directory/gs-session" "$@"
}
if [ "$mode" = runtime ]; then runtime || exit 1; printf '%s\n' "$directory"; exit 0; fi
if [ "$mode" = boot ]; then
    # /tmp clears at boot. Claim before validation so corrupt/missing targets and
    # failed loads never repeat when the firmware calls MainUI again.
    mkdir /tmp/brickos-gameswitcher-boot-attempted 2>/dev/null || exit 0
fi
runtime && healthy || fallback "$@"
# A session that ended without closing its POWER endpoint, or died, leaves the socket
# behind; with no gs-session alive nobody owns it. A live session keeps the normal launcher.
control=/tmp/brickos-gameswitcher-power/control
if [ -e "$control" ] && ! pidof gs-session >/dev/null 2>&1; then rm -f "$control"; fi
[ ! -e "$control" ] || fallback "$@"
case "$mode" in
boot) (exec_session --sd "$sd" --power-control /tmp/brickos-gameswitcher-power --boot); exit 0 ;;
apps) exec_session --sd "$sd" --power-control /tmp/brickos-gameswitcher-power --apps ;;
pick) exec_session --sd "$sd" --power-control /tmp/brickos-gameswitcher-power --pick ;;
resume)
    [ $# = 2 ] || exit 2
    exec_session --sd "$sd" --power-control /tmp/brickos-gameswitcher-power --resume "$1" "$2" ;;
launch)
    [ $# = 4 ] || exit 2
    [ -z "${NET_PARAM:-}" ] || fallback "$@"
    "$directory/gs-session" --sd "$sd" --check-launch "$1" "$2" "$3" >/dev/null || fallback "$@"
    exec_session --sd "$sd" --power-control /tmp/brickos-gameswitcher-power --launch "$1" "$2" "$3" ;;
*) exit 2 ;;
esac
