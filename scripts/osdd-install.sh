#!/bin/sh
# Device-side helper for the opt-in persistent BrickOS OSD on the TrimUI Brick.
#
#   osdd-install.sh preflight CONFIG   Report the state an install depends on, and its
#                                      fingerprint. Changes nothing.
#   osdd-install.sh install CONFIG     Stage BrickOS for the next boot if the state still
#                                      has the fingerprint of the dry run.
#   osdd-install.sh rollback CONFIG    Restore the SD state from before the first install
#                                      and hand a running BrickOS OSD back to the OSD the
#                                      firmware would start.
#
# With boot=1 in CONFIG, the helper runs at boot before the firmware's daemon loop, so no
# OSD runs yet: install also accepts none, and neither command stops or starts an OSD. The
# loop then starts the OSD it selects after a rollback.
#
# CONFIG is a shell fragment. Every path is below `root`, which is empty on the device.
# Nothing under /usr/trimui is written. Files are staged under a temporary name and
# renamed into place; an executing image is never overwritten.
set -u

command=${1:-}
config=${2:-}
[ -n "$command" ] && [ -f "$config" ] || { echo "usage: osdd-install.sh preflight|install|rollback CONFIG" >&2; exit 2; }
root=''
expected_fingerprint=''
candidate=''
candidate_hash=''
start_template=''
launch_template=''
start_wait_s=3
stop_timeout_s=5
boot=''
. "$config"

sd=$root/mnt/SDCARD
starts=$sd/System/starts
start_script=$starts/brickos-osdd.sh
install_parent=$sd/BrickOS
install=$install_parent/osd
record=$install/install-record
official_dir=$root/usr/trimui/osd
official=$official_dir/trimui_osdd
official_libs=$root/usr/trimui/lib
runtrimui=$root/usr/trimui/bin/runtrimui.sh
sd_bundle=$sd/trimui
udisk=$root/mnt/UDISK/trimui
udisk_img=$root/mnt/UDISK/trimui.img
boot_log=$root/tmp/brickos-osdd-boot.log
health=$root/tmp/brickos-osdd.health
panel_signals="$root/tmp/trimui_osd/osdd_show_up $root/tmp/trimui_inputd/grab"
start_marker='# BrickOS OSD start script'

hash_of() { if [ -f "$1" ]; then sha256sum "$1" | cut -d ' ' -f 1; else echo absent; fi; }
fail() { echo "result=refused"; echo "reason=$*"; exit 3; }

writers() {
    for pid in $(pidof trimui_osdd 2>/dev/null); do
        echo "pid=$pid exe=$(readlink "/proc/$pid/exe" 2>/dev/null) cwd=$(readlink "/proc/$pid/cwd" 2>/dev/null)"
    done
}

# The entry that makes /mnt/SDCARD/trimui an SDCARD bundle, if any: anything other than a res
# directory. The firmware takes its SD branch whenever the folder exists, but a tree that holds
# only res/, where RetroArch finds its loading screen, has no OSD, so the next branch starts one.
# An empty tree is no bundle either.
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

# The OSD the firmware starts when no trimui_osdd runs, in its SDCARD, UDISK, /usr order.
next_osd() {
    if sd_tree_entry "$sd_bundle" >/dev/null; then echo "$sd_bundle/osd/trimui_osdd $sd_bundle/osd $sd_bundle/lib"
    elif [ -f "$udisk_img" ] && [ -d "$udisk/osd" ]; then echo "$udisk/osd/trimui_osdd $udisk/osd $udisk/lib"
    else echo "$official $official_dir $official_libs"
    fi
}

state() {
    echo "official_hash=$(hash_of "$official")"
    # The official daemon, identified by its path, is the fallback of every boot.
    [ -f "$official" ] && [ -x "$official" ] && echo "official_present=yes" || echo "official_present=no"
    echo "usr_hash runtrimui.sh=$(hash_of "$runtrimui")"
    writers | sed 's/^/writer /'
    echo "writers=$(writers | wc -l)"
    if sd_tree_entry "$sd_bundle" >/dev/null; then echo "sd_bundle=present"
    elif [ -d "$sd_bundle" ]; then echo "sd_bundle=loading-screen"
    else echo "sd_bundle=absent"; fi
    echo "sd_osd=$(hash_of "$sd_bundle/osd/trimui_osdd")"
    echo "udisk_osd=$(hash_of "$udisk/osd/trimui_osdd")"
    [ -f "$udisk_img" ] && echo "udisk_img=present" || echo "udisk_img=absent"
    echo "next_osd=$(next_osd | cut -d ' ' -f 1)"
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
        [ "$(hash_of "$install/$version/trimui_osdd")" = "$candidate_hash" ] &&
        [ -f "$launch_template" ] && [ "$(render "$launch_template" | sha256sum | cut -d ' ' -f 1)" = "$(hash_of "$install/launch.sh")" ] &&
        [ -f "$start_template" ] && [ "$(render "$start_template" | sha256sum | cut -d ' ' -f 1)" = "$(hash_of "$start_script")" ]
}

