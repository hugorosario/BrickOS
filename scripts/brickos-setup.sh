#!/bin/sh
# BrickOS setup. Installs, updates, or removes the BrickOS input daemon, OSD, LED daemon, MainUI,
# GameSwitcher and in-game menu from a release package, at boot from the SD card or on request
# over ADB. It is packaged as BrickOS/package/<build>/setup.sh.
#
#   setup.sh preflight [--boot]   Verify the package and report every path an install would
#                                 write, the launchers it would change and skip, the installs
#                                 it would adopt, any refusal, and one fingerprint of the
#                                 device state. Changes nothing.
#   setup.sh install [--boot]     Install or update every component to this package's build.
#                                 A run either completes or puts back every file it touched.
#   setup.sh rollback [--boot]    Remove every component, put back the launchers BrickOS
#                                 changed, and leave the official daemons in charge.
#
# --boot means the firmware's daemon loop has not run yet, so no daemon is stopped or started.
# The setup drives the component helpers in the order input daemon, OSD, LED daemon, MainUI,
# GameSwitcher, menu, and rolls back in the reverse order. MainUI's helper is
# mainui/mainui-install.sh and GameSwitcher's is gameswitcher/gameswitcher-setup.sh; the others
# are helpers/<component>-install.sh. GameSwitcher takes the RetroArch launchers its launch check
# accepts, from Emus/*/launch*.sh, and the menu goes into every other /mnt/SDCARD/Emus/*/launch.sh
# with exactly one standard RetroArch command. An activation by GameSwitcher's own earlier
# installer is undone first, before the input daemon, whose producer it then no longer is.
# Writes stay under /mnt/SDCARD: BrickOS/, the BrickOS start scripts in System/starts/, the
# Uninstall BrickOS app in Apps/BrickOSUninstall/, GameSwitcher's app in Apps/GameSwitcher/,
# and those launchers.
# The app is the package's uninstall/ folder: it is installed or replaced on a fresh install or
# an update to another build, removed once every component has rolled back, and a folder of
# that name whose launch.sh lacks the app's marker line refuses the setup; so does an
# Apps/GameSwitcher that GameSwitcher's helper did not write. Nothing else in Apps/ is written.
# A rollback while the BrickOS MainUI runs leaves its files to a one-shot cleanup start script,
# so the frontend is the official one at the next boot. Nothing under /usr/trimui is written, /mnt/SDCARD/trimui
# is never created or changed, and no stock version or hash is checked: the official daemons
# are known by their path and process. A /mnt/SDCARD/trimui with any entry other than res/
# refuses the setup, since the firmware then runs its SD branch; one that holds only res/,
# RetroArch's loading screen, does not. Boot visuals and Settings icons are added only where
# their file name is absent, and stay as user data on rollback. The LED configuration
# BrickOS/ledd/config.json is user data too: the setup never writes or removes it.
# Loading-screen presets are added as whole folders, only where no entry of that name exists,
# and stay too. A MainUI wallpaper is added where absent and replaces an earlier BrickOS version
# (listed in the package's wallpapers/earlier.sha256); any other file of that name is the user's
# and stays. The wallpapers stay on rollback as well. A copy of this script
# and the helpers in BrickOS/setup/ lets the boot script roll back when the package is gone.
# Every install and rollback appends a block to BrickOS/setup.log and to /tmp/brickos-setup.log.
set -u

usage() { echo "usage: setup.sh preflight|install|rollback [--boot]" >&2; exit 2; }
command=${1:-}
boot=0
case "${2:-}" in --boot) boot=1 ;; '') ;; *) usage ;; esac
case "$command" in preflight|install|rollback) ;; *) usage ;; esac

package=$(cd "$(dirname "$0")" && pwd)
root=${BRICKOS_ROOT:-}
devices=/proc/bus/input/devices
mounts=/proc/mounts
start_wait_tenths=30
start_wait_s=3
restart_delay_s=5
stop_timeout_s=5
# Host tests only: the step after which a failure is simulated.
inject_failure=''
[ -n "${BRICKOS_SETUP_CONFIG:-}" ] && . "$BRICKOS_SETUP_CONFIG"

sd=$root/mnt/SDCARD
brickos=$sd/BrickOS
starts=$sd/System/starts
boot_script=$starts/brickos-00-setup.sh
setup_record=$brickos/setup-record
versions_file=$brickos/versions.json
installed_copy=$brickos/setup
log=$brickos/setup.log
tmp_log=$root/tmp/brickos-setup.log
helpers=$package/helpers
build=${package##*/}
boot_marker='# BrickOS setup start script'
inputd_marker='# BrickOS input daemon start script'
osdd_marker='# BrickOS OSD start script'
ledd_marker='# BrickOS LED daemon start script'
mainui_marker='# BrickOS frontend start script'
mainui_cleanup_marker='# BrickOS frontend cleanup script'
# The components in install order; rollback runs them backwards.
components='inputd osdd ledd mainui gameswitcher tmenu'
# The helper copies the installed setup keeps, relative to the package folder.
helper_files='helpers/inputd-install.sh helpers/osdd-install.sh helpers/ledd-install.sh helpers/tmenu-install.sh
mainui/mainui-install.sh gameswitcher/gameswitcher-setup.sh'
app=$sd/Apps/BrickOSUninstall
app_marker='# BrickOS uninstall app'
updater=$brickos/settings/Updates
updater_marker='# BrickOS updater settings'
official_inputd=$root/usr/trimui/bin/trimui_inputd
# The OSDs the firmware itself starts: /usr, or its UDISK update image.
official_osdds="$root/usr/trimui/osd/trimui_osdd $root/mnt/UDISK/trimui/osd/trimui_osdd"
visual_kinds='bootlogos splashscreens icons/settings'

hash_of() { if [ -f "$1" ]; then sha256sum "$1" | cut -d ' ' -f 1; else echo absent; fi; }
digest() { sha256sum | cut -d ' ' -f 1; }
device() { echo "${1#"$root"}"; }
val() { sed -n "s/^$2=//p" "$1" 2>/dev/null | tail -n 1; }
# Prints a line and keeps it for the log.
say() { echo "$*"; echo "$*" >> "$work/summary"; }

if [ "$package" = "$installed_copy" ]; then
    [ "$command" = rollback ] || { echo "result=refused"; echo "reason=the installed copy only rolls back"; exit 3; }
    build=installed
else
    case "$build" in ''|*[!0-9a-f]*) echo "result=refused"; echo "reason=the package folder is not named by its build"; exit 3 ;; esac
