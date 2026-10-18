#!/bin/sh
# Device-side helper for the opt-in BrickOS input daemon.
#
#   inputd-install.sh preflight CONFIG   Report the state an install depends on, and its
#                                        fingerprint. Changes nothing.
#   inputd-install.sh install CONFIG     Stage BrickOS for the next boot if the state still has
#                                        the fingerprint of the dry run.
#   inputd-install.sh rollback CONFIG    Remove the start script and the BrickOS files, stop a
#                                        running BrickOS daemon, and start the official one.
#
# With boot=1 in CONFIG, the helper runs at boot before the firmware's daemon loop, so no
# producer runs yet: install also accepts none, and neither command stops or starts a daemon.
# The loop then starts the official daemon after a rollback.
#
# CONFIG is a shell fragment; every path is below `root`, which is empty on the device. The
# install adds a start script to /mnt/SDCARD/System/starts and files under
# /mnt/SDCARD/BrickOS/inputd. It never creates /mnt/SDCARD/trimui, and never writes under
# /usr/trimui. An entry in /mnt/SDCARD/trimui other than res/ makes runtrimui.sh run its SD
# branch, so the install refuses; a tree that holds only res/, RetroArch's loading screen, is
# left alone and does not stop it. Files are staged under a temporary name and renamed into
# place, the start script last; a running image is never overwritten. The optional profile
# inputd.json belongs to the user: nothing here writes or removes it.
set -u

command=${1:-}
config=${2:-}
[ -n "$command" ] && [ -f "$config" ] || { echo "usage: inputd-install.sh preflight|install|rollback CONFIG" >&2; exit 2; }
root=''
expected_fingerprint=''
candidate=''
candidate_hash=''
start_template=''
launch_template=''
devices=/proc/bus/input/devices
start_wait_tenths=30
stop_timeout_s=5
boot=''
# Host tests only: the install step after which a failure is simulated.
inject_failure=''
. "$config"

sd=$root/mnt/SDCARD
starts=$sd/System/starts
start_script=$starts/brickos-inputd.sh
install_parent=$sd/BrickOS
install=$install_parent/inputd
record=$install/install-record
official_dir=$root/usr/trimui/bin
official=$official_dir/trimui_inputd
official_libs=$root/usr/trimui/lib
sd_tree=$sd/trimui
health=$root/tmp/brickos-inputd.health
boot_log=$root/tmp/brickos-inputd-boot.log
start_marker='# BrickOS input daemon start script'

hash_of() { if [ -f "$1" ]; then sha256sum "$1" | cut -d ' ' -f 1; else echo absent; fi; }
fail() { echo "result=refused"; echo "reason=$*"; exit 3; }
# The entry that makes /mnt/SDCARD/trimui an active SD tree, if any: anything other than a res
# directory. The firmware takes its SD branch whenever the folder exists, but a tree that holds
# only res/, where RetroArch finds its loading screen, has no programs for that branch, so the
# firmware falls through to /usr as with no tree. An empty tree is inactive too.
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
players() { grep -c '^N: Name="TRIMUI Player1"' "$devices" 2>/dev/null; }
producers() {
    for pid in $(pidof trimui_inputd 2>/dev/null); do
        echo "pid=$pid exe=$(readlink "/proc/$pid/exe" 2>/dev/null)"
    done
}