# Writes a file from standard input under a temporary name and renames it into place.
put() {
    local target=$1 temporary="$1.brickos-tmp"
    cat > "$temporary" && chmod 755 "$temporary" && sync && mv "$temporary" "$target"
}

render() {
    sed -e "s#@INSTALL_DIR@#$install#g" -e "s#@OFFICIAL_DIR@#$official_dir#g" \
        -e "s#@OFFICIAL_LIBS@#$official_libs#g" -e "s#@BOOT_LOG@#$boot_log#g" -e "s#@HEALTH@#$health#g" \
        -e "s#@START_WAIT_S@#$start_wait_s#g" -e "s#@PANEL_SIGNALS@#$panel_signals#g" "$1"
}

install_osd() {
    local report fingerprint version target
    report=$(preflight)
    fingerprint=$(echo "$report" | sed -n 's/^fingerprint=//p')
    [ -n "$expected_fingerprint" ] && [ "$fingerprint" = "$expected_fingerprint" ] ||
        fail "the device state changed since the dry run (fingerprint $fingerprint)"
    echo "$report" | grep -qx 'official_present=yes' || fail "official OSD $official is missing or not executable"
    if [ "$boot" = 1 ]; then
        echo "$report" | grep -qx 'writers=[01]' || fail "expected at most one OSD writer at boot"
    else
        echo "$report" | grep -qx 'writers=1' || fail "expected exactly one OSD writer"
    fi
    [ -f "$candidate" ] && [ "$(hash_of "$candidate")" = "$candidate_hash" ] || fail "staged BrickOS OSD does not match its hash"
    [ -f "$start_template" ] && [ -f "$launch_template" ] || fail "missing start or launch template"
    grep -qx "$start_marker" "$start_template" || fail "start template lacks its marker"
    if [ -f "$start_script" ] && ! grep -qx "$start_marker" "$start_script"; then
        fail "$start_script exists and was not written by BrickOS"
    fi
    version=$(echo "$candidate_hash" | cut -c 1-12)
    target=$install/$version
    # The state before the first install, which rollback restores.
    if [ ! -f "$record" ]; then
        local starts_created=0 parent_created=0
        [ -d "$starts" ] || starts_created=1
        [ -d "$install_parent" ] || parent_created=1
        mkdir -p "$starts" "$install" || fail "cannot create $starts or $install"
        printf 'starts_created=%s\nparent_created=%s\n' "$starts_created" "$parent_created" | put "$record" ||
            fail "cannot write $record"
    fi
    mkdir -p "$target" || fail "cannot create $target"
    if [ "$(hash_of "$target/trimui_osdd")" != "$candidate_hash" ]; then
        # A different image under this version name is replaced by rename; the running
        # image, if any, keeps its own file.
        put "$target/trimui_osdd" < "$candidate" || fail "cannot stage $target/trimui_osdd"
        [ "$(hash_of "$target/trimui_osdd")" = "$candidate_hash" ] || fail "staged image does not verify"
    fi
    render "$launch_template" | put "$install/launch.sh" || fail "cannot write launch.sh"
    echo "$version $candidate_hash" | put "$install/current" || fail "cannot write current"
    # Activation is the last step.
    render "$start_template" | put "$start_script" || fail "cannot write $start_script"
    # Older versions are removed unless a process still executes them.
    for old in "$install"/*/trimui_osdd; do
        [ -f "$old" ] || continue
        [ "${old%/trimui_osdd}" = "$target" ] && continue
        running=0
        for pid in $(pidof trimui_osdd 2>/dev/null); do
            [ "$(readlink "/proc/$pid/exe")" = "$old" ] && running=1
        done
        [ "$running" -eq 0 ] && rm -rf "${old%/trimui_osdd}"
    done
    sync
    echo "result=installed"
    echo "version=$version"
    echo "elf=$target/trimui_osdd"
    echo "start_script=$start_script"
    echo "official_hash_after=$(hash_of "$official")"
    echo "writers_after=$(writers | wc -l)"
}

wait_writers() {
    local want=$1 limit=$2 waited=0
    while [ "$waited" -lt "$limit" ]; do
        [ "$(writers | wc -l)" -eq "$want" ] && return 0
        sleep 0.2
        waited=$((waited + 1))
    done
    [ "$(writers | wc -l)" -eq "$want" ]
}

start_osd() {
    local exe=$1 cwd=$2 libs=$3
    ( cd "$cwd" && LD_LIBRARY_PATH=$libs exec start-stop-daemon -S -b -x "$exe" ) </dev/null >/dev/null 2>&1
    wait_writers 1 25 && [ "$(writers | sed -n 's/^pid=[0-9]* exe=\([^ ]*\) .*/\1/p')" = "$exe" ]
}

