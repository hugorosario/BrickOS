#!/bin/sh
# Device-side helper for the BrickOS LED daemon.
#
#   ledd-install.sh preflight CONFIG   Report the state an install depends on, and its
#                                      fingerprint. Changes nothing.
#   ledd-install.sh install CONFIG     Stage the daemon for the next boot if the state still has
#                                      the fingerprint of the dry run.
#   ledd-install.sh rollback CONFIG    Remove the start script, stop a running daemon, which
#                                      ends its effects, and remove the installed files.
#
# With boot=1 in CONFIG, the helper runs at boot before the start scripts, so no daemon runs
# yet and none is stopped.
#
# CONFIG is a shell fragment; every path is below `root`, which is empty on the device. The
# install adds a start script to /mnt/SDCARD/System/starts and files under
# /mnt/SDCARD/BrickOS/ledd. It never writes under /usr/trimui and never stops a stock process:
# no stock process owns the LEDs, so without the daemon they simply follow the firmware. Files
# are staged under a temporary name and renamed into place, the start script last; a running
# image is never overwritten. The configuration config.json and a DISABLED latch belong to the
# user: nothing here writes or removes them.
set -u

command=${1:-}
config=${2:-}
[ -n "$command" ] && [ -f "$config" ] || { echo "usage: ledd-install.sh preflight|install|rollback CONFIG" >&2; exit 2; }
root=''
expected_fingerprint=''
candidate=''
candidate_hash=''
start_template=''
launch_template=''
restart_delay_s=5
stop_timeout_s=5
boot=''
# Host tests only: the install step after which a failure is simulated.
inject_failure=''
. "$config"

sd=$root/mnt/SDCARD
starts=$sd/System/starts
start_script=$starts/brickos-ledd.sh
install_parent=$sd/BrickOS
install=$install_parent/ledd
record=$install/install-record
health=$root/tmp/brickos-ledd.health
failures=$root/tmp/brickos-ledd-launch.failures
boot_log=$root/tmp/brickos-ledd-boot.log
start_marker='# BrickOS LED daemon start script'

hash_of() { if [ -f "$1" ]; then sha256sum "$1" | cut -d ' ' -f 1; else echo absent; fi; }
fail() { echo "result=refused"; echo "reason=$*"; exit 3; }
daemons() {
    for pid in $(pidof brickos-ledd 2>/dev/null); do
        echo "pid=$pid exe=$(readlink "/proc/$pid/exe" 2>/dev/null)"
    done
}
user_file() { [ "$1" = "$install/config.json" ] || [ "$1" = "$install/DISABLED" ]; }

state() {
    local file
    daemons | sed 's/^/daemon /'
    echo "daemons=$(daemons | wc -l)"
    if [ -d "$starts" ]; then
        echo "starts_dir=present"
        for file in "$starts"/*; do
            [ -f "$file" ] && echo "start ${file##*/}=$(hash_of "$file")"
        done
    else
        echo "starts_dir=absent"
    fi
    # The user's files may change at any time and do not affect an install.
    if [ -d "$install" ]; then
        echo "install_dir=present"
        for file in "$install"/* "$install"/*/*; do
            [ -f "$file" ] && ! user_file "$file" && echo "install ${file#"$install"/}=$(hash_of "$file")"
        done
    else
        echo "install_dir=absent"
    fi
}

preflight() {
    local lines
    lines=$(state)
    echo "$lines"
    echo "fingerprint=$(echo "$lines" | sha256sum | cut -d ' ' -f 1)"
    echo "free_kb=$(df -k "$sd" 2>/dev/null | tail -n 1 | awk '{print $4}')"
    up_to_date && echo "up_to_date=yes" || echo "up_to_date=no"
}

# Whether an install of this candidate would write the same bytes that are already in place.
up_to_date() {
    local version
    version=$(echo "$candidate_hash" | cut -c 1-12)
    [ -n "$version" ] && [ "$(cat "$install/current" 2>/dev/null)" = "$version $candidate_hash" ] &&
        [ "$(hash_of "$install/$version/brickos-ledd")" = "$candidate_hash" ] &&
        [ -f "$launch_template" ] && [ "$(render "$launch_template" | sha256sum | cut -d ' ' -f 1)" = "$(hash_of "$install/launch.sh")" ] &&
        [ -f "$start_template" ] && [ "$(render "$start_template" | sha256sum | cut -d ' ' -f 1)" = "$(hash_of "$start_script")" ]
}

