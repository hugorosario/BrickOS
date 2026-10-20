#!/bin/sh
# Device-side helper for GameSwitcher, the release package's `gameswitcher` component.
#
#   gameswitcher-setup.sh preflight CONFIG   Report the state an install depends on, its
#                                            fingerprint, and the plan. Changes nothing.
#   gameswitcher-setup.sh migrate CONFIG     Undo an activation made by GameSwitcher's own earlier
#                                            installer if the state still has the fingerprint of
#                                            the dry run.
#   gameswitcher-setup.sh install CONFIG     Install or update GameSwitcher if the state still
#                                            has the fingerprint of the dry run.
#   gameswitcher-setup.sh rollback CONFIG    Put back every launcher GameSwitcher took and remove
#                                            its files, keeping the player's data.
#
# Its files sit in /mnt/SDCARD/BrickOS/gameswitcher/ beside GameSwitcher's store (the index
# pointers, checkpoints, thumbnails, settings and play time), which it never writes or removes:
#   runtime/<build>/   GameSwitcher, gs-session, libtmenu.so and MANIFEST.sha256; <build> is the
#                      first 12 hex digits of the manifest's SHA-256
#   runtime/current    "<build> <manifest SHA-256>". It lives under runtime/ because on the FAT
#                      card a pointer beside the store's CURRENT would be the same file.
#   entry.sh           the entry for the launchers, the app and MainUI's boot hook
#   install-record     the folders the install created
#   launchers/<path>.record and .backup   one pair per launcher GameSwitcher took; the backup
#                      holds the launcher's stock bytes
# and the app in /mnt/SDCARD/Apps/GameSwitcher/, whose launch.sh carries the app's marker line,
# and Settings > GameSwitcher in /mnt/SDCARD/BrickOS/settings/GameSwitcher/, whose menu.sh carries
# its marker line, with systems/systems.json written from the launchers GameSwitcher keeps. Its
# rows keep the player's choices in BrickOS/gameswitcher/OFF, excluded-systems and settings.json,
# which this helper never writes or removes, so they survive installs and updates.
#
# GameSwitcher takes each /mnt/SDCARD/Emus/*/launch*.sh whose stock form runs one compatible
# RetroArch command and nothing it does not know, and which gs-session's launch check accepts:
# in the stock form, or in BrickOS's menu form when the menu's record and backup match, in which
# case the menu's backup becomes GameSwitcher's and the menu's record goes. The checks test
# behaviour and paths: the session library loads with its exports (gs-session --check-runtime),
# and the RetroArch executable and the core are AArch64 programs at their paths. No stock file is
# compared with a hash. When the session library fails its check, GameSwitcher is unavailable:
# nothing of it stays active, the launchers it held get their stock bytes back, and the menu
# takes them. A launcher changed by hand after the install is left alone and reported.
#
# An activation by GameSwitcher's own earlier installer (BrickOS/gameswitcher/activation-record)
# is verified as that installer did, and migrate undoes it: its launchers and MainUI's wrapper.sh
# and current get their recorded bytes back, its input start script, app and runtimes go, and its
# input daemon stops, so that BrickOS's input daemon serves game POWER. A recorded file that
# changed refuses before anything is written, naming the file. An install or rollback that finds
# such an activation undoes it first.
#
# CONFIG is a shell fragment; every path is below `root`, which is empty on the device. `payload`
# is the package's gameswitcher/ folder; a rollback needs none. Files are staged under a temporary
# name and renamed into place.
set -u

command=${1:-}
config=${2:-}
[ -n "$command" ] && [ -f "$config" ] ||
    { echo "usage: gameswitcher-setup.sh preflight|migrate|install|rollback CONFIG" >&2; exit 2; }
root=''
payload=''
expected_fingerprint=''
boot=0
stop_timeout_s=10
. "$config"

sd=$root/mnt/SDCARD
# The same places as the launchers and the entry script see them on the device.
dsd=/mnt/SDCARD
parent=$sd/BrickOS
base=$parent/gameswitcher
dbase=$dsd/BrickOS/gameswitcher
runtimes=$base/runtime
record=$base/install-record
records=$base/launchers
app=$sd/Apps/GameSwitcher
tile=$parent/settings/GameSwitcher
tmenu_records=$parent/tmenu/launchers
old_record=$base/activation-record
old_run=$root/tmp/brickos-gameswitcher-input
marker='# BrickOS GameSwitcher launcher'
app_marker='# BrickOS GameSwitcher app'
runtime_files='GameSwitcher gs-session libtmenu.so'
app_files='config.json icon.png launch.sh'
tile_marker='# BrickOS GameSwitcher settings'
tile_files='GameSwitcher.json icon.png lang/pt.json menu.sh'
# Everything the folder holds: the package's files and systems/systems.json, which the install
# writes from the launchers GameSwitcher keeps (systems_menu).
tile_tree='./GameSwitcher.json ./icon.png ./lang ./lang/pt.json ./menu.sh ./systems ./systems/systems.json '