state() {
    local file entry
    [ -f "$official" ] && echo "official=present" || echo "official=absent"
    producers | sed 's/^/producer /'
    echo "producers=$(producers | wc -l)"
    echo "players=$(players)"
    if entry=$(sd_tree_entry "$sd_tree"); then echo "sd_trimui_tree=active $entry"
    elif [ -d "$sd_tree" ]; then echo "sd_trimui_tree=loading-screen"
    else echo "sd_trimui_tree=absent"; fi
    if [ -d "$starts" ]; then
        echo "starts_dir=present"
        for file in "$starts"/*; do
            [ -f "$file" ] && echo "start ${file##*/}=$(hash_of "$file")"
        done
    else
        echo "starts_dir=absent"
    fi
    if [ -d "$install" ]; then
        echo "install_dir=present"
        for file in "$install"/* "$install"/*/*; do
            [ -f "$file" ] && echo "install ${file#"$install"/}=$(hash_of "$file")"
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
        [ "$(hash_of "$install/$version/trimui_inputd")" = "$candidate_hash" ] &&
        [ -f "$launch_template" ] && [ "$(render "$launch_template" | sha256sum | cut -d ' ' -f 1)" = "$(hash_of "$install/launch.sh")" ] &&
        [ -f "$start_template" ] && [ "$(render "$start_template" | sha256sum | cut -d ' ' -f 1)" = "$(hash_of "$start_script")" ]
}

put() {
    local target=$1 temporary="$1.brickos-tmp"
    cat > "$temporary" && chmod 755 "$temporary" && sync && mv "$temporary" "$target"
}

render() {
    sed -e "s#@INSTALL_DIR@#$install#g" -e "s#@OFFICIAL@#$official#g" -e "s#@OFFICIAL_LIBS@#$official_libs#g" \
        -e "s#@HEALTH@#$health#g" -e "s#@DEVICES@#$devices#g" -e "s#@BOOT_LOG@#$boot_log#g" \
        -e "s#@START_WAIT_TENTHS@#$start_wait_tenths#g" -e "s#@SD_TREE@#$sd_tree#g" "$1"
}

install_inputd() {
    local report fingerprint version target old running pid exe
    report=$(preflight)
    fingerprint=$(echo "$report" | sed -n 's/^fingerprint=//p')
    [ -n "$expected_fingerprint" ] && [ "$fingerprint" = "$expected_fingerprint" ] ||
        fail "the device state changed since the dry run (fingerprint $fingerprint)"
    echo "$report" | grep -qx 'official=present' || fail "$official is missing; it is the fallback producer"
    case "$(echo "$report" | sed -n 's/^sd_trimui_tree=//p')" in
        "active "*) fail "$sd_tree holds $(sd_tree_entry "$sd_tree"); this install needs the firmware's /usr branch" ;;
    esac
    if [ "$boot" = 1 ]; then
        echo "$report" | grep -qx 'producers=[01]' || fail "expected at most one input producer at boot"
    else
        echo "$report" | grep -qx 'producers=1' || fail "expected exactly one input producer"
    fi
    exe=$(producers | sed -n 's/^pid=[0-9]* exe=//p')
    case "$exe" in
        "$official"|"$install"/*) ;;
        '') [ "$boot" = 1 ] && echo "$report" | grep -qx 'producers=0' || fail "unknown input producer" ;;
        *) fail "unknown input producer $exe" ;;
    esac
    [ -f "$candidate" ] && [ "$(hash_of "$candidate")" = "$candidate_hash" ] || fail "staged BrickOS daemon does not match its hash"
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
    backup=$(mktemp -d "$root/tmp/brickos-inputd-install.XXXXXX") || fail "cannot create a backup folder"
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
        rm -f "$target/trimui_inputd.brickos-tmp"
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
    if [ "$(hash_of "$target/trimui_inputd")" != "$candidate_hash" ]; then
        put "$target/trimui_inputd" < "$candidate" || abort "cannot stage $target/trimui_inputd"
        [ "$(hash_of "$target/trimui_inputd")" = "$candidate_hash" ] || abort "staged image does not verify"
    fi
    step elf
    render "$launch_template" | put "$install/launch.sh" || abort "cannot write launch.sh"
    step launch
    echo "$version $candidate_hash" | put "$install/current" || abort "cannot write current"
    step current
    # Activation is the last step.
    render "$start_template" | put "$start_script" || abort "cannot write $start_script"
    rm -rf "$backup"
    for old in "$install"/*/trimui_inputd; do
        [ -f "$old" ] || continue
        [ "${old%/trimui_inputd}" = "$target" ] && continue
        running=0
        for pid in $(pidof trimui_inputd 2>/dev/null); do
            [ "$(readlink "/proc/$pid/exe")" = "$old" ] && running=1
        done
        [ "$running" -eq 0 ] && rm -rf "${old%/trimui_inputd}"
    done
    sync
    echo "result=installed"
    echo "version=$version"
    echo "elf=$target/trimui_inputd"
    echo "start_script=$start_script"
}