fi
mkdir -p "$root/tmp"
work=$(mktemp -d "$root/tmp/brickos-setup.XXXXXX") || { echo "result=refused"; echo "reason=cannot create a work folder"; exit 3; }
trap 'rm -rf "$work"' EXIT
: > "$work/summary"

log_run() {
    local file
    for file in "$log" "$tmp_log"; do
        [ -d "${file%/*}" ] || continue
        {
            echo "== $(date '+%Y-%m-%d %H:%M:%S %Z') $command$([ "$boot" = 1 ] && echo ' --boot') build=$build"
            cat "$work/summary"
        } >> "$file"
        # Past 64 KiB, the log keeps its last 32 KiB from the first whole line.
        if [ "$(($(wc -c < "$file")))" -gt 65536 ]; then
            tail -c 32768 "$file" | sed '1d' > "$file.brickos-tmp" && mv "$file.brickos-tmp" "$file"
        fi
    done
    sync
}

# Ends a run that changed nothing, or whose changes were all put back.
refused() {
    say "result=$1"
    shift
    for reason do say "reason=$reason"; done
    [ "$command" = preflight ] || log_run
    exit 3
}

# Verifies every packaged file against the manifest's size and SHA-256.
check_manifest() {
    local hash size path file bad=0
    [ -f "$package/manifest.txt" ] || { echo "manifest.txt is missing"; return 1; }
    [ "$(digest < "$package/manifest.txt" | cut -c 1-12)" = "$build" ] ||
        { echo "manifest.txt does not match build $build"; return 1; }
    while read -r hash size path; do
        file=$package/$path
        if [ ! -f "$file" ]; then echo "$path is missing"; bad=1
        elif [ "$(($(wc -c < "$file")))" != "$size" ] || [ "$(hash_of "$file")" != "$hash" ]; then
            echo "$path differs from the manifest"; bad=1
        fi
    done < "$package/manifest.txt"
    return $bad
}
packaged_hash() { awk -v p="$1" '$3 == p { print $1 }' "$package/manifest.txt"; }

config_inputd() {
    cat > "$work/inputd.config" <<EOF
root='$root'
expected_fingerprint='${1:-}'
candidate='$package/bin/trimui_inputd'
candidate_hash='${inputd_hash:-}'
start_template='$package/templates/brickos-inputd-start.sh'
launch_template='$package/templates/brickos-inputd-launch.sh'
devices='$devices'
start_wait_tenths=$start_wait_tenths
stop_timeout_s=$stop_timeout_s
boot=$boot
EOF
}
config_osdd() {
    cat > "$work/osdd.config" <<EOF
root='$root'
expected_fingerprint='${1:-}'
candidate='$package/bin/trimui_osdd'
candidate_hash='${osdd_hash:-}'
start_template='$package/templates/brickos-osdd-start.sh'
launch_template='$package/templates/brickos-osdd-launch.sh'
start_wait_s=$start_wait_s
stop_timeout_s=$stop_timeout_s
boot=$boot
EOF
}
config_ledd() {
    cat > "$work/ledd.config" <<EOF
root='$root'
expected_fingerprint='${1:-}'
candidate='$package/bin/brickos-ledd'
candidate_hash='${ledd_hash:-}'
start_template='$package/templates/brickos-ledd-start.sh'
launch_template='$package/templates/brickos-ledd-launch.sh'
restart_delay_s=$restart_delay_s
stop_timeout_s=$stop_timeout_s
boot=$boot
EOF
}
config_tmenu() {
    cat > "$work/tmenu.config" <<EOF
root='$root'
launchers='$launchers'
candidate='$package/bin/libtmenu.so'
candidate_hash='${tmenu_hash:-}'
build='$(echo "${tmenu_hash:-}" | cut -c 1-12)'
expected_fingerprint='${1:-}'
EOF
}
# A fresh MainUI install gets the BrickOS presentation; an update keeps the one it finds.
config_mainui() {
    cat > "$work/mainui.config" <<EOF
root='$root'
expected_fingerprint='${1:-}'
candidate='$package/mainui/MainUI'
candidate_hash='${mainui_hash:-}'
start_template='$package/mainui/brickos-mainui-start.sh'
wrapper_template='$package/mainui/brickos-mainui-wrapper.sh'
mounts='$mounts'
presentation=auto
EOF
}
config_gameswitcher() {
    cat > "$work/gameswitcher.config" <<EOF
root='$root'
payload='$package/gameswitcher'
expected_fingerprint='${1:-}'
boot=$boot
stop_timeout_s=$stop_timeout_s
EOF
}
helper_path() {
    case "$1" in
        mainui) echo "$package/mainui/mainui-install.sh" ;;
        gameswitcher) echo "$package/gameswitcher/gameswitcher-setup.sh" ;;
        *) echo "$helpers/$1-install.sh" ;;
    esac
}
# Runs a component helper; its report goes to $work/<component>.<command>.
helper() { config_$1 "${3:-}"; sh "$(helper_path "$1")" "$2" "$work/$1.config" > "$work/$1.$2" 2>&1; }
# The launchers GameSwitcher holds after an install, by its preflight's plan; the menu leaves them.
gameswitcher_held() {
    awk '$1 == "plan" && $2 == "launcher" && ($4 == "install" || $4 == "from-menu" || $4 == "migrate" ||
        $4 == "update" || $4 == "current" || $4 == "left") { print $3 }' "$work/gameswitcher.preflight" 2>/dev/null
}
# Takes GameSwitcher's launchers out of the menu's list.
menu_launchers() {
    local path held list=''
    held=$(gameswitcher_held)
    for path in $launchers; do
        echo "$held" | grep -qxF "$path" || list="$list${list:+ }$path"
    done
    launchers=$list
}