hash_of() { if [ -f "$1" ]; then sha256sum "$1" | cut -d ' ' -f 1; else echo absent; fi; }
digest() { sha256sum | cut -d ' ' -f 1; }
dev() { echo "${1#"$root"}"; }
fail() { echo "result=refused"; echo "reason=$*"; exit 3; }
name_of() { echo "$1" | tr '/' '_'; }
field() { sed -n "s/^$2=//p" "$1" 2>/dev/null | tail -n 1; }
running() { [ -n "$(pidof ra64.trimui gs-session GameSwitcher 2>/dev/null)" ]; }

# The runtime's manifest as the install writes it, in a fixed order, and its identity.
payload_manifest() { local file; for file in $runtime_files; do echo "$(hash_of "$payload/$file")  $file"; done; }
manifest_hash=''
build=''
if [ -n "$payload" ]; then
    manifest_hash=$(payload_manifest | digest)
    build=$(echo "$manifest_hash" | cut -c 1-12)
fi
payload_complete() {
    local file
    for file in $runtime_files entry.sh; do [ -f "$payload/$file" ] || return 1; done
    for file in $app_files; do [ -f "$payload/app/$file" ] || return 1; done
    for file in $tile_files; do [ -f "$payload/settings/$file" ] || return 1; done
    grep -qx "$app_marker" "$payload/app/launch.sh" && grep -qx "$tile_marker" "$payload/settings/menu.sh"
}