# Stops a running BrickOS OSD and its launcher, and starts the OSD the firmware selects
# when none is left.
hand_back() {
    local pid exe waited next cwd libs
    # The launcher that waits on BrickOS would start the official OSD after the stop, so
    # it is stopped first; BrickOS keeps running without it.
    for proc in /proc/[0-9]*; do
        # The error redirection comes first: a process can exit before its cmdline opens.
        case "$(tr "\0" " " 2>/dev/null < "$proc/cmdline")" in
            *"$install/launch.sh"*) kill -TERM "${proc#/proc/}" 2>/dev/null && echo "stopped_launcher=${proc#/proc/}" ;;
        esac
    done
    # Then a running BrickOS OSD is stopped and replaced by the OSD the firmware selects.
    for pid in $(pidof trimui_osdd 2>/dev/null); do
        exe=$(readlink "/proc/$pid/exe" 2>/dev/null)
        case "$exe" in
            "$install"/*)
                kill -TERM "$pid" 2>/dev/null
                waited=0
                while kill -0 "$pid" 2>/dev/null && [ "$waited" -lt $((stop_timeout_s * 5)) ]; do
                    sleep 0.2; waited=$((waited + 1))
                done
                # A killed daemon cannot remove its panel signals.
                kill -0 "$pid" 2>/dev/null && kill -KILL "$pid" 2>/dev/null && sleep 0.2 && rm -f $panel_signals
                kill -0 "$pid" 2>/dev/null && fail "BrickOS OSD pid $pid did not stop"
                echo "stopped=$pid $exe" ;;
        esac
    done
    # Whenever no OSD is left running, the one the firmware selects is started.
    if [ "$(writers | wc -l)" -eq 0 ]; then
        next=$(next_osd)
        exe=${next%% *}; cwd=$(echo "$next" | cut -d ' ' -f 2); libs=${next##* }
        if start_osd "$exe" "$cwd" "$libs"; then
            echo "started=$exe"
        else
            echo "warning=$exe did not start; starting the official OSD"
            for pid in $(pidof trimui_osdd 2>/dev/null); do kill -KILL "$pid" 2>/dev/null; done
            wait_writers 0 25
            start_osd "$official" "$official_dir" "$official_libs" && echo "started=$official" ||
                fail "no OSD could be started"
        fi
    fi
}

rollback() {
    local official_before
    official_before=$(hash_of "$official")
    # First, the next boot no longer starts BrickOS.
    if [ -f "$start_script" ]; then
        grep -qx "$start_marker" "$start_script" || fail "$start_script was not written by BrickOS"
        mv "$start_script" "$start_script.removing" && rm -f "$start_script.removing" && sync ||
            fail "cannot remove $start_script"
    fi
    if [ "$boot" = 1 ]; then
        # The firmware loop, which has not run yet, starts the OSD it selects.
        echo "daemons=left to the firmware loop"
    else
        hand_back
    fi
    # Finally, the files the install added, and the directories it created. A frontend's theme
    # profile, or any other file BrickOS did not write, stays with its directory.
    if [ -d "$install" ]; then
        starts_created=0; parent_created=0
        [ -f "$record" ] && . "$record"
        for old in "$install"/*/trimui_osdd; do
            [ -f "$old" ] && rm -rf "${old%/trimui_osdd}"
        done
        rm -f "$install/launch.sh" "$install/current" "$install/DISABLED" "$record"
        rmdir "$install" 2>/dev/null
        [ "$parent_created" = 1 ] && rmdir "$install_parent" 2>/dev/null
        [ "$starts_created" = 1 ] && rmdir "$starts" 2>/dev/null
    fi
    sync
    [ -f "$start_script" ] && fail "$start_script is still present"
    for left in "$install/launch.sh" "$install/current" "$record" "$install"/*/trimui_osdd; do
        [ -e "$left" ] && fail "$left is still present"
    done
    [ "$(hash_of "$official")" = "$official_before" ] || fail "official OSD changed during rollback"
    echo "result=rolled-back"
    echo "writers_after=$(writers | wc -l)"
    writers | sed 's/^/writer /'
}

case "$command" in
    preflight) preflight ;;
    install) install_osd ;;
    rollback) rollback ;;
    *) echo "unknown command $command" >&2; exit 2 ;;
esac