# The launchers the menu may change. A path the helpers cannot take as a plain word is skipped.
discover() {
    local file path
    launchers=''; odd_launchers=''
    for file in "$sd"/Emus/*/launch.sh; do
        [ -f "$file" ] || continue
        path=$(device "$file")
        case "$path" in
            *[!A-Za-z0-9_./-]*) odd_launchers="$odd_launchers$path
" ;;
            *) launchers="$launchers${launchers:+ }$path" ;;
        esac
    done
    discovered=$launchers
}

# The packaged visuals and icons whose file name is absent on the card, then every file of each
# packaged loading-screen preset whose folder name is absent, as relative paths. A preset is one
# set, so nothing is ever added to a folder that is already there.
seed_plan() {
    local kind file preset
    for kind in $visual_kinds; do
        for file in "$package/$kind"/*; do
            [ -f "$file" ] || continue
            [ -e "$brickos/$kind/${file##*/}" ] || echo "$kind/${file##*/}"
        done
    done
    for preset in "$package/loading-screens/"*/; do
        preset=${preset%/}
        [ -d "$preset" ] || continue
        if [ -e "$brickos/loading-screens/${preset##*/}" ] || [ -L "$brickos/loading-screens/${preset##*/}" ]; then continue; fi
        for file in "$preset"/*; do
            [ -f "$file" ] && echo "loading-screens/${preset##*/}/${file##*/}"
        done
    done
}

# What the install does with each packaged wallpaper, one line each: "add NAME" when the card has
# none, "replace NAME" when it has an earlier BrickOS version, "keep NAME" when it has anything
# else, which is the user's; nothing when it already has the packaged bytes.
wallpaper_plan() {
    local file name target current
    for file in "$package/wallpapers/"*.png; do
        [ -f "$file" ] || continue
        name=${file##*/}; target=$brickos/wallpapers/$name
        if [ -L "$target" ] || { [ -e "$target" ] && [ ! -f "$target" ]; }; then
            echo "keep $name"
        elif [ ! -e "$target" ]; then
            echo "add $name"
        else
            current=$(hash_of "$target")
            if [ "$current" = "$(hash_of "$file")" ]; then
                :
            elif grep -qxF "$current  $name" "$package/wallpapers/earlier.sha256" 2>/dev/null; then
                echo "replace $name"
            else
                echo "keep $name"
            fi
        fi
    done
}

marked() { [ ! -e "$1" ] || grep -qx "$2" "$1"; }
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
installed_build() { val "$setup_record" build; }
needs_seed() { [ "$(installed_build)" != "$build" ]; }

# The uninstall app's packaged files, relative to uninstall/, one per line.
app_files() { awk '$3 ~ /^uninstall\// { print substr($3, 11) }' "$package/manifest.txt" 2>/dev/null; }
# The app folder is BrickOS's when its launch.sh carries the app's marker line.
app_ours() { [ -d "$app" ] && [ ! -L "$app" ] && grep -qx "$app_marker" "$app/launch.sh" 2>/dev/null; }
# 0 when the app folder holds exactly the package's app files, with their bytes.
app_current() {
    local file listed
    [ -d "$app" ] || return 1
    listed=$(app_files | LC_ALL=C sort)
    [ "$(ls -A "$app" | LC_ALL=C sort)" = "$listed" ] || return 1
    for file in $listed; do
        [ "$(hash_of "$app/$file")" = "$(packaged_hash "uninstall/$file")" ] || return 1
    done
}
# The app folder's entries and their hashes, for the fingerprint.
app_state() {
    local file
    [ -e "$app" ] || [ -L "$app" ] || { echo absent; return; }
    for file in "$app"/* "$app"/.[!.]* "$app"/..?*; do
        [ -e "$file" ] || [ -L "$file" ] || continue
        echo "${file##*/} $(hash_of "$file")"
    done | digest
}
# Like the boot visuals, the app is written on a fresh install or an update to another build,
# and only when its folder does not already hold this build's files.
needs_app() { needs_seed && [ -n "$(app_files)" ] && ! app_current; }

updater_files() { awk '$3 ~ /^updater\// { print substr($3, 9) }' "$package/manifest.txt" 2>/dev/null; }
updater_ours() { [ -d "$updater" ] && [ ! -L "$updater" ] && grep -qx "$updater_marker" "$updater/menu.sh" 2>/dev/null; }
updater_current() {
    local file
    [ -n "$(updater_files)" ] || return 0
    updater_ours || return 1
    for file in $(updater_files); do
        [ "$(hash_of "$updater/$file")" = "$(packaged_hash "updater/$file")" ] || return 1
    done
}
needs_updater() { [ -n "$(updater_files)" ] && ! updater_current; }