# The core of a launcher's one RetroArch command, when the launcher holds nothing else that
# GameSwitcher does not know; nothing otherwise. $2 is the launcher's folder on the device. An
# echo of plain text (no $, backquote, backslash, redirection, pipe, ; & or parenthesis) only
# writes to the launch log, and a call to the folder's performance.sh does nothing when the
# script is not there; the command may leave out $NET_PARAM.
core_of() {
    local performance=absent
    { [ -e "$root$2/performance.sh" ] || [ -L "$root$2/performance.sh" ]; } && performance=present
    awk -v ra="$dsd/RetroArch" -v emu="$2" -v performance="$performance" '
        {sub(/\r$/," "); sub(/^[ \t]+/,""); sub(/[ \t]+$/,"")}
        /^#/ || /^$/ {next}
        $0=="RA_DIR="ra {r=1; next} $0=="EMU_DIR="emu {e=1; next}
        $0=="cd $RA_DIR/" || $0=="$EMU_DIR/cpufreq.sh" || $0=="$EMU_DIR/cpuswitch.sh" || $0=="NET_PARAM=" {next}
        $0=="$EMU_DIR/performance.sh" && performance=="absent" {next}
        $0=="echo $0 $*" || /^echo \$RA_DIR\/\.retroarch\/cores\/[A-Za-z0-9_+-]+\.so$/ {next}
        /^echo( [^$`\\<>|;&()]*)?$/ {next}
        /^(LD_LIBRARY_PATH=\$LD_LIBRARY_PATH )?HOME=\$RA_DIR\/ \$RA_DIR\/ra64.trimui -v (\$NET_PARAM )?-L \$RA_DIR\/\.retroarch\/cores\/[A-Za-z0-9_+-]+\.so "\$\*"$/ {
            n++; c=$0; sub(/.*cores\//,"",c); sub(/ ".*/,"",c); next
        }
        {bad=1} END {if(!bad && r && e && n==1) print c}' "$1" 2>/dev/null
}

# Single-quoted, so no path in the launcher is ever a shell command.
quote() { printf "'%s'" "$(printf '%s' "$1" | sed "s/'/'\\\\''/g")"; }
# The launcher GameSwitcher writes for a launcher path and its core.
wrapper() {
    echo '#!/bin/sh'
    echo "$marker"
    printf 'exec sh '; quote "$dbase/entry.sh"; printf ' launch '; quote "$1"
    printf ' "$*" '; quote "$dsd/RetroArch/.retroarch/cores/$2"
    printf ' '; quote "$dbase/launchers/$(name_of "$1").backup"; printf '\n'
}

# GameSwitcher's own earlier activation: the files its installer may have written.
old_safe() {
    case "$1" in
        "$dsd"/Emus/*/launch*.sh|"$dsd"/Apps/GameSwitcher/config.json|"$dsd"/Apps/GameSwitcher/icon.png|\
        "$dsd"/Apps/GameSwitcher/launch.sh|"$dsd"/BrickOS/mainui/wrapper.sh|"$dsd"/BrickOS/mainui/current|\
        "$dbase"/entry.sh|"$dsd"/System/starts/00-brickos-gameswitcher-input.sh) ;;
        *) return 1 ;;
    esac
    case "$1" in *'|'*|*..*) return 1 ;; esac
    [ ! -L "$root$1" ]
}
# Prints nothing when the activation verifies as its installer recorded it; otherwise the first
# recorded file that changed.
old_problem() {
    local number before after target current launcher backup expected
    [ -f "$old_record/files" ] && [ -f "$old_record/launchers" ] || { dev "$old_record"; return; }
    while IFS='|' read -r number before after target; do
        case "$number" in ''|*[!0-9]*) echo "$(dev "$old_record")/files"; return ;; esac
        old_safe "$target" || { echo "$target"; return; }
        current=$(hash_of "$root$target")
        [ "$current" = "$after" ] || [ "$current" = "$before" ] || { echo "$target"; return; }
        [ "$before" = absent ] || [ "$(hash_of "$old_record/$number.original")" = "$before" ] ||
            { echo "$(dev "$old_record")/$number.original"; return; }
    done < "$old_record/files"
    while IFS='|' read -r launcher backup expected; do
        old_safe "$launcher" && [ "$backup" = "$launcher.brickos-gameswitcher-original" ] && [ ! -L "$root$backup" ] ||
            { echo "$launcher"; return; }
        [ ! -e "$root$backup" ] || [ "$(hash_of "$root$backup")" = "$expected" ] || { echo "$backup"; return; }
    done < "$old_record/launchers"
}
# The stock bytes the earlier activation kept for a launcher it still holds, if any.
old_original() {
    local number before after target
    [ -f "$old_record/files" ] || return 0
    while IFS='|' read -r number before after target; do
        [ "$target" = "$1" ] && [ "$before" != absent ] && [ "$(hash_of "$root$target")" = "$after" ] &&
            echo "$old_record/$number.original"
    done < "$old_record/files"
}

# The launchers GameSwitcher scans, and those it recorded. A path the setup cannot take as a
# plain word is left to the setup's report.
known() {
    local file
    {
        for file in "$sd"/Emus/*/launch*.sh; do
            [ -f "$file" ] || continue
            case "${file#"$root"}" in *[!A-Za-z0-9_./-]*) continue ;; esac
            echo "${file#"$root"}"
        done
        for file in "$records"/*.record; do [ -f "$file" ] && field "$file" launcher; done
    } | awk 'NF && !seen[$0]++'
}

# One row per launcher, "path|hash|form|state|core|reason". Forms: stock, menu (BrickOS's menu
# line with a matching menu record), old (GameSwitcher's earlier activation), gameswitcher (this
# helper's launcher with a matching record and backup), changed (it differs from what this helper
# wrote), and other. States: install, from-menu and migrate (GameSwitcher takes it), current,
# update (GameSwitcher keeps it, rewriting it when its form changed), release (GameSwitcher gives
# it back), left, pass (it has a RetroArch command, but GameSwitcher does not take it), and none.
classify() {
    local path=$1 target=$root$1 name hash form=other source='' core='' state=none reason='' rec trec tbackup
    name=$(name_of "$path")
    hash=$(hash_of "$target")
    rec=$records/$name.record
    trec=$tmenu_records/$name.record
    tbackup=$tmenu_records/$name.backup
    if [ -f "$rec" ]; then
        if [ "$hash" = "$(field "$rec" installed_hash)" ] &&
            [ "$(hash_of "$records/$name.backup")" = "$(field "$rec" original_hash)" ]; then
            form=gameswitcher; source=$records/$name.backup
        elif [ "$hash" = "$(field "$rec" original_hash)" ]; then
            form=stock; source=$target
        else
            form=changed
        fi
    elif [ -n "$(old_original "$path")" ]; then
        form=old; source=$(old_original "$path")
    elif [ -f "$trec" ] && [ "$hash" = "$(field "$trec" installed_hash)" ] &&
        [ "$(hash_of "$tbackup")" = "$(field "$trec" original_hash)" ]; then
        form=menu; source=$tbackup
    elif [ "$hash" != absent ]; then
        form=stock; source=$target
    fi
    [ -n "$source" ] && core=$(core_of "$source" "${path%/*}")
    if [ "$form" = changed ]; then
        state=left
        if [ "$hash" = absent ]; then reason="the launcher is missing"; else reason="the launcher or its backup changed after the install"; fi
    elif [ -z "$core" ]; then
        if [ "$form" = gameswitcher ]; then state=release; reason="its stock form no longer has one compatible RetroArch command"; fi
    elif [ "$available" != yes ]; then
        if [ "$form" = gameswitcher ]; then state=release; else state=pass; fi
        reason="GameSwitcher is unavailable: $unavailable_reason"
    elif [ -z "$sample" ]; then
        if [ "$form" = gameswitcher ]; then state=release; else state=pass; fi
        reason="no ROM to check the launch with"
    elif ! "$payload/gs-session" --sd "$sd" --check-launch "$target" "$sample" "$sd/RetroArch/.retroarch/cores/$core" >/dev/null 2>&1; then
        if [ "$form" = gameswitcher ]; then state=release; else state=pass; fi
        reason="the launch check refused $core"
    else
        case "$form" in
            stock) state=install ;;
            menu) state=from-menu ;;
            old) state=migrate ;;
            gameswitcher)
                if [ "$(wrapper "$path" "$core" | digest)" = "$hash" ]; then state=current; else state=update; fi ;;
        esac
    fi
    echo "$path|$hash|$form|$state|$core|$reason"
}
classify_all() {
    local path
    sample=$(find "$sd/Roms" -type f 2>/dev/null | head -n 1)
    available=no; unavailable_reason=''
    if [ -z "$payload" ]; then unavailable_reason="no package"
    elif ! payload_complete; then unavailable_reason="the package's gameswitcher/ folder is incomplete"
    elif [ ! -x "$payload/gs-session" ]; then unavailable_reason="the package's gs-session cannot run where the package is"
    elif ! "$payload/gs-session" --check-runtime >/dev/null 2>&1; then
        unavailable_reason="the session library failed its check"
    else available=yes; fi
    table=$(for path in $(known); do classify "$path"; done)
}

# The app folder: absent, brickos (its launch.sh has the marker), old (the earlier activation's,
# which migrate removes), or foreign.
app_kind() {
    if [ ! -e "$app" ] && [ ! -L "$app" ]; then echo absent
    elif [ -d "$app" ] && [ ! -L "$app" ] && grep -qx "$app_marker" "$app/launch.sh" 2>/dev/null; then echo brickos
    elif [ -d "$old_record" ] && [ -z "$(old_problem)" ] &&
        grep -q "|$dsd/Apps/GameSwitcher/launch.sh\$" "$old_record/files" 2>/dev/null; then echo old
    else echo foreign; fi
}
# 0 when the app folder holds exactly the package's app files, with their bytes.
app_current() {
    local file
    [ "$(app_kind)" = brickos ] || return 1
    [ "$(ls -A "$app" | LC_ALL=C sort | tr '\n' ' ')" = "$app_files " ] || return 1
    for file in $app_files; do [ "$(hash_of "$app/$file")" = "$(hash_of "$payload/app/$file")" ] || return 1; done
}
# The Settings folder: absent, brickos (its menu.sh has the marker), or foreign.
tile_kind() {
    if [ -L "$parent/settings" ]; then echo foreign
    elif [ ! -e "$tile" ] && [ ! -L "$tile" ]; then echo absent
    elif [ -d "$tile" ] && [ ! -L "$tile" ] && grep -qx "$tile_marker" "$tile/menu.sh" 2>/dev/null; then echo brickos
    else echo foreign; fi
}
# 0 when the Settings folder holds exactly the package's files, with their bytes.
tile_current() {
    local file
    [ "$(tile_kind)" = brickos ] || return 1
    [ "$(cd "$tile" && find . -mindepth 1 | LC_ALL=C sort | tr '\n' ' ')" = "$tile_tree" ] || return 1
    for file in $tile_files; do [ "$(hash_of "$tile/$file")" = "$(hash_of "$payload/settings/$file")" ] || return 1; done
    [ "$(hash_of "$tile/systems/systems.json")" = "$(systems_menu | digest)" ]
}
# The Emus folders of the launchers GameSwitcher keeps, which Settings > GameSwitcher > Systems
# lists for the player to exclude.
taken_systems() {
    local path hash form st core reason system
    echo "$table" | while IFS='|' read -r path hash form st core reason; do
        case "$st" in install|from-menu|migrate|update|current) ;; *) continue ;; esac
        system=${path%/*}; system=${system##*/}
        case "$system" in ''|*[!A-Za-z0-9._-]*) continue ;; esac
        echo "$system"
    done | LC_ALL=C sort -u
}
# The Systems menu: one toggle per system, on while the system uses GameSwitcher.
systems_menu() {
    local system first=yes
    printf '{\n  "menu": [\n'
    for system in $(taken_systems); do
        [ "$first" = yes ] || printf ',\n'
        first=no
        printf '    { "label": "%s", "type": "toggle", "selected": 1, "load": "sh ../menu.sh system-status %s", "execute": "sh ../menu.sh system %s", "description": "Off: GUIDE in games of this system opens the in-game menu" }' \
            "$system" "$system" "$system"
    done
    [ "$first" = no ] || printf '    { "label": "No systems", "type": "info", "value": "" }'
    printf '\n  ]\n}\n'
}
# 0 when runtime/current names a runtime folder whose files match its manifest.
runtime_valid() {
    local id expected file
    read -r id expected < "$runtimes/current" 2>/dev/null || return 1
    [ "$id" = "$1" ] && [ "$expected" = "$2" ] || return 1
    [ "$(hash_of "$runtimes/$id/MANIFEST.sha256")" = "$expected" ] || return 1
    for file in $runtime_files; do [ -f "$runtimes/$id/$file" ] && [ ! -L "$runtimes/$id/$file" ] || return 1; done
    (cd "$runtimes/$id" && sha256sum -c MANIFEST.sha256 >/dev/null 2>&1)
}

state() {
    local file
    echo "available=$available"
    [ "$available" = yes ] || echo "unavailable_reason=$unavailable_reason"
    [ -n "$build" ] && echo "build=$build"
    for file in $runtime_files entry.sh app/config.json app/icon.png app/launch.sh settings/GameSwitcher.json settings/icon.png settings/lang/pt.json settings/menu.sh; do
        [ -n "$payload" ] && echo "payload $file $(hash_of "$payload/$file")"
    done
    [ -n "$sample" ] && echo "sample_rom=yes" || echo "sample_rom=no"
    echo "app=$(app_kind)"
    for file in "$app"/* "$app"/.[!.]*; do [ -f "$file" ] && echo "app_file ${file##*/} $(hash_of "$file")"; done
    echo "settings=$(tile_kind)"
    [ -d "$tile" ] && (cd "$tile" && find . -type f | LC_ALL=C sort) | while read -r file; do
        echo "settings_file $file $(hash_of "$tile/$file")"
    done
    if [ -d "$old_record" ]; then
        file=$(old_problem)
        if [ -z "$file" ]; then echo "old_activation=valid"; else echo "old_activation=changed $file"; fi
        cat "$old_record/files" "$old_record/launchers" 2>/dev/null | digest | sed 's/^/old_record /'
    else
        echo "old_activation=absent"
    fi
    for file in "$base/entry.sh" "$record" "$base/DISABLED" "$runtimes"/* "$runtimes"/*/* "$records"/*; do
        [ -f "$file" ] && echo "installed $(dev "$file") $(hash_of "$file")"
    done
    echo "$table" | while IFS='|' read -r path hash form st core reason; do
        [ -n "$path" ] && echo "launcher $path hash=$hash form=$form state=$st"
    done
    running && echo "running=yes" || echo "running=no"
}
fingerprint() { state | digest; }

# Sets the plan from the table: changes ("state path core" rows to write), and the files to
# write or remove.
plan() {
    local path hash form st core reason dir left=no
    changes=''
    runtime_write=no; entry_write=no; app_write=no; app_remove=no; old_runtimes=''; disabled_remove=no
    tile_write=no; tile_remove=no
    while IFS='|' read -r path hash form st core reason; do
        case "$st" in
            install|from-menu|migrate|update|release) changes="$changes$st $path $core
" ;;
            left) left=yes ;;
        esac
    done <<EOF
$table
EOF
    if [ "$available" = yes ]; then
        runtime_valid "$build" "$manifest_hash" || runtime_write=yes
        [ "$(hash_of "$base/entry.sh")" = "$(hash_of "$payload/entry.sh")" ] || entry_write=yes
        app_current || app_write=yes
        tile_current || tile_write=yes
    else
        [ "$(app_kind)" = brickos ] && app_remove=yes
        [ "$(tile_kind)" = brickos ] && tile_remove=yes
        # A launcher left as it was still runs the entry, so the runtime stays with it.
        [ "$left" = yes ] && return 0
        [ -f "$base/entry.sh" ] && entry_write=remove
        [ -f "$runtimes/current" ] && runtime_write=remove
    fi
    for dir in "$runtimes"/*/; do
        [ -d "$dir" ] || continue
        dir=${dir%/}
        [ "$available" = yes ] && [ "${dir##*/}" = "$build" ] && continue
        old_runtimes="$old_runtimes$(dev "$dir")
"
    done
    [ -e "$base/DISABLED" ] && disabled_remove=yes
    return 0
}

up_to_date() {
    [ -z "$changes" ] && [ "$runtime_write" = no ] && [ "$entry_write" = no ] && [ "$app_write" = no ] &&
        [ "$app_remove" = no ] && [ "$tile_write" = no ] && [ "$tile_remove" = no ] &&
        [ -z "$old_runtimes" ] && [ "$disabled_remove" = no ] && [ ! -d "$old_record" ] &&
        { [ "$available" != yes ] || [ -f "$record" ]; }
}

print_plan() {
    local path hash form st core reason
    [ -d "$old_record" ] && echo "plan migrate $(dev "$old_record")"
    [ "$runtime_write" = yes ] && echo "plan runtime $dbase/runtime/$build $manifest_hash"
    [ "$runtime_write" = remove ] && echo "plan remove $dbase/runtime/current"
    [ "$entry_write" = yes ] && echo "plan entry $dbase/entry.sh $(hash_of "$payload/entry.sh")"
    [ "$entry_write" = remove ] && echo "plan remove $dbase/entry.sh"
    [ "$app_write" = yes ] && echo "plan app $dsd/Apps/GameSwitcher"
    [ "$app_remove" = yes ] && echo "plan remove $dsd/Apps/GameSwitcher"
    [ "$tile_write" = yes ] && echo "plan settings $dsd/BrickOS/settings/GameSwitcher"
    [ "$tile_remove" = yes ] && echo "plan remove $dsd/BrickOS/settings/GameSwitcher"
    [ "$disabled_remove" = yes ] && echo "plan remove $dbase/DISABLED"
    echo "$old_runtimes" | while read -r path; do [ -n "$path" ] && echo "plan remove $path"; done
    while IFS='|' read -r path hash form st core reason; do
        case "$st" in
            install|from-menu|migrate|update) echo "plan launcher $path $st backup=$dbase/launchers/$(name_of "$path").backup" ;;
            current|release) echo "plan launcher $path $st" ;;
            left) echo "plan launcher $path left $reason; its backup stays at $dbase/launchers/$(name_of "$path").backup" ;;
            pass) echo "plan launcher $path pass $reason" ;;
        esac
    done <<EOF
$table
EOF
}

preflight() {
    classify_all
    state
    echo "fingerprint=$(fingerprint)"
    plan
    print_plan
    up_to_date && echo "up_to_date=yes" || echo "up_to_date=no"
}

# Copies a file under a temporary name, checks its hash, and renames it into place.
stage() {
    local source=$1 destination=$2 expected=$3
    cp "$source" "$destination.brickos-tmp" && [ "$(hash_of "$destination.brickos-tmp")" = "$expected" ] ||
        { rm -f "$destination.brickos-tmp"; fail "could not stage $(dev "$destination")"; }
    mv "$destination.brickos-tmp" "$destination" || fail "could not write $(dev "$destination")"
}

# Asks the earlier activation's input watchdog to stop its daemon and restore the official one.
stop_old_daemon() {
    local i=0
    [ -e "$old_run/ready" ] && [ ! -e "$old_run/restored" ] || return 0
    touch "$old_run/stop" || return 1
    while [ ! -e "$old_run/restored" ] && [ "$i" -lt $((stop_timeout_s * 10)) ]; do sleep 0.1; i=$((i + 1)); done
    [ -e "$old_run/restored" ] && echo "stopped=the earlier activation's input daemon"
}

# Undoes GameSwitcher's own earlier activation, as its installer's rollback did, and removes it.
undo_old() {
    local problem number before after target launcher backup expected dir count=0
    problem=$(old_problem)
    [ -z "$problem" ] || fail "GameSwitcher's own activation changed: $problem"
    stop_old_daemon || fail "the earlier activation's input daemon did not stop"
    while IFS='|' read -r number before after target; do
        [ "$(hash_of "$root$target")" = "$after" ] || continue
        if [ "$before" = absent ]; then
            rm -f "$root$target" || fail "cannot remove $target"
        else
            stage "$old_record/$number.original" "$root$target" "$before"
        fi
        echo "migrated=$target"
        count=$((count + 1))
    done < "$old_record/files"
    while IFS='|' read -r launcher backup expected; do
        [ -e "$root$backup" ] && [ "$(hash_of "$root$backup")" = "$expected" ] && rm -f "$root$backup"
    done < "$old_record/launchers"
    for dir in "$runtimes"/*/; do
        dir=${dir%/}
        case "${dir##*/}" in *[!0-9a-f]*) continue ;; esac
        [ "${#dir}" -gt 0 ] && [ "$(($(echo "${dir##*/}" | wc -c)))" = 65 ] && rm -rf "$dir"
    done
    # The earlier pointer held one 64-digit version. On the FAT card it is the same file as
    # runtime/current, which a later install writes.
    if [ -f "$runtimes/CURRENT" ] && [ "$(($(wc -c < "$runtimes/CURRENT")))" = 65 ] && grep -qx '[0-9a-f]*' "$runtimes/CURRENT"; then
        rm -f "$runtimes/CURRENT"
    fi
    rmdir "$runtimes" 2>/dev/null
    rmdir "$app" 2>/dev/null
    rm -rf "$old_record" || fail "cannot remove $(dev "$old_record")"
    sync
    echo "migrated_files=$count"
}

do_migrate() {
    classify_all
    [ "$(fingerprint)" = "$expected_fingerprint" ] || fail "the device state changed since the dry run"
    running && fail "a game or GameSwitcher is running"
    if [ ! -d "$old_record" ]; then echo "result=nothing-to-migrate"; return 0; fi
    undo_old
    echo "result=migrated"
}

# Removes the files of an unavailable or rolled-back GameSwitcher that no launcher needs.
remove_files() {
    if [ "$(app_kind)" = brickos ]; then rm -rf "$app" && echo "removed=$dsd/Apps/GameSwitcher"; fi
    rm -rf "$app.brickos-tmp" "$app.brickos-old"
    if [ "$(tile_kind)" = brickos ]; then rm -rf "$tile" && echo "removed=$dsd/BrickOS/settings/GameSwitcher"; fi
    rm -rf "$tile.brickos-tmp" "$tile.brickos-old"
    if [ -z "$(ls "$records" 2>/dev/null)" ]; then
        rm -rf "$runtimes" "$records" "$base/entry.sh" "$record" "$base/DISABLED"
        rmdir "$base" 2>/dev/null
        [ "$parent_created" = 1 ] && rmdir "$parent/settings" "$parent" 2>/dev/null
    fi
    return 0
}

do_install() {
    local kind path core name target backup original staged='' changed=0 released=0 rec trec
    classify_all
    [ "$(fingerprint)" = "$expected_fingerprint" ] || fail "the device state changed since the dry run"
    running && fail "a game or GameSwitcher is running"
    if [ -d "$old_record" ]; then
        local problem
        problem=$(old_problem)
        [ -z "$problem" ] || fail "GameSwitcher's own activation changed: $problem"
    fi
    [ "$(app_kind)" = foreign ] && fail "$dsd/Apps/GameSwitcher exists and was not written by BrickOS"
    [ "$(tile_kind)" = foreign ] && fail "$dsd/BrickOS/settings/GameSwitcher exists and was not written by BrickOS"
    if [ -d "$old_record" ]; then
        undo_old
        classify_all
    fi
    plan
    if up_to_date; then
        echo "result=up-to-date"
        state
        return 0
    fi
    parent_created=$(field "$record" parent_created)
    [ -n "$parent_created" ] || { [ -d "$parent" ] && parent_created=0 || parent_created=1; }
    abort() {
        echo "$staged" | while read -r path; do
            [ -n "$path" ] || continue
            name=$(name_of "$path")
            rm -f "$root$path.brickos-tmp" "$records/$name.record.brickos-tmp" "$records/$name.backup.brickos-tmp"
        done
        rm -rf "$runtimes/.new-$build" "$app.brickos-tmp" "$tile.brickos-tmp"
        rm -f "$base/entry.sh.brickos-tmp" "$runtimes/current.brickos-tmp" "$record.brickos-tmp"
        fail "$*"
    }
    mkdir -p "$records" || abort "cannot create $(dev "$records")"
    if [ "$available" = yes ]; then
        if [ "$runtime_write" = yes ] && ! { [ -f "$runtimes/$build/MANIFEST.sha256" ] &&
            [ "$(hash_of "$runtimes/$build/MANIFEST.sha256")" = "$manifest_hash" ] &&
            (cd "$runtimes/$build" && sha256sum -c MANIFEST.sha256 >/dev/null 2>&1); }; then
            rm -rf "$runtimes/.new-$build"
            mkdir -p "$runtimes/.new-$build" || abort "cannot create the runtime folder"
            for path in $runtime_files; do cp "$payload/$path" "$runtimes/.new-$build/$path" || abort "cannot copy $path"; done
            chmod a+x "$runtimes/.new-$build/GameSwitcher" "$runtimes/.new-$build/gs-session"
            payload_manifest > "$runtimes/.new-$build/MANIFEST.sha256"
            (cd "$runtimes/.new-$build" && sha256sum -c MANIFEST.sha256 >/dev/null 2>&1) &&
                [ "$(hash_of "$runtimes/.new-$build/MANIFEST.sha256")" = "$manifest_hash" ] ||
                abort "the copied runtime does not match its manifest"
            # The coordinator finds the session library beside itself, as inputd's lease check requires.
            "$runtimes/.new-$build/gs-session" --check-runtime >/dev/null 2>&1 || abort "the copied runtime failed its check"
            rm -rf "$runtimes/$build"
            mv "$runtimes/.new-$build" "$runtimes/$build" || abort "cannot place the runtime"
        fi
        if [ "$entry_write" = yes ]; then
            cp "$payload/entry.sh" "$base/entry.sh.brickos-tmp" &&
                [ "$(hash_of "$base/entry.sh.brickos-tmp")" = "$(hash_of "$payload/entry.sh")" ] || abort "cannot stage entry.sh"
        fi
        if [ "$app_write" = yes ]; then
            rm -rf "$app.brickos-tmp"
            mkdir -p "$app.brickos-tmp" || abort "cannot stage the app"
            for path in $app_files; do
                cp "$payload/app/$path" "$app.brickos-tmp/$path" &&
                    [ "$(hash_of "$app.brickos-tmp/$path")" = "$(hash_of "$payload/app/$path")" ] || abort "cannot stage the app's $path"
            done
        fi
        if [ "$tile_write" = yes ]; then
            rm -rf "$tile.brickos-tmp"
            mkdir -p "$tile.brickos-tmp/lang" "$tile.brickos-tmp/systems" || abort "cannot stage Settings > GameSwitcher"
            systems_menu > "$tile.brickos-tmp/systems/systems.json" || abort "cannot stage Settings > GameSwitcher's systems"
            for path in $tile_files; do
                cp "$payload/settings/$path" "$tile.brickos-tmp/$path" &&
                    [ "$(hash_of "$tile.brickos-tmp/$path")" = "$(hash_of "$payload/settings/$path")" ] ||
                    abort "cannot stage Settings > GameSwitcher's $path"
            done
        fi
    fi
    # Every launcher, its backup and its record are staged before any is renamed into place.
    while read -r kind path core; do
        [ -n "$path" ] || continue
        name=$(name_of "$path"); target=$root$path; backup=$records/$name.backup; rec=$records/$name.record
        staged="$staged$path
"
        case "$kind" in
            install)
                cp "$target" "$backup.brickos-tmp" && [ "$(hash_of "$backup.brickos-tmp")" = "$(hash_of "$target")" ] ||
                    abort "cannot back up $path"
                original=$(hash_of "$target") ;;
            from-menu)
                trec=$tmenu_records/$name.record
                original=$(field "$trec" original_hash)
                cp "$tmenu_records/$name.backup" "$backup.brickos-tmp" && [ "$(hash_of "$backup.brickos-tmp")" = "$original" ] ||
                    abort "cannot take over the menu's backup of $path" ;;
            update)
                original=$(field "$rec" original_hash) ;;
            release)
                original=$(field "$rec" original_hash)
                cp "$backup" "$target.brickos-tmp" && [ "$(hash_of "$target.brickos-tmp")" = "$original" ] ||
                    abort "cannot restore $path"
                chmod a+x "$target.brickos-tmp"
                continue ;;
            *) abort "unexpected state $kind for $path" ;;
        esac
        wrapper "$path" "$core" > "$target.brickos-tmp" || abort "cannot write the new $path"
        chmod a+x "$target.brickos-tmp"
        {
            echo "launcher=$path"
            echo "original_hash=$original"
            echo "installed_hash=$(hash_of "$target.brickos-tmp")"
            echo "core=$core"
            echo "build=$build"
            echo "installed_at=$(date +%s)"
        } > "$rec.brickos-tmp" || abort "cannot write the record of $path"
    done <<EOF
$changes
EOF
    if [ "$available" = yes ]; then
        printf '%s %s\n' "$build" "$manifest_hash" > "$runtimes/current.brickos-tmp" || abort "cannot stage runtime/current"
        printf 'format=1\nparent_created=%s\n' "$parent_created" > "$record.brickos-tmp" || abort "cannot stage the install record"
    fi
    # Activation: the entry, the runtime pointer and the app, then each launcher with its backup
    # and record. A launcher released by an unavailable GameSwitcher gets its stock bytes back.
    if [ "$available" = yes ]; then
        [ "$entry_write" = yes ] && mv "$base/entry.sh.brickos-tmp" "$base/entry.sh"
        mv "$runtimes/current.brickos-tmp" "$runtimes/current"
        if [ "$app_write" = yes ]; then
            rm -rf "$app.brickos-old"
            if [ -e "$app" ]; then mv "$app" "$app.brickos-old" || abort "cannot replace the app"; fi
            mv "$app.brickos-tmp" "$app" || abort "cannot place the app"
            rm -rf "$app.brickos-old"
            echo "app=written"
        fi
        if [ "$tile_write" = yes ]; then
            rm -rf "$tile.brickos-old"
            if [ -e "$tile" ]; then mv "$tile" "$tile.brickos-old" || abort "cannot replace Settings > GameSwitcher"; fi
            mv "$tile.brickos-tmp" "$tile" || abort "cannot place Settings > GameSwitcher"
            rm -rf "$tile.brickos-old"
            echo "settings=written"
        fi
    fi
    while read -r kind path core; do
        [ -n "$path" ] || continue
        name=$(name_of "$path")
        if [ "$kind" = release ]; then
            mv "$root$path.brickos-tmp" "$root$path"
            rm -f "$records/$name.record" "$records/$name.backup"
            echo "released=$path"
            released=$((released + 1))
            continue
        fi
        [ -f "$records/$name.backup.brickos-tmp" ] && mv "$records/$name.backup.brickos-tmp" "$records/$name.backup"
        mv "$root$path.brickos-tmp" "$root$path"
        mv "$records/$name.record.brickos-tmp" "$records/$name.record"
        if [ "$kind" = from-menu ]; then
            rm -f "$tmenu_records/$name.record" "$tmenu_records/$name.backup"
            echo "taken_from_menu=$path"
        fi
        echo "changed=$path"
        changed=$((changed + 1))
    done <<EOF
$changes
EOF
    if [ "$available" = yes ]; then
        echo "$old_runtimes" | while read -r path; do [ -n "$path" ] && rm -rf "$root$path" && echo "removed=$path"; done
        rm -f "$base/DISABLED"
        mv "$record.brickos-tmp" "$record"
    else
        remove_files
    fi
    sync
    echo "result=installed"
    echo "available=$available"
    [ "$available" = yes ] || echo "reason=GameSwitcher is unavailable: $unavailable_reason"
    echo "launchers_changed=$changed"
    echo "launchers_released=$released"
    classify_all
    state
}

do_rollback() {
    local path hash form st core reason name target backup original restored=0 left=0
    running && fail "a game or GameSwitcher is running"
    classify_all
    if [ ! -d "$old_record" ] && [ ! -e "$record" ] && [ -z "$(ls "$records" 2>/dev/null)" ] && [ ! -e "$runtimes" ] &&
        [ ! -e "$base/entry.sh" ] && [ "$(app_kind)" != brickos ] && [ "$(tile_kind)" != brickos ]; then
        echo "result=nothing-installed"
        state
        return 0
    fi
    [ "$(app_kind)" = foreign ] && echo "app_left=$dsd/Apps/GameSwitcher was not written by BrickOS"
    [ "$(tile_kind)" = foreign ] && echo "settings_left=$dsd/BrickOS/settings/GameSwitcher was not written by BrickOS"
    [ -d "$old_record" ] && undo_old
    parent_created=$(field "$record" parent_created)
    while IFS='|' read -r path hash form st core reason; do
        [ -n "$path" ] || continue
        name=$(name_of "$path"); target=$root$path; backup=$records/$name.backup
        [ -f "$records/$name.record" ] || continue
        original=$(field "$records/$name.record" original_hash)
        if [ "$form" = gameswitcher ]; then
            stage "$backup" "$target" "$original"
        elif [ "$hash" != "$original" ]; then
            echo "left=$path $reason; its backup stays at $(dev "$backup")"
            left=$((left + 1))
            continue
        fi
        rm -f "$records/$name.record" "$backup"
        echo "restored=$path"
        restored=$((restored + 1))
    done <<EOF
$table
EOF
    remove_files
    sync
    echo "result=rolled-back"
    echo "launchers_restored=$restored"
    echo "launchers_left=$left"
    classify_all
    state
}

case "$command" in
preflight) preflight ;;
migrate) do_migrate ;;
install) do_install ;;
rollback) do_rollback ;;
*) echo "unknown command $command" >&2; exit 2 ;;
esac
