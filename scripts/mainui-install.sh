#!/bin/sh
# Device-side helper for the opt-in BrickOS frontend.
#
#   mainui-install.sh preflight CONFIG   Report the state an install depends on, and its
#                                        fingerprint. Changes nothing.
#   mainui-install.sh install CONFIG     Stage BrickOS for the next boot if the state still has
#                                        the fingerprint of the dry run.
#   mainui-install.sh rollback CONFIG    Remove the start script and the BrickOS files, and
#                                        undo the binds when no BrickOS frontend runs.
#
# CONFIG is a shell fragment; every path is below `root`, which is empty on the device. The
# install adds a start script to /mnt/SDCARD/System/starts and files under
# /mnt/SDCARD/BrickOS/mainui: <version>/MainUI, wrapper.sh, current ("version elf-hash
# wrapper-hash presentation"), and a record of the folders it created. `presentation` is
# stock, material-you, or auto, and defaults to stock. auto, which the release package's setup
# uses, keeps the presentation an existing install has (stock when it names none or an invalid
# one) and gives a fresh install material-you. It never creates /mnt/SDCARD/trimui, never writes
# under /usr, and binds nothing itself: the start script binds at boot. Files are staged under a
# temporary name and renamed into place, the start script last; a running image is never
# overwritten. When the installed build, wrapper, start script and presentation already match,
# the preflight reports up_to_date=yes and the install writes nothing.
#
# The rollback removes only what an install writes, and keeps the frontend's preferences in
# BrickOS/mainui/brickos. While the BrickOS frontend runs, its files cannot go: the rollback
# then writes a one-shot System/starts/brickos-mainui-cleanup.sh, which removes them at the next
# boot, before any frontend starts, and then itself. A later install removes that script.
set -u

command=${1:-}
config=${2:-}
[ -n "$command" ] && [ -f "$config" ] || { echo "usage: mainui-install.sh preflight|install|rollback CONFIG" >&2; exit 2; }
root=''
expected_fingerprint=''
candidate=''
candidate_hash=''
start_template=''
wrapper_template=''
mounts=/proc/mounts
max_failures=3
presentation=stock
# Host tests only: the install step after which a failure is simulated.
inject_failure=''
. "$config"
case "$presentation" in
    stock|material-you|auto) ;;
    *) echo "result=refused"; echo "reason=presentation must be stock, material-you or auto"; exit 3 ;;
esac

sd=$root/mnt/SDCARD
starts=$sd/System/starts
start_script=$starts/brickos-mainui.sh
install_parent=$sd/BrickOS
install=$install_parent/mainui
record=$install/install-record
official=$root/usr/trimui/bin/MainUI
private=$root/tmp/brickos-mainui-official/MainUI
run_dir=$root/tmp/brickos-mainui-run
boot_log=$root/tmp/brickos-mainui-boot.log
sd_tree=$sd/trimui
start_marker='# BrickOS frontend start script'
cleanup_script=$starts/brickos-mainui-cleanup.sh
cleanup_marker='# BrickOS frontend cleanup script'
# An installed build's folder is named by the first 12 hex digits of its image's hash.
hex12='[0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f]'