# Checks the package and the device, runs every helper's preflight, and prints the plan.
# Sets refusals, the components' hashes, and the up-to-date state.
assess() {
    local problems exe line path kind reason component file entry
    refusals=''
    if ! problems=$(check_manifest); then
        echo "$problems" | sed 's/^/manifest_problem=/'
        refusals="the package does not match its manifest: $(echo "$problems" | head -n 1)
"
        return
    fi
    echo "manifest=ok"
    inputd_hash=$(packaged_hash bin/trimui_inputd)
    osdd_hash=$(packaged_hash bin/trimui_osdd)
    ledd_hash=$(packaged_hash bin/brickos-ledd)
    tmenu_hash=$(packaged_hash bin/libtmenu.so)
    mainui_hash=$(packaged_hash mainui/MainUI)
    if entry=$(sd_tree_entry "$sd/trimui"); then
        refusals="$refusals/mnt/SDCARD/trimui holds $entry, so the firmware runs its SD branch instead of /usr
"
    fi
    [ -n "$(pidof ra64.trimui 2>/dev/null)" ] && refusals="${refusals}a game is running
"
    marked "$starts/brickos-inputd.sh" "$inputd_marker" ||
        refusals="$refusals/mnt/SDCARD/System/starts/brickos-inputd.sh exists and was not written by BrickOS
"
    marked "$starts/brickos-osdd.sh" "$osdd_marker" ||
        refusals="$refusals/mnt/SDCARD/System/starts/brickos-osdd.sh exists and was not written by BrickOS
"
    marked "$starts/brickos-ledd.sh" "$ledd_marker" ||
        refusals="$refusals/mnt/SDCARD/System/starts/brickos-ledd.sh exists and was not written by BrickOS
"
    marked "$boot_script" "$boot_marker" ||
        refusals="$refusals/mnt/SDCARD/System/starts/brickos-00-setup.sh exists and was not written by BrickOS
"
    marked "$starts/brickos-mainui.sh" "$mainui_marker" ||
        refusals="$refusals/mnt/SDCARD/System/starts/brickos-mainui.sh exists and was not written by BrickOS
"
    marked "$starts/brickos-mainui-cleanup.sh" "$mainui_cleanup_marker" ||
        refusals="$refusals/mnt/SDCARD/System/starts/brickos-mainui-cleanup.sh exists and was not written by BrickOS
"
    if { [ -e "$app" ] || [ -L "$app" ]; } && ! app_ours; then
        refusals="$refusals/mnt/SDCARD/Apps/BrickOSUninstall exists and was not written by BrickOS
"
    fi
    if [ -L "$brickos/settings" ] || { { [ -e "$updater" ] || [ -L "$updater" ]; } && ! updater_ours; }; then
        refusals="$refusals/mnt/SDCARD/BrickOS/settings/Updates is not owned by BrickOS, or settings is linked
"
    fi
    discover
    helper inputd preflight
    helper osdd preflight
    helper ledd preflight
    helper mainui preflight
    helper gameswitcher preflight
    menu_launchers
    helper tmenu preflight
    for component in $components; do sed "s/^/$component: /" "$work/$component.preflight"; done
    [ "$(val "$work/gameswitcher.preflight" app)" = foreign ] &&
        refusals="$refusals/mnt/SDCARD/Apps/GameSwitcher exists and was not written by BrickOS
"
    case "$(val "$work/gameswitcher.preflight" old_activation)" in
        changed\ *) refusals="${refusals}GameSwitcher's own activation changed: $(val "$work/gameswitcher.preflight" old_activation | cut -d ' ' -f 2-)
" ;;
    esac
    # The producers must be the official daemons, known by path and process, or BrickOS. The input
    # daemon of GameSwitcher's own earlier activation counts as BrickOS's while that activation is
    # intact, since the setup stops it first.
    for exe in $(sed -n 's/^producer pid=[0-9]* exe=//p' "$work/inputd.preflight"); do
        case "$exe" in
            "$official_inputd"|"$brickos/inputd/"*) ;;
            "$brickos/gameswitcher/runtime/"*/trimui_inputd)
                [ "$(val "$work/gameswitcher.preflight" old_activation)" = valid ] || refusals="${refusals}unknown input producer $exe
" ;;
            *) refusals="${refusals}unknown input producer $exe
" ;;
        esac
    done
    for exe in $(sed -n 's/^writer pid=[0-9]* exe=\([^ ]*\) .*/\1/p' "$work/osdd.preflight"); do
        case " $official_osdds " in *" $exe "*) continue ;; esac
        case "$exe" in "$brickos/osd/"*) ;; *) refusals="${refusals}unknown OSD writer $exe
" ;; esac
    done
    if [ "$boot" = 1 ]; then
        [ "$(val "$work/inputd.preflight" producers)" -le 1 ] 2>/dev/null || refusals="${refusals}more than one input producer
"
        [ "$(val "$work/osdd.preflight" writers)" -le 1 ] 2>/dev/null || refusals="${refusals}more than one OSD writer
"
    else
        [ "$(val "$work/inputd.preflight" producers)" = 1 ] || refusals="${refusals}expected exactly one input producer
"
        [ "$(val "$work/osdd.preflight" writers)" = 1 ] || refusals="${refusals}expected exactly one OSD writer
"
    fi
    [ "$(val "$work/inputd.preflight" official)" = present ] || refusals="${refusals}the official input daemon is missing
"
    [ "$(val "$work/osdd.preflight" official_present)" = yes ] || refusals="${refusals}the official OSD is missing
"
    [ "$(val "$work/tmenu.preflight" retroarch_present)" = yes ] || refusals="${refusals}ra64.trimui is missing
"
    [ "$(val "$work/tmenu.preflight" official_present)" = yes ] || refusals="${refusals}the official libtmenu.so is missing
"
    [ "$(val "$work/mainui.preflight" official)" = present ] || refusals="${refusals}the official MainUI is missing