put() {
    local target=$1 temporary="$1.brickos-tmp"
    cat > "$temporary" && chmod 755 "$temporary" && sync && mv "$temporary" "$target"
}

render() {
    sed -e "s#@INSTALL_DIR@#$install#g" -e "s#@ROOT@#$root#g" -e "s#@HEALTH@#$health#g" \
        -e "s#@FAILURES@#$failures#g" -e "s#@BOOT_LOG@#$boot_log#g" -e "s#@RESTART_DELAY_S@#$restart_delay_s#g" "$1"
}

install_ledd() {
    local report fingerprint version target old running pid
    report=$(preflight)
    fingerprint=$(echo "$report" | sed -n 's/^fingerprint=//p')
    [ -n "$expected_fingerprint" ] && [ "$fingerprint" = "$expected_fingerprint" ] ||
        fail "the device state changed since the dry run (fingerprint $fingerprint)"
    [ -f "$candidate" ] && [ "$(hash_of "$candidate")" = "$candidate_hash" ] || fail "staged LED daemon does not match its hash"
    [ -f "$start_template" ] && [ -f "$launch_template" ] || fail "missing start or launch template"
    grep -qx "$start_marker" "$start_template" || fail "start template lacks its marker"
    if [ -f "$start_script" ] && ! grep -qx "$start_marker" "$start_script"; then
        fail "$start_script exists and was not written by BrickOS"
    fi
    version=$(echo "$candidate_hash" | cut -c 1-12)
    target=$install/$version
    # From here on, a failure puts every file and folder back as it was before this install.
    local had_starts=0 had_parent=0 had_install=0 had_target=0 name
    [ -d "$starts" ] && had_starts=1
    [ -d "$install_parent" ] && had_parent=1
    [ -d "$install" ] && had_install=1
    [ -d "$target" ] && had_target=1
    mkdir -p "$root/tmp" && backup=$(mktemp -d "$root/tmp/brickos-ledd-install.XXXXXX") || fail "cannot create a backup folder"
    for name in launch.sh current install-record; do
        [ -f "$install/$name" ] && cp -p "$install/$name" "$backup/$name"
    done
    [ -f "$start_script" ] && cp -p "$start_script" "$backup/start-script"
    abort() {
        for name in launch.sh current install-record; do
            rm -f "$install/$name.brickos-tmp"
            if [ -f "$backup/$name" ]; then mv "$backup/$name" "$install/$name"; else rm -f "$install/$name"; fi
        done
        rm -f "$start_script.brickos-tmp"
        if [ -f "$backup/start-script" ]; then mv "$backup/start-script" "$start_script"; else rm -f "$start_script"; fi
        rm -f "$target/brickos-ledd.brickos-tmp"
        [ "$had_target" = 1 ] || rm -rf "$target"
        [ "$had_install" = 1 ] || rmdir "$install" 2>/dev/null
        [ "$had_parent" = 1 ] || rmdir "$install_parent" 2>/dev/null
        [ "$had_starts" = 1 ] || rmdir "$starts" 2>/dev/null
        rm -rf "$backup"
        sync
        fail "$*; the previous state was restored"
    }
    step() { [ "$inject_failure" = "$1" ] && abort "simulated failure after $1"; return 0; }
    if [ ! -f "$record" ]; then
        mkdir -p "$starts" "$install" || abort "cannot create $starts or $install"
        printf 'starts_created=%s\nparent_created=%s\ninstall_created=%s\n' $((1 - had_starts)) $((1 - had_parent)) $((1 - had_install)) |
            put "$record" || abort "cannot write $record"
    fi
    step record
    mkdir -p "$target" || abort "cannot create $target"
    if [ "$(hash_of "$target/brickos-ledd")" != "$candidate_hash" ]; then
        put "$target/brickos-ledd" < "$candidate" || abort "cannot stage $target/brickos-ledd"
        [ "$(hash_of "$target/brickos-ledd")" = "$candidate_hash" ] || abort "staged image does not verify"
    fi
    step elf
    render "$launch_template" | put "$install/launch.sh" || abort "cannot write launch.sh"
    step launch
    echo "$version $candidate_hash" | put "$install/current" || abort "cannot write current"
    step current
    # Activation is the last step.
    render "$start_template" | put "$start_script" || abort "cannot write $start_script"
    rm -rf "$backup"
    for old in "$install"/*/brickos-ledd; do
        [ -f "$old" ] || continue
        [ "${old%/brickos-ledd}" = "$target" ] && continue
        running=0
        for pid in $(pidof brickos-ledd 2>/dev/null); do
            [ "$(readlink "/proc/$pid/exe")" = "$old" ] && running=1
        done
        [ "$running" -eq 0 ] && rm -rf "${old%/brickos-ledd}"
    done
    sync
    echo "result=installed"
    echo "version=$version"
    echo "elf=$target/brickos-ledd"
    echo "start_script=$start_script"
}

# Stops the supervisors first, so none restarts the daemon, then the daemons themselves. A
# daemon stopped by SIGTERM ends its own effects; one that has to be killed cannot, so its
# binary ends the recorded pattern with --end-effects.
stop_daemons() {
    local proc pid exe waited
    for proc in /proc/[0-9]*; do
        case "$(tr "\0" " " 2>/dev/null < "$proc/cmdline")" in
            *"$install/launch.sh"*) kill -TERM "${proc#/proc/}" 2>/dev/null && echo "stopped_launcher=${proc#/proc/}" ;;
        esac
    done
    for pid in $(pidof brickos-ledd 2>/dev/null); do
        exe=$(readlink "/proc/$pid/exe" 2>/dev/null)
        case "$exe" in
            "$install"/*)
                kill -TERM "$pid" 2>/dev/null
                waited=0
                while kill -0 "$pid" 2>/dev/null && [ "$waited" -lt $((stop_timeout_s * 10)) ]; do
                    sleep 0.1; waited=$((waited + 1))
                done
                if kill -0 "$pid" 2>/dev/null; then
                    kill -KILL "$pid" 2>/dev/null && sleep 0.2
                    kill -0 "$pid" 2>/dev/null && fail "LED daemon pid $pid did not stop"
                    "$exe" ${root:+--root "$root"} --end-effects </dev/null >/dev/null 2>&1 && echo "ended_effects=$exe"
                fi
                echo "stopped=$pid $exe" ;;
        esac
    done
}

rollback() {
    local file starts_created=0 parent_created=0 install_created=0
    # First, the next boot no longer starts the daemon.
    if [ -f "$start_script" ]; then
        grep -qx "$start_marker" "$start_script" || fail "$start_script was not written by BrickOS"
        mv "$start_script" "$start_script.removing" && rm -f "$start_script.removing" && sync ||
            fail "cannot remove $start_script"
    fi
    if [ "$boot" = 1 ]; then
        echo "daemons=none started yet"
    else
        stop_daemons
    fi
    if [ -d "$install" ]; then
        [ -f "$record" ] && . "$record"
        for file in "$install"/* "$install"/.[!.]*; do
            [ -e "$file" ] || continue
            user_file "$file" && continue
            rm -rf "$file"
        done
        [ "$install_created" = 1 ] && rmdir "$install" 2>/dev/null
        [ "$parent_created" = 1 ] && rmdir "$install_parent" 2>/dev/null
        [ "$starts_created" = 1 ] && rmdir "$starts" 2>/dev/null
    fi
    sync
    [ -f "$start_script" ] && fail "$start_script is still present"
    [ -e "$install/launch.sh" ] && fail "$install/launch.sh is still present"
    echo "result=rolled-back"
    echo "daemons_after=$(daemons | wc -l)"
}

case "$command" in
    preflight) preflight ;;
    install) install_ledd ;;
    rollback) rollback ;;
    *) echo "unknown command $command" >&2; exit 2 ;;
esac