hash_of() { if [ -f "$1" ]; then sha256sum "$1" | cut -d ' ' -f 1; else echo absent; fi; }
# The presentation an install writes: auto keeps an existing install's (stock when it names none
# or an invalid one) and gives a fresh install material-you.
resolved_presentation() {
    local v e w p
    [ "$presentation" = auto ] || { echo "$presentation"; return; }
    if [ -f "$install/current" ]; then
        read -r v e w p < "$install/current"
        case "$p" in material-you) echo material-you ;; *) echo stock ;; esac
        return
    fi
    echo material-you
}
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
# Frontend processes, by executable path (the name MainUI does not identify one).
frontends() {
    local proc exe
    for proc in /proc/[0-9]*; do
        exe=$(readlink "$proc/exe" 2>/dev/null) || continue
        case "$exe" in
            "$official"|"$private"|"$install"/*/MainUI) echo "pid=${proc#/proc/} exe=$exe" ;;
        esac
    done
}
brickos_running() { frontends | grep -q "exe=$install/"; }
mounted_over() { grep -q " $1 " "$mounts" 2>/dev/null; }

state() {
    local file
    echo "presentation=$(resolved_presentation)"
    [ -f "$official" ] && echo "official=present" || echo "official=absent"
    frontends | sed 's/^/frontend /'
    local entry
    if entry=$(sd_tree_entry "$sd_tree"); then echo "sd_trimui_tree=active $entry"
    elif [ -d "$sd_tree" ]; then echo "sd_trimui_tree=loading-screen"
    else echo "sd_trimui_tree=absent"; fi
    mounted_over "$official" && echo "official_mount=present" || echo "official_mount=absent"
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

# 0 when the candidate is installed as it would be now: its image, the rendered wrapper and start
# script, and the presentation, with no cleanup pending.
up_to_date() {
    local v e w p version
    [ -n "$candidate_hash" ] && [ -f "$install/current" ] && [ -f "$start_script" ] || return 1
    [ ! -e "$cleanup_script" ] || return 1
    read -r v e w p < "$install/current"
    version=$(echo "$candidate_hash" | cut -c 1-12)
    [ "$v" = "$version" ] && [ "$e" = "$candidate_hash" ] && [ "$p" = "$(resolved_presentation)" ] || return 1
    [ "$(hash_of "$install/$version/MainUI")" = "$candidate_hash" ] || return 1
    [ -f "$wrapper_template" ] && [ -f "$start_template" ] || return 1
    [ "$(hash_of "$install/wrapper.sh")" = "$w" ] &&
        [ "$(render "$wrapper_template" | sha256sum | cut -d ' ' -f 1)" = "$w" ] || return 1
    [ "$(render "$start_template" | sha256sum | cut -d ' ' -f 1)" = "$(hash_of "$start_script")" ]
}

preflight() {
    local lines
    lines=$(state)
    echo "$lines"
    echo "fingerprint=$(echo "$lines" | grep -v '^frontend ' | sha256sum | cut -d ' ' -f 1)"
    echo "free_kb=$(df -k "$sd" 2>/dev/null | tail -n 1 | awk '{print $4}')"
    if up_to_date; then echo "up_to_date=yes"; else echo "up_to_date=no"; fi
}

put() {
    local target=$1 temporary="$1.brickos-tmp"
    cat > "$temporary" && chmod 755 "$temporary" && sync && mv "$temporary" "$target"
}

render() {
    sed -e "s#@INSTALL_DIR@#$install#g" -e "s#@OFFICIAL@#$official#g" -e "s#@PRIVATE@#$private#g" \
        -e "s#@MOUNTS@#$mounts#g" -e "s#@RUN_DIR@#$run_dir#g" -e "s#@BOOT_LOG@#$boot_log#g" \
        -e "s#@MAX_FAILURES@#$max_failures#g" -e "s#@SD_TREE@#$sd_tree#g" "$1"
}

install_mainui() {
    local report fingerprint version target old wrapper_hash
    report=$(preflight)
    fingerprint=$(echo "$report" | sed -n 's/^fingerprint=//p')
    [ -n "$expected_fingerprint" ] && [ "$fingerprint" = "$expected_fingerprint" ] ||
        fail "the device state changed since the dry run (fingerprint $fingerprint)"
    echo "$report" | grep -qx 'official=present' || fail "$official is missing; it is the fallback frontend"
    case "$(echo "$report" | sed -n 's/^sd_trimui_tree=//p')" in
        "active "*) fail "$sd_tree holds $(sd_tree_entry "$sd_tree"); this install needs the firmware's /usr branch" ;;
    esac
    [ -f "$candidate" ] && [ "$(hash_of "$candidate")" = "$candidate_hash" ] || fail "staged BrickOS frontend does not match its hash"
    [ -f "$start_template" ] && [ -f "$wrapper_template" ] || fail "missing start or wrapper template"
    grep -qx "$start_marker" "$start_template" || fail "start template lacks its marker"
    if [ -f "$start_script" ] && ! grep -qx "$start_marker" "$start_script"; then
        fail "$start_script exists and was not written by BrickOS"
    fi
    if [ -f "$cleanup_script" ] && ! grep -qx "$cleanup_marker" "$cleanup_script"; then
        fail "$cleanup_script exists and was not written by BrickOS"
    fi
    if up_to_date; then
        echo "result=up-to-date"
        echo "version=$(echo "$candidate_hash" | cut -c 1-12)"
        return 0
    fi
    version=$(echo "$candidate_hash" | cut -c 1-12)
    target=$install/$version
    # From here on, a failure puts every file and folder back as it was before this install.
    local had_starts=0 had_parent=0 had_install=0 had_target=0 name backup
    [ -d "$starts" ] && had_starts=1
    [ -d "$install_parent" ] && had_parent=1
    [ -d "$install" ] && had_install=1
    [ -d "$target" ] && had_target=1
    backup=$(mktemp -d "$root/tmp/brickos-mainui-install.XXXXXX") || fail "cannot create a backup folder"
    for name in wrapper.sh current install-record; do
        [ -f "$install/$name" ] && cp -p "$install/$name" "$backup/$name"
    done
    [ -f "$start_script" ] && cp -p "$start_script" "$backup/start-script"
    [ -f "$cleanup_script" ] && cp -p "$cleanup_script" "$backup/cleanup-script"
    abort() {
        for name in wrapper.sh current install-record; do
            rm -f "$install/$name.brickos-tmp"
            if [ -f "$backup/$name" ]; then mv "$backup/$name" "$install/$name"; else rm -f "$install/$name"; fi
        done
        rm -f "$start_script.brickos-tmp"
        if [ -f "$backup/start-script" ]; then mv "$backup/start-script" "$start_script"; else rm -f "$start_script"; fi
        [ -f "$backup/cleanup-script" ] && mv "$backup/cleanup-script" "$cleanup_script"
        rm -f "$target/MainUI.brickos-tmp"
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
    if [ "$(hash_of "$target/MainUI")" != "$candidate_hash" ]; then
        put "$target/MainUI" < "$candidate" || abort "cannot stage $target/MainUI"
        [ "$(hash_of "$target/MainUI")" = "$candidate_hash" ] || abort "staged image does not verify"
    fi
    step elf
    render "$wrapper_template" | put "$install/wrapper.sh" || abort "cannot write wrapper.sh"
    wrapper_hash=$(hash_of "$install/wrapper.sh")
    step wrapper
    echo "$version $candidate_hash $wrapper_hash $(resolved_presentation)" | put "$install/current" || abort "cannot write current"
    step current
    # Activation is the last step. A cleanup left by an earlier rollback would remove this
    # install at the next boot, so it goes.
    render "$start_template" | put "$start_script" || abort "cannot write $start_script"
    rm -f "$cleanup_script"
    rm -rf "$backup"
    for old in "$install"/*/MainUI; do
        [ -f "$old" ] || continue
        [ "${old%/MainUI}" = "$target" ] && continue
        frontends | grep -q "exe=$old\$" || rm -rf "${old%/MainUI}"
    done
    sync
    echo "result=installed"
    echo "version=$version"
    echo "elf=$target/MainUI"
    echo "start_script=$start_script"
}