"

    echo "setup_build=$build"
    echo "installed_build=$(installed_build)"
    for component in $components; do
        if [ "$(val "$work/$component.preflight" up_to_date)" = yes ]; then echo "component $component up-to-date"
        else echo "component $component install"; fi
    done
    [ -f "$setup_record" ] || for component in inputd osd ledd tmenu mainui; do
        [ -f "$brickos/$component/install-record" ] && echo "adopt=$component $(device "$brickos/$component")"
    done
    [ "$(val "$work/gameswitcher.preflight" old_activation)" = valid ] &&
        echo "migrate=gameswitcher $(device "$brickos/gameswitcher/activation-record")"
    [ "$(val "$work/gameswitcher.preflight" available)" = no ] &&
        echo "gameswitcher_unavailable=$(val "$work/gameswitcher.preflight" unavailable_reason)"
    if [ "$(val "$work/inputd.preflight" up_to_date)" != yes ]; then
        echo "write $(device "$brickos/inputd")/$(echo "$inputd_hash" | cut -c 1-12)/trimui_inputd $inputd_hash"
        echo "write $(device "$brickos/inputd")/launch.sh from templates/brickos-inputd-launch.sh $(packaged_hash templates/brickos-inputd-launch.sh)"
        echo "write $(device "$brickos/inputd")/current"
        echo "write $(device "$starts")/brickos-inputd.sh from templates/brickos-inputd-start.sh $(packaged_hash templates/brickos-inputd-start.sh)"
    fi
    if [ "$(val "$work/osdd.preflight" up_to_date)" != yes ]; then
        echo "write $(device "$brickos/osd")/$(echo "$osdd_hash" | cut -c 1-12)/trimui_osdd $osdd_hash"
        echo "write $(device "$brickos/osd")/launch.sh from templates/brickos-osdd-launch.sh $(packaged_hash templates/brickos-osdd-launch.sh)"
        echo "write $(device "$brickos/osd")/current"
        echo "write $(device "$starts")/brickos-osdd.sh from templates/brickos-osdd-start.sh $(packaged_hash templates/brickos-osdd-start.sh)"
    fi
    if [ "$(val "$work/ledd.preflight" up_to_date)" != yes ]; then
        echo "write $(device "$brickos/ledd")/$(echo "$ledd_hash" | cut -c 1-12)/brickos-ledd $ledd_hash"
        echo "write $(device "$brickos/ledd")/launch.sh from templates/brickos-ledd-launch.sh $(packaged_hash templates/brickos-ledd-launch.sh)"
        echo "write $(device "$brickos/ledd")/current"
        echo "write $(device "$starts")/brickos-ledd.sh from templates/brickos-ledd-start.sh $(packaged_hash templates/brickos-ledd-start.sh)"
    fi
    if [ "$(val "$work/mainui.preflight" up_to_date)" != yes ]; then
        echo "write $(device "$brickos/mainui")/$(echo "$mainui_hash" | cut -c 1-12)/MainUI $mainui_hash"
        echo "write $(device "$brickos/mainui")/wrapper.sh from mainui/brickos-mainui-wrapper.sh $(packaged_hash mainui/brickos-mainui-wrapper.sh)"
        echo "write $(device "$brickos/mainui")/current"
        echo "write $(device "$starts")/brickos-mainui.sh from mainui/brickos-mainui-start.sh $(packaged_hash mainui/brickos-mainui-start.sh)"
    fi
    # GameSwitcher's launchers and files, then the menu's.
    while read -r line kind path action reason; do
        case "$line $kind $action" in
            "plan runtime "*) echo "write $path/ $action" ;;
            "plan entry "*) echo "write $path $action" ;;
            "plan app "*) for file in config.json icon.png launch.sh; do echo "write $path/$file $(packaged_hash "gameswitcher/app/$file")"; done ;;
            "plan remove "*) echo "remove $path" ;;
            "plan launcher install"|"plan launcher from-menu"|"plan launcher migrate"|"plan launcher update")
                echo "launcher_change=$path gameswitcher $action"
                echo "write ${reason#backup=}" ;;
            "plan launcher current") echo "launcher_gameswitcher=$path" ;;
            "plan launcher release") echo "launcher_change=$path release" ;;
            "plan launcher left") echo "launcher_skip=$path $reason" ;;
        esac
    done < "$work/gameswitcher.preflight"
    while read -r line kind path action reason; do
        case "$line $kind $action" in
            "plan library "*|"plan current "*) echo "write $path $action" ;;
            "plan launcher install"|"plan launcher update"|"plan launcher adopt")
                echo "launcher_change=$path $action"
                echo "write ${reason#backup=}" ;;
            "plan launcher skip") echo "launcher_skip=$path $reason" ;;
            "plan remove-library "*) echo "remove $path" ;;
        esac
    done < "$work/tmenu.preflight"
    echo "$odd_launchers" | while read -r path; do [ -n "$path" ] && echo "launcher_skip=$path the path has characters the setup does not support"; done
    if needs_seed; then
        seed_plan | while IFS= read -r path; do
            echo "seed $(device "$brickos")/$path $(hash_of "$package/$path")"
        done
        [ -d "$brickos/fonts" ] || echo "folder $(device "$brickos/fonts")"
        wallpaper_plan | while read -r action name; do
            case "$action" in
                add) echo "seed $(device "$brickos/wallpapers")/$name $(hash_of "$package/wallpapers/$name")" ;;
                replace) echo "write $(device "$brickos/wallpapers")/$name $(hash_of "$package/wallpapers/$name")" ;;
                keep) echo "wallpaper_keep=$(device "$brickos/wallpapers")/$name" ;;
            esac
        done
    fi
    if needs_app; then
        for file in $(app_files); do echo "write $(device "$app")/$file $(packaged_hash "uninstall/$file")"; done
    fi
    if needs_updater; then
        for file in $(updater_files); do echo "write $(device "$updater")/$file $(packaged_hash "updater/$file")"; done
    fi
    for file in setup.sh $helper_files; do
        [ "$(hash_of "$installed_copy/$file")" = "$(hash_of "$package/$file")" ] ||
            echo "write $(device "$installed_copy")/$file $(hash_of "$package/$file")"
    done
    needs_seed && echo "write $(device "$setup_record")"
    if [ -f "$package/versions.json" ] && [ "$(hash_of "$versions_file")" != "$(hash_of "$package/versions.json")" ]; then
        echo "write $(device "$versions_file") $(hash_of "$package/versions.json")"
    fi
    if [ "${package%/*}" = "$brickos/package" ]; then
        for file in "$brickos"/package/*/manifest.txt; do
            [ -f "$file" ] && [ "${file%/manifest.txt}" != "$package" ] && echo "remove $(device "${file%/manifest.txt}")"
        done
    fi
}

# One fingerprint of everything the plan depends on.
fingerprint() {
    {
        for component in $components; do echo "$component $(val "$work/$component.preflight" fingerprint)"; done
        echo "boot_script $(hash_of "$boot_script")"
        echo "packages $(ls "$brickos/package" 2>/dev/null | tr '\n' ' ')"
        echo "setup_record $(hash_of "$setup_record")"
        echo "component_versions $(hash_of "$versions_file")"
        echo "app $(app_state)"
        for file in $(updater_files); do echo "updater $file $(hash_of "$updater/$file")"; done
        echo "odd_launchers $odd_launchers"
        needs_seed && seed_plan
        needs_seed && wallpaper_plan
    } | digest
}

up_to_date() {
    needs_updater && return 1
    if [ -f "$package/versions.json" ]; then
        [ "$(hash_of "$versions_file")" = "$(hash_of "$package/versions.json")" ] || return 1
    fi
    [ "$(installed_build)" = "$build" ] && for component in $components; do
        [ "$(val "$work/$component.preflight" up_to_date)" = yes ] || return 1
    done
}

preflight() {
    assess
    [ -n "$refusals" ] && echo "$refusals" | while IFS= read -r reason; do [ -n "$reason" ] && echo "refuse=$reason"; done
    [ -n "${launchers+set}" ] && echo "fingerprint=$(fingerprint)"
    if [ -n "$refusals" ]; then echo "result=refused"; exit 3; fi
    if up_to_date; then echo "up_to_date=yes"; echo "result=up-to-date"; else echo "up_to_date=no"; echo "result=ready"; fi
}

# Keeps a copy of every file and folder an install may touch, and the folders it may create.
snapshot() {
    local n=0 path
    : > "$work/seeded"
    : > "$work/seeded-sets"
    : > "$work/replaced"
    mkdir "$work/undo" && : > "$work/undo/list" || return 1
    # GameSwitcher's files only, never its store beside them; every launcher it scans, with the
    # earlier activation's backups beside them.
    for path in "$starts/brickos-inputd.sh" "$starts/brickos-osdd.sh" "$starts/brickos-ledd.sh" \
        "$starts/brickos-mainui.sh" "$starts/brickos-mainui-cleanup.sh" "$starts/00-brickos-gameswitcher-input.sh" \
        $(for path in "$sd"/Emus/*/launch*.sh "$sd"/Emus/*/launch*.sh.brickos-gameswitcher-original; do
            [ -e "$path" ] && echo "$path"; done) \
        $(for path in $launchers; do echo "$root$path"; done) \
        "$brickos/inputd" "$brickos/osd" "$brickos/ledd" "$brickos/tmenu" "$brickos/mainui" \
        "$brickos/gameswitcher/runtime" "$brickos/gameswitcher/entry.sh" "$brickos/gameswitcher/install-record" \
        "$brickos/gameswitcher/launchers" "$brickos/gameswitcher/activation-record" "$brickos/gameswitcher/DISABLED" \
        "$installed_copy" "$setup_record" "$versions_file" "$app" "$sd/Apps/GameSwitcher" "$updater"; do
        n=$((n + 1))
        if [ -e "$path" ]; then
            cp -pR "$path" "$work/undo/$n" || return 1
            echo "$n present $path"
        else
            echo "$n absent $path"
        fi >> "$work/undo/list"
    done
    for path in "$brickos" "$starts" "$brickos/bootlogos" "$brickos/splashscreens" "$brickos/icons" "$brickos/icons/settings" \
        "$brickos/loading-screens" "$brickos/fonts" "$brickos/wallpapers" "$sd/Apps" "$brickos/gameswitcher" "$brickos/settings"; do
        [ -d "$path" ] || echo "0 folder $path" >> "$work/undo/list"
    done
}

# Puts back every file and folder from the snapshot, removes the files and preset folders
# seeded by this run, and removes the folders it created.
restore() {
    local n kind path
    [ -f "$work/undo/list" ] || return 0
    while read -r n kind path; do
        case "$kind" in
            present) rm -rf "$path" && cp -pR "$work/undo/$n" "$path" ;;
            absent) rm -rf "$path" ;;
        esac
    done < "$work/undo/list"
    while IFS= read -r path; do rm -f "$path" "$path.brickos-tmp"; done < "$work/seeded"
    # Only preset folders this run created are listed, so rm -rf never reaches the user's own.
    [ -f "$work/seeded-sets" ] && while IFS= read -r path; do rm -rf "$path"; done < "$work/seeded-sets"
    [ -f "$work/replaced" ] && while IFS= read -r name; do
        cp -p "$work/undo/wallpapers/$name" "$brickos/wallpapers/$name"
        rm -f "$brickos/wallpapers/$name.brickos-tmp"
    done < "$work/replaced"
    rm -rf "$app.brickos-tmp" "$app.brickos-old" "$sd/Apps/GameSwitcher.brickos-tmp" "$sd/Apps/GameSwitcher.brickos-old"
    sed -n 's/^0 folder //p' "$work/undo/list" | sort -r | while IFS= read -r path; do rmdir "$path" 2>/dev/null; done
    sync
}

# Ends a run whose step failed after changes were made: everything is put back first.
failed() {
    local step=$1
    shift
    restore
    say "result=failed"
    say "failed_step=$step"
    say "reason=$*"
    say "restored=yes"
    log_run
    exit 3
}

seed() {
    local path source target preset
    while IFS= read -r path; do
        source=$package/$path; target=$brickos/$path
        case "$path" in
            loading-screens/*/*)
                preset=${path#loading-screens/}
                preset=$brickos/loading-screens/${preset%%/*}
                if [ ! -d "$preset" ]; then
                    echo "$preset" >> "$work/seeded-sets"
                    mkdir -p "$preset" || return 1
                fi ;;
        esac
        mkdir -p "${target%/*}" || return 1
        echo "$target" >> "$work/seeded"
        cp "$source" "$target.brickos-tmp" && [ "$(hash_of "$target.brickos-tmp")" = "$(hash_of "$source")" ] &&
            mv "$target.brickos-tmp" "$target" || return 1
    done < "$work/seed-plan"
}

# Adds the absent wallpapers and replaces the earlier BrickOS versions, keeping a copy of each
# replaced file for restore; the user's own stay.
install_wallpapers() {
    local action name source target
    mkdir -p "$work/undo/wallpapers" || return 1
    while read -r action name; do
        source=$package/wallpapers/$name; target=$brickos/wallpapers/$name
        case "$action" in
            add) mkdir -p "$brickos/wallpapers" && echo "$target" >> "$work/seeded" || return 1 ;;
            replace) cp -p "$target" "$work/undo/wallpapers/$name" && echo "$name" >> "$work/replaced" || return 1 ;;
            *) continue ;;
        esac
        cp "$source" "$target.brickos-tmp" && [ "$(hash_of "$target.brickos-tmp")" = "$(hash_of "$source")" ] &&
            mv "$target.brickos-tmp" "$target" || return 1
    done < "$work/wallpaper-plan"
}

# Runs one component helper's install with the fingerprint of its own fresh preflight, which also
# tells whether the component is up to date after the steps before it.
step() {
    local component=$1 result
    [ "$component" = tmenu ] && { launchers=$discovered; menu_launchers; }
    helper "$component" preflight
    if [ "$(val "$work/$component.preflight" up_to_date)" = yes ]; then
        say "$component=up-to-date"
        return 0
    fi
    [ "$inject_failure" = "$component" ] && failed "$component" "simulated failure in $component"
    helper "$component" install "$(val "$work/$component.preflight" fingerprint)"
    result=$(val "$work/$component.install" result)
    sed "s/^/$component: /" "$work/$component.install"
    case "$result" in
        installed|up-to-date) ;;
        *) failed "$component" "$(val "$work/$component.install" reason)" ;;
    esac
    if [ "$component" = gameswitcher ] && [ "$(val "$work/$component.install" available)" = no ]; then
        say "gameswitcher=unavailable"
        say "gameswitcher_reason=$(val "$work/$component.install" reason)"
    elif [ -n "$2" ]; then say "$component=updated"; else say "$component=installed"; fi
}

# Stages the package's app as a whole folder beside the app, then swaps it in. A failure leaves
# the snapshot to put back the previous folder, or none.
install_app() {
    local file stage=$app.brickos-tmp old=$app.brickos-old
    rm -rf "$stage" "$old"
    mkdir -p "$stage" || return 1
    for file in $(app_files); do
        cp "$package/uninstall/$file" "$stage/$file" &&
            [ "$(hash_of "$stage/$file")" = "$(packaged_hash "uninstall/$file")" ] || return 1
    done
    if [ -e "$app" ] || [ -L "$app" ]; then mv "$app" "$old" || return 1; fi
    mv "$stage" "$app" || return 1
    rm -rf "$old"
}

install_copy() {
    local file
    mkdir -p "$installed_copy/helpers" "$installed_copy/mainui" "$installed_copy/gameswitcher" || return 1
    for file in setup.sh $helper_files; do
        [ "$(hash_of "$installed_copy/$file")" = "$(hash_of "$package/$file")" ] && continue
        cp "$package/$file" "$installed_copy/$file.brickos-tmp" && mv "$installed_copy/$file.brickos-tmp" "$installed_copy/$file" ||
            return 1
    done
}

install_updater() {
    local file stage=$updater.brickos-tmp old=$updater.brickos-old
    needs_updater || return 0
    rm -rf "$stage" "$old"
    mkdir -p "$stage" || return 1
    for file in $(updater_files); do
        cp "$package/updater/$file" "$stage/$file" &&
            [ "$(hash_of "$stage/$file")" = "$(packaged_hash "updater/$file")" ] || return 1
    done
    [ ! -e "$updater" ] || mv "$updater" "$old" || return 1
    mv "$stage" "$updater" || return 1
    rm -rf "$old"
}

do_install() {
    local path previous count app_action
    assess > "$work/assess"
    cat "$work/assess"
    if [ -n "$refusals" ]; then
        set --
        while IFS= read -r path; do [ -n "$path" ] && set -- "$@" "$path"; done <<EOF
$refusals
EOF
        refused refused "$@"
    fi
    sed -n 's/^launcher_skip=/launcher_skipped=/p' "$work/assess" >> "$work/summary"
    if up_to_date; then
        for component in $components; do say "$component=up-to-date"; done
        say "result=up-to-date"
        log_run
        exit 0
    fi
    previous=$(installed_build)
    snapshot || failed snapshot "cannot keep a copy of the files this install changes"
    if needs_seed; then
        seed_plan > "$work/seed-plan"
        seed || failed seed "cannot add the boot visuals, loading-screen presets and Settings icons"
        count=$(wc -l < "$work/seed-plan")
        say "seeded=$((count))"
        # The folder BrickOS's Appearance reads fonts from: created once, then the user's.
        if [ ! -d "$brickos/fonts" ]; then
            mkdir -p "$brickos/fonts" || failed seed "cannot create $(device "$brickos/fonts")"
            say "created=$(device "$brickos/fonts")"
        fi
        wallpaper_plan > "$work/wallpaper-plan"
        install_wallpapers || failed seed "cannot install the wallpapers"
        say "wallpapers_added=$(grep -c '^add ' "$work/wallpaper-plan")"
        say "wallpapers_replaced=$(grep -c '^replace ' "$work/wallpaper-plan")"
        while read -r action name; do
            [ "$action" = keep ] && say "wallpaper_kept=$(device "$brickos/wallpapers")/$name"
        done < "$work/wallpaper-plan"
    fi
    [ "$inject_failure" = seed ] && failed seed "simulated failure after seeding"
    if needs_app; then
        if [ -e "$app" ]; then app_action=updated; else app_action=installed; fi
        install_app || failed app "cannot install $(device "$app")"
        say "app=$app_action"
    elif needs_seed && [ -n "$(app_files)" ]; then
        say "app=up-to-date"
    fi
    [ "$inject_failure" = app ] && failed app "simulated failure after the app"
    # GameSwitcher's own earlier activation goes first: its input daemon must stop before
    # BrickOS's installs, and MainUI's files must be back before MainUI's step.
    if [ "$(val "$work/gameswitcher.preflight" old_activation)" = valid ]; then
        [ "$inject_failure" = migrate ] && failed gameswitcher "simulated failure in the migration"
        helper gameswitcher preflight
        helper gameswitcher migrate "$(val "$work/gameswitcher.preflight" fingerprint)"
        sed "s/^/gameswitcher: /" "$work/gameswitcher.migrate"
        [ "$(val "$work/gameswitcher.migrate" result)" = migrated ] ||
            failed gameswitcher "$(val "$work/gameswitcher.migrate" reason)"
        say "gameswitcher=migrated"
    fi
    step inputd "$([ -f "$brickos/inputd/current" ] && echo update)"
    [ "$inject_failure" = inputd-done ] && failed inputd "simulated failure after inputd"
    step osdd "$([ -f "$brickos/osd/current" ] && echo update)"
    [ "$inject_failure" = osdd-done ] && failed osdd "simulated failure after osdd"
    step ledd "$([ -f "$brickos/ledd/current" ] && echo update)"
    [ "$inject_failure" = ledd-done ] && failed ledd "simulated failure after ledd"
    step mainui "$([ -f "$brickos/mainui/current" ] && echo update)"
    [ "$inject_failure" = mainui-done ] && failed mainui "simulated failure after mainui"
    step gameswitcher "$([ -f "$brickos/gameswitcher/install-record" ] && echo update)"
    [ "$inject_failure" = gameswitcher-done ] && failed gameswitcher "simulated failure after gameswitcher"
    step tmenu "$([ -d "$brickos/tmenu" ] && echo update)"
    sed -n 's/^changed=/launcher_gameswitcher=/p; s/^released=/launcher_released=/p' "$work/gameswitcher.install" 2>/dev/null >> "$work/summary"
    sed -n 's/^changed=/launcher_changed=/p' "$work/tmenu.install" 2>/dev/null >> "$work/summary"
    install_updater || {
        rm -rf "$updater.brickos-tmp" "$updater.brickos-old"
        failed updater "cannot install the Updates settings"
    }
    install_copy || failed finish "cannot write $(device "$installed_copy")"
    if [ -f "$package/versions.json" ]; then
        cp "$package/versions.json" "$versions_file.brickos-tmp" && mv "$versions_file.brickos-tmp" "$versions_file" ||
            failed finish "cannot write $(device "$versions_file")"
    fi
    if [ "$previous" != "$build" ]; then
        printf 'build=%s\n' "$build" > "$setup_record.brickos-tmp" && mv "$setup_record.brickos-tmp" "$setup_record" ||
            failed finish "cannot write $(device "$setup_record")"
    fi
    [ "$inject_failure" = finish ] && failed finish "simulated failure before the end"
    sync
    # Older packages are no longer needed once this build is installed.
    if [ "${package%/*}" = "$brickos/package" ]; then
        for path in "$brickos"/package/*/manifest.txt; do
            [ -f "$path" ] && [ "${path%/manifest.txt}" != "$package" ] || continue
            rm -rf "${path%/manifest.txt}" && say "removed_package=$(device "${path%/manifest.txt}")"
        done
    fi
    sync
    if [ -z "$previous" ]; then say "result=installed"; else say "result=updated"; fi
    say "previous_build=${previous:-none}"
    log_run
}

do_rollback() {
    local component result failures='' exe
    [ -n "$(pidof ra64.trimui 2>/dev/null)" ] && refused refused "a game is running"
    discover
    inputd_hash=''; osdd_hash=''; ledd_hash=''; tmenu_hash=''; mainui_hash=''
    # The menu, GameSwitcher, MainUI, the LED daemon, the OSD, then the input daemon: the reverse
    # of the install. GameSwitcher's own earlier activation is undone with GameSwitcher, before
    # MainUI, since it restores MainUI's files.
    for component in tmenu gameswitcher mainui ledd osdd inputd; do
        case "$component" in
            tmenu) [ -d "$brickos/tmenu" ] || grep -q '# BrickOS in-game menu$' $(for path in $launchers; do echo "$root$path"; done) /dev/null 2>/dev/null ;;
            gameswitcher)
                [ -e "$brickos/gameswitcher/install-record" ] || [ -e "$brickos/gameswitcher/runtime" ] ||
                    [ -e "$brickos/gameswitcher/entry.sh" ] || [ -e "$brickos/gameswitcher/activation-record" ] ||
                    [ -n "$(ls "$brickos/gameswitcher/launchers" 2>/dev/null)" ] ||
                    grep -qx '# BrickOS GameSwitcher app' "$sd/Apps/GameSwitcher/launch.sh" 2>/dev/null ;;
            mainui) [ -e "$starts/brickos-mainui.sh" ] || [ -f "$brickos/mainui/current" ] || [ -f "$brickos/mainui/install-record" ] ;;
            ledd) [ -e "$starts/brickos-ledd.sh" ] || [ -f "$brickos/ledd/current" ] || [ -f "$brickos/ledd/install-record" ] ;;
            osdd) [ -e "$starts/brickos-osdd.sh" ] || [ -f "$brickos/osd/current" ] || [ -f "$brickos/osd/install-record" ] ;;
            inputd) [ -e "$starts/brickos-inputd.sh" ] || [ -f "$brickos/inputd/current" ] || [ -f "$brickos/inputd/install-record" ] ;;
        esac || { say "$component=absent"; continue; }
        helper "$component" rollback
        sed "s/^/$component: /" "$work/$component.rollback"
        result=$(val "$work/$component.rollback" result)
        case "$result" in
            rolled-back|nothing-installed) say "$component=rolled-back" ;;
            *) say "$component=failed"; failures="$failures $component: $(val "$work/$component.rollback" reason);" ;;
        esac
    done
    sed -n 's/^restored=/launcher_restored=/p; s/^stripped=/launcher_restored=/p; s/^left=/launcher_left=/p' \
        "$work/tmenu.rollback" "$work/gameswitcher.rollback" 2>/dev/null >> "$work/summary"
    # MainUI's files stay while its frontend runs; a one-shot start script removes them at the
    # next boot, which runs the official frontend.
    grep -qx 'cleanup=next-boot' "$work/mainui.rollback" 2>/dev/null && say "frontend=official-at-next-boot"
    if [ -n "$failures" ]; then
        say "result=rollback-incomplete"
        say "reason=$failures"
        log_run
        exit 3
    fi
    # Every component has rolled back, so the uninstall app goes too, if BrickOS wrote it.
    if [ -e "$app" ] || [ -L "$app" ]; then
        if app_ours; then
            rm -rf "$app" "$app.brickos-tmp" "$app.brickos-old"
            if [ -e "$app" ]; then
                say "result=rollback-incomplete"
                say "reason=cannot remove $(device "$app")"
                log_run
                exit 3
            fi
            say "app=removed"
        else
            say "app_left=$(device "$app") was not written by BrickOS"
        fi
    fi
    if updater_ours; then
        rm -rf "$updater" "$updater.brickos-tmp" "$updater.brickos-old"
        rm -f "$brickos/updates/pending"
    fi
    rm -f "$setup_record" "$versions_file"
    rm -rf "$installed_copy"
    sync
    say "result=rolled-back"
    log_run
}

case "$command" in
    preflight) preflight ;;
    install) do_install ;;
    rollback) do_rollback ;;
esac