wait_players() {
    local want=$1 tenths=$2 i=0
    while [ "$i" -lt "$tenths" ]; do
        [ "$(players)" = "$want" ] && return 0
        sleep 0.1
        i=$((i + 1))
    done
    [ "$(players)" = "$want" ]
}

rollback() {
    local official_before pid exe waited proc starts_created=0 parent_created=0 install_created=0
    official_before=$(hash_of "$official")
    # First, the next boot no longer starts BrickOS.
    if [ -f "$start_script" ]; then
        grep -qx "$start_marker" "$start_script" || fail "$start_script was not written by BrickOS"
        mv "$start_script" "$start_script.removing" && rm -f "$start_script.removing" && sync ||
            fail "cannot remove $start_script"
    fi
    if [ "$boot" = 1 ]; then
        # The firmware loop, which has not run yet, starts the official daemon.
        echo "daemons=left to the firmware loop"
    else
        # The supervisor would start the official daemon after BrickOS stops; it is stopped
        # first so this rollback starts exactly one.
        for proc in /proc/[0-9]*; do
            case "$(tr "\0" " " 2>/dev/null < "$proc/cmdline")" in
                *"$install/launch.sh"*) kill -TERM "${proc#/proc/}" 2>/dev/null && echo "stopped_launcher=${proc#/proc/}" ;;
            esac
        done
        for pid in $(pidof trimui_inputd 2>/dev/null); do
            exe=$(readlink "/proc/$pid/exe" 2>/dev/null)
            case "$exe" in
                "$install"/*)
                    kill -TERM "$pid" 2>/dev/null
                    waited=0
                    while kill -0 "$pid" 2>/dev/null && [ "$waited" -lt $((stop_timeout_s * 10)) ]; do
                        sleep 0.1; waited=$((waited + 1))
                    done
                    kill -0 "$pid" 2>/dev/null && kill -KILL "$pid" 2>/dev/null && sleep 0.2
                    kill -0 "$pid" 2>/dev/null && fail "BrickOS pid $pid did not stop"
                    echo "stopped=$pid $exe" ;;
            esac
        done
        if [ -z "$(pidof trimui_inputd 2>/dev/null)" ]; then
            wait_players 0 30 || echo "warning=a TRIMUI Player1 is still present"
            (cd "$official_dir" && LD_LIBRARY_PATH=$official_libs setsid ./trimui_inputd </dev/null >/dev/null 2>&1 &)
            wait_players 1 50 && echo "started=$official" || fail "the official daemon did not publish TRIMUI Player1"
        fi
    fi
    if [ -d "$install" ]; then
        [ -f "$record" ] && . "$record"
        for file in "$install"/* "$install"/.[!.]*; do
            [ -e "$file" ] || continue
            [ "$file" = "$install/inputd.json" ] && continue
            rm -rf "$file"
        done
        [ "$install_created" = 1 ] && rmdir "$install" 2>/dev/null
        [ "$parent_created" = 1 ] && rmdir "$install_parent" 2>/dev/null
        [ "$starts_created" = 1 ] && rmdir "$starts" 2>/dev/null
    fi
    sync
    [ -f "$start_script" ] && fail "$start_script is still present"
    [ -e "$install/launch.sh" ] && fail "$install/launch.sh is still present"
    [ "$(hash_of "$official")" = "$official_before" ] || fail "official daemon changed during rollback"
    echo "result=rolled-back"
    echo "players_after=$(players)"
    producers | sed 's/^/producer /'
}

case "$command" in
    preflight) preflight ;;
    install) install_inputd ;;
    rollback) rollback ;;
    *) echo "unknown command $command" >&2; exit 2 ;;
esac