# An entry of BrickOS/mainui that an install wrote: a build folder, the wrapper, the manifest,
# the record, or a staged file. Everything else, such as the preferences in brickos/, stays.
owned() {
    case "${1##*/}" in
        wrapper.sh|current|install-record|*.brickos-tmp) return 0 ;;
        $hex12) [ -d "$1" ] ;;
        *) return 1 ;;
    esac
}
# The one-shot boot script that removes, before any frontend starts, the files a rollback had to
# leave while the BrickOS frontend ran, and then itself.
write_cleanup() {
    local starts_created=0 parent_created=0 install_created=0
    [ -f "$record" ] && . "$record"
    put "$cleanup_script" <<EOF
#!/bin/sh
$cleanup_marker
# Written by a frontend rollback while the BrickOS frontend ran. At this boot no start script
# binds it any more: its remaining files go, then this script. Its preferences stay.
install='$install'
for file in "\$install"/*; do
    [ -e "\$file" ] || continue
    case "\${file##*/}" in
        wrapper.sh|current|install-record|*.brickos-tmp) rm -rf "\$file" ;;
        $hex12) [ -d "\$file" ] && rm -rf "\$file" ;;
    esac
done
[ "$install_created" = 1 ] && rmdir "\$install" 2>/dev/null
[ "$parent_created" = 1 ] && rmdir '$install_parent' 2>/dev/null
rm -f '$cleanup_script'
[ "$starts_created" = 1 ] && rmdir '$starts' 2>/dev/null
sync
EOF
}

rollback() {
    local official_before starts_created=0 parent_created=0 install_created=0 file
    # While the wrapper is bound over it, the official file is reached through its private bind.
    official_file() { if mounted_over "$official" && mounted_over "$private"; then echo "$private"; else echo "$official"; fi; }
    official_before=$(hash_of "$(official_file)")
    # First, the next boot no longer binds anything.
    if [ -f "$start_script" ]; then
        grep -qx "$start_marker" "$start_script" || fail "$start_script was not written by BrickOS"
        mv "$start_script" "$start_script.removing" && rm -f "$start_script.removing" && sync ||
            fail "cannot remove $start_script"
    fi
    # The binds only exist until the next boot; they are undone now unless BrickOS runs.
    if mounted_over "$official"; then
        if brickos_running; then
            echo "binds=left-until-reboot"
        else
            umount "$official" 2>/dev/null || fail "cannot undo the bind over $official"
            mounted_over "$private" && umount "$private" 2>/dev/null
            echo "binds=removed"
        fi
    fi
    if [ -d "$install" ] && ! brickos_running; then
        [ -f "$record" ] && . "$record"
        for file in "$install"/*; do
            [ -e "$file" ] || continue
            owned "$file" && rm -rf "$file"
        done
        rm -f "$cleanup_script"
        [ "$install_created" = 1 ] && rmdir "$install" 2>/dev/null
        [ "$parent_created" = 1 ] && rmdir "$install_parent" 2>/dev/null
        [ "$starts_created" = 1 ] && rmdir "$starts" 2>/dev/null
    elif [ -d "$install" ]; then
        echo "files=left-while-brickos-runs"
        write_cleanup || fail "cannot write $cleanup_script"
        echo "cleanup=next-boot"
    fi
    sync
    [ -f "$start_script" ] && fail "$start_script is still present"
    [ "$(hash_of "$(official_file)")" = "$official_before" ] || fail "official MainUI changed during rollback"
    echo "result=rolled-back"
}

case "$command" in
    preflight) preflight ;;
    install) install_mainui ;;
    rollback) rollback ;;
    *) echo "unknown command $command" >&2; exit 2 ;;
esac
