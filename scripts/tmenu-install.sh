#!/bin/sh
# Device-side helper for the opt-in BrickOS in-game menu.
#
#   tmenu-install.sh preflight CONFIG   Report the state an install depends on, its
#                                       fingerprint, and the plan. Changes nothing.
#   tmenu-install.sh install CONFIG     Install or update the menu library for a list of
#                                       launchers if the state still has the fingerprint
#                                       of the dry run.
#   tmenu-install.sh rollback CONFIG    Put back the exact bytes of every launcher that
#                                       still holds BrickOS's line, and remove the BrickOS
#                                       files no launcher left alone needs.
#
# The install changes one line of each launcher: RetroArch's command gets the BrickOS
# library folder in front of LD_LIBRARY_PATH. A launcher qualifies when it has exactly one
# standard RetroArch command, or BrickOS's form of it; any other is reported and left
# alone, as are MainUI and /usr/trimui. Each launcher has a record and an exact-byte backup
# under BrickOS/tmenu/launchers/, and each build has its own library folder, removed once
# no launcher points at it. An install from the earlier single-launcher format is migrated
# on first use. A rollback restores only launchers whose bytes are still BrickOS's, and
# strips BrickOS's line from a listed launcher that has no record, as after the BrickOS
# folder was deleted by hand.
#
# A launcher that goes through GameSwitcher, known by GameSwitcher's marker line or record, never
# gets the line: GameSwitcher loads the menu itself. The library is installed even when no
# launcher takes the line, and BrickOS/tmenu/current names its build, because GameSwitcher's
# fallback launches load the menu from there.
#
# CONFIG is a shell fragment with a space-separated `launchers` list; every path is below
# `root`, which is empty on the device. Files are staged under a temporary name and renamed
# into place, the launchers last.
set -u

command=${1:-}
config=${2:-}
[ -n "$command" ] && [ -f "$config" ] || { echo "usage: tmenu-install.sh preflight|install|rollback CONFIG" >&2; exit 2; }
root=''
launchers=''
launcher=''
candidate=''
candidate_hash=''
build=''
expected_fingerprint=''
. "$config"
# The single launcher of earlier configs.
[ -n "$launcher" ] && launchers="$launchers $launcher"
for item in $launchers; do
    case "$item" in /mnt/SDCARD/Emus/*/*.sh) ;; *) echo "launcher must be an SD emulator script" >&2; exit 2 ;; esac
done
case "$command:$build" in rollback:*) ;; *:*[!0-9a-f]*|*:) echo "build must be hexadecimal" >&2; exit 2 ;; esac

sd=$root/mnt/SDCARD
install_parent=$sd/BrickOS
install=$install_parent/tmenu
record=$install/install-record
records=$install/launchers
retroarch=$sd/RetroArch/ra64.trimui
official=$root/usr/trimui/lib/libtmenu.so
# The library folder as the launchers see it on the device.
libdir=/mnt/SDCARD/BrickOS/tmenu/$build
marker='# BrickOS in-game menu'
gameswitcher_records=$install_parent/gameswitcher/launchers
gameswitcher_marker='# BrickOS GameSwitcher launcher'
# The awk forms avoid backslashes, which awk -v would interpret.
exec_re='^HOME=[$]RA_DIR/ [$]RA_DIR/ra64[.]trimui '
prefix_re='^LD_LIBRARY_PATH=/mnt/SDCARD/BrickOS/tmenu/[0-9a-f]+:[$]LD_LIBRARY_PATH '
suffix=" $marker"

hash_of() { if [ -f "$1" ]; then sha256sum "$1" | cut -d ' ' -f 1; else echo absent; fi; }
fail() { echo "result=refused"; echo "reason=$*"; exit 3; }
running() { [ -n "$(pidof ra64.trimui 2>/dev/null)" ]; }
name_of() { echo "$1" | tr '/' '_'; }
field() { sed -n "s/^$2=//p" "$1" 2>/dev/null; }

# The awk functions shared by every launcher transform. strip() returns the RetroArch command
# inside BrickOS's line, or nothing for any other line.
awk_lib='
function strip(l,   s) {
    if (match(l, pre) && substr(l, length(l) - length(suf) + 1) == suf) {
        s = substr(l, RLENGTH + 1, length(l) - RLENGTH - length(suf))
        if (s ~ exe) return s
    }
    return ""
}'
awk_run() { awk -v pre="$prefix_re" -v exe="$exec_re" -v suf="$suffix" -v dir="$libdir" "$awk_lib $1" "$2"; }
# Prints the counts of standard commands, BrickOS lines, and lines with a carriage return.
scan() { awk_run '/\r/ { cr++ } { if (strip($0) != "") b++; else if ($0 ~ exe) e++ } END { print e + 0, b + 0, cr + 0 }' "$1"; }
# The launcher without BrickOS's line, with its original command back.
without_menu() { awk_run '{ s = strip($0); print (s != "" ? s : $0) }' "$1"; }
# The launcher with BrickOS's line for this build in place of its one RetroArch command.
with_menu() {
    awk_run '{ s = strip($0); l = (s != "" ? s : $0)
        if (!done && l ~ exe) { print "LD_LIBRARY_PATH=" dir ":$LD_LIBRARY_PATH " l suf; done = 1 } else print $0 }' "$1"
}
digest() { sha256sum | cut -d ' ' -f 1; }
# The build a launcher's BrickOS line points at.
content_build() { sed -n 's#^LD_LIBRARY_PATH=/mnt/SDCARD/BrickOS/tmenu/\([0-9a-f]*\):.*#\1#p' "$1" 2>/dev/null | head -n 1; }

# A launcher's record and backup, in the current or the single-launcher format.
record_of() {
    if [ -f "$records/$(name_of "$1").record" ]; then echo "$records/$(name_of "$1").record"
    elif [ -f "$record" ] && [ "$(field "$record" launcher)" = "$1" ]; then echo "$record"; fi
}
backup_of() {
    if [ -f "$records/$(name_of "$1").backup" ]; then echo "$records/$(name_of "$1").backup"
    else echo "$install/backup/$(name_of "$1")"; fi
}
recorded() {
    local file
    for file in "$records"/*.record "$record"; do [ -f "$file" ] && field "$file" launcher; done
}
known() { { for item in $launchers; do echo "$item"; done; recorded; } | awk 'NF && !seen[$0]++'; }
listed() { local item; for item in $launchers; do [ "$item" = "$1" ] && return 0; done; return 1; }

# Sets st_hash, st_exec, st_brickos, st_build, st_state, and st_reason for a launcher. States:
# new (one standard command, gets the menu), current (BrickOS's line for this build), update
# (BrickOS's line for another build), orphan (BrickOS's line without a record), changed (it
# differs from what BrickOS wrote), gameswitcher (it goes through GameSwitcher), ineligible, and
# missing.
classify() {
    local path=$1 target=$root$1 rec backup counts
    st_state=; st_reason=; st_build=; st_exec=0; st_brickos=0; st_cr=0
    if [ ! -f "$target" ]; then st_hash=absent; st_state=missing; st_reason="the launcher is missing"; return; fi
    st_hash=$(hash_of "$target")
    if [ -f "$gameswitcher_records/$(name_of "$path").record" ] || grep -qx "$gameswitcher_marker" "$target"; then
        st_state=gameswitcher; st_reason="the launcher goes through GameSwitcher"
        return
    fi
    counts=$(scan "$target")
    st_exec=${counts%% *}; st_cr=${counts##* }; counts=${counts#* }; st_brickos=${counts%% *}
    st_build=$(content_build "$target")
    rec=$(record_of "$path")
    if [ -n "$rec" ] && [ "$st_hash" != "$(field "$rec" original_hash)" ]; then
        backup=$(backup_of "$path")
        # BrickOS's bytes: the backup with only the RetroArch command changed to BrickOS's line.
        if [ "$st_brickos" = 1 ] && [ "$st_exec" = 0 ] && [ "$(hash_of "$backup")" = "$(field "$rec" original_hash)" ] &&
            [ "$(without_menu "$target" | digest)" = "$(awk '{ print }' "$backup" | digest)" ]; then
            [ "$st_build" = "$build" ] && st_state=current || st_state=update
        else
            st_state=changed; st_reason="the launcher changed after the install"
        fi
    elif [ -z "$rec" ] && [ "$st_brickos" = 1 ] && [ "$st_exec" = 0 ]; then
        st_state=orphan
    elif [ "$st_cr" != 0 ]; then
        st_state=ineligible; st_reason="the launcher has CRLF line endings"
    elif [ "$st_exec" = 1 ] && [ "$st_brickos" = 0 ]; then
        st_state=new
    else
        st_state=ineligible; st_reason="the launcher has $((st_exec + st_brickos)) RetroArch commands, not 1"
    fi
}

state() {
    local path
    [ -f "$retroarch" ] && echo "retroarch_present=yes" || echo "retroarch_present=no"
    [ -f "$official" ] && echo "official_present=yes" || echo "official_present=no"
    [ -d "$install_parent" ] && echo "brickos_dir=present" || echo "brickos_dir=absent"
    if [ ! -f "$record" ]; then echo "install_format=none"
    elif [ -n "$(field "$record" launcher)" ]; then echo "install_format=1"
    else echo "install_format=2"; fi
    known | while read -r path; do
        classify "$path"
        listed "$path" && in_list=yes || in_list=no
        echo "launcher $path hash=$st_hash exec_lines=$st_exec brickos_lines=$st_brickos listed=$in_list state=$st_state"
    done
    if [ -d "$install" ]; then
        find "$install" -type f | sort | while read -r file; do echo "installed ${file#"$root"} $(hash_of "$file")"; done
    fi
    running && echo "retroarch_running=yes" || echo "retroarch_running=no"
}

fingerprint() { state | digest; }

# Sets the plan: menu_launchers (listed launchers that end with BrickOS's line), changes (the
# launchers to write, as "state path" lines), skips ("path reason" lines), keeps (recorded
# launchers not listed), gameswitcher_launchers, library_write, current_write, and the old
# library folders to remove.
plan() {
    local path file refs dir name
    menu_launchers=; changes=; skips=; keeps=; old_libraries=; gameswitcher_launchers=
    for path in $(known); do
        classify "$path"
        if ! listed "$path"; then
            keeps="$keeps$path
"
            continue
        fi
        case "$st_state" in
            new|update|orphan) changes="$changes$st_state $path
"; menu_launchers="$menu_launchers$path
" ;;
            current) menu_launchers="$menu_launchers$path
" ;;
            gameswitcher) gameswitcher_launchers="$gameswitcher_launchers$path
" ;;
            *) skips="$skips$path $st_reason
" ;;
        esac
    done
    library_write=no
    [ "$(hash_of "$install/$build/libtmenu.so")" != "$candidate_hash" ] && library_write=yes
    current_write=no
    [ "$(cat "$install/current" 2>/dev/null)" = "$build" ] || current_write=yes
    # The builds still referenced after the install: this one, and those of every record or
    # launcher the install leaves as it is.
    refs=$(
        echo "$build"
        for file in "$records"/*.record "$record"; do
            [ -f "$file" ] && path=$(field "$file" launcher) && [ -n "$path" ] || continue
            echo "$menu_launchers" | grep -qxF "$path" || field "$file" build
        done
        for file in "$sd"/Emus/*/*.sh; do
            [ -f "$file" ] || continue
            echo "$menu_launchers" | grep -qxF "${file#"$root"}" || content_build "$file"
        done
    )
    for dir in "$install"/*/; do
        [ -d "$dir" ] || continue
        dir=${dir%/}; name=${dir##*/}
        case "$name" in launchers|backup|*[!0-9a-f]*) continue ;; esac
        echo "$refs" | grep -qx "$name" || old_libraries="$old_libraries${dir#"$root"}
"
    done
}

up_to_date() {
    [ -z "$changes" ] && [ "$library_write" = no ] && [ "$current_write" = no ] && [ -z "$old_libraries" ] &&
        [ -z "$(field "$record" launcher)" ]
}

print_plan() {
    local path reason kind
    [ "$library_write" = yes ] && echo "plan library $libdir/libtmenu.so $candidate_hash"
    [ "$current_write" = yes ] && echo "plan current /mnt/SDCARD/BrickOS/tmenu/current $build"
    [ -n "$(field "$record" launcher)" ] && echo "plan migrate $(field "$record" launcher)"
    echo "$changes" | while read -r kind path; do
        [ -n "$path" ] || continue
        case "$kind" in new) kind=install ;; orphan) kind=adopt ;; esac
        echo "plan launcher $path $kind backup=${records#"$root"}/$(name_of "$path").backup"
    done
    echo "$menu_launchers" | while read -r path; do
        [ -n "$path" ] && ! echo "$changes" | cut -d ' ' -f 2 | grep -qxF "$path" && echo "plan launcher $path current"
    done
    echo "$gameswitcher_launchers" | while read -r path; do [ -n "$path" ] && echo "plan launcher $path gameswitcher"; done
    echo "$skips" | while read -r path reason; do [ -n "$path" ] && echo "plan launcher $path skip $reason"; done
    echo "$keeps" | while read -r path; do [ -n "$path" ] && echo "plan launcher $path keep not listed"; done
    echo "$old_libraries" | while read -r path; do [ -n "$path" ] && echo "plan remove-library $path"; done
}

preflight() {
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
        { rm -f "$destination.brickos-tmp"; fail "could not stage $destination"; }
    mv "$destination.brickos-tmp" "$destination"
}

# Moves an install in the single-launcher format to one record and backup per launcher.
migrate() {
    local path name parent_created
    path=$(field "$record" launcher)
    [ -n "$path" ] || return 0
    name=$(name_of "$path")
    parent_created=$(field "$record" parent_created)
    mkdir -p "$records" || fail "cannot create $records"
    if [ -f "$install/backup/$name" ]; then
        stage "$install/backup/$name" "$records/$name.backup" "$(hash_of "$install/backup/$name")"
    fi
    grep -v '^parent_created=' "$record" > "$records/$name.record.brickos-tmp" &&
        mv "$records/$name.record.brickos-tmp" "$records/$name.record" || fail "cannot migrate $record"
    printf 'format=2\nparent_created=%s\n' "${parent_created:-0}" > "$record.brickos-tmp" &&
        mv "$record.brickos-tmp" "$record" || fail "cannot migrate $record"
    rm -rf "$install/backup"
    echo "migrated=$path"
}

# Removes the library folders no record and no launcher points at.
remove_old_libraries() {
    local refs dir name file
    refs=$(
        cat "$install/current" 2>/dev/null
        for file in "$records"/*.record; do [ -f "$file" ] && field "$file" build; done
        for file in "$sd"/Emus/*/*.sh; do [ -f "$file" ] && content_build "$file"; done
    )
    for dir in "$install"/*/; do
        [ -d "$dir" ] || continue
        dir=${dir%/}; name=${dir##*/}
        case "$name" in launchers|backup|*[!0-9a-f]*) continue ;; esac
        echo "$refs" | grep -qx "$name" && continue
        rm -rf "$dir" && echo "removed_library=${dir#"$root"}"
    done
}

do_install() {
    [ "$(fingerprint)" = "$expected_fingerprint" ] || fail "the device state changed since the dry run"
    [ -f "$retroarch" ] || fail "ra64.trimui is missing"
    # The official library serves every call the BrickOS one does not recognize.
    [ -f "$official" ] || fail "the official libtmenu.so is missing"
    running && fail "a game is running"
    [ "$(hash_of "$candidate")" = "$candidate_hash" ] || fail "the staged library does not match its hash"
    plan
    if up_to_date; then
        echo "result=up-to-date"
        state
        return 0
    fi
    local parent_created=0 install_created=0 kind path name target backup original staged='' changed=0
    [ -d "$install_parent" ] || parent_created=1
    [ -d "$install" ] || install_created=1
    abort() {
        echo "$staged" | while read -r path; do
            [ -n "$path" ] || continue
            name=$(name_of "$path")
            rm -f "$root$path.brickos-tmp" "$records/$name.record.brickos-tmp" "$records/$name.backup.brickos-tmp"
        done
        if [ "$install_created" = 1 ]; then
            rm -rf "$install"
            [ "$parent_created" = 1 ] && rmdir "$install_parent" 2>/dev/null
        fi
        fail "$*"
    }
    mkdir -p "$records" || abort "cannot create $records"
    if [ ! -f "$record" ]; then
        printf 'format=2\nparent_created=%s\n' "$parent_created" > "$record.brickos-tmp" &&
            mv "$record.brickos-tmp" "$record" || abort "cannot write $record"
    fi
    migrate
    if [ "$library_write" = yes ]; then
        mkdir -p "$install/$build" || abort "cannot create $install/$build"
        cp "$candidate" "$install/$build/libtmenu.so.brickos-tmp" &&
            [ "$(hash_of "$install/$build/libtmenu.so.brickos-tmp")" = "$candidate_hash" ] &&
            mv "$install/$build/libtmenu.so.brickos-tmp" "$install/$build/libtmenu.so" || abort "could not stage the library"
    fi
    if [ "$current_write" = yes ]; then
        echo "$build" > "$install/current.brickos-tmp" && mv "$install/current.brickos-tmp" "$install/current" ||
            abort "cannot write $install/current"
    fi
    # Every launcher, its backup, and its record are staged before any is renamed into place.
    for path in $(echo "$changes" | cut -d ' ' -f 2); do
        classify "$path"
        kind=$st_state; name=$(name_of "$path"); target=$root$path; backup=$records/$name.backup
        staged="$staged$path
"
        case "$kind" in
            new)
                cp "$target" "$backup.brickos-tmp" && [ "$(hash_of "$backup.brickos-tmp")" = "$st_hash" ] ||
                    abort "cannot back up $path"
                original=$st_hash ;;
            orphan)
                # The original command is the one inside BrickOS's line.
                without_menu "$target" > "$backup.brickos-tmp" || abort "cannot back up $path"
                original=$(hash_of "$backup.brickos-tmp") ;;
            update)
                original=$(field "$(record_of "$path")" original_hash) ;;
            *) abort "unexpected state $kind for $path" ;;
        esac
        with_menu "$target" > "$target.brickos-tmp" || abort "cannot write the new $path"
        # Only the RetroArch command may differ from the original, and it must be BrickOS's.
        if [ -f "$backup.brickos-tmp" ]; then original_text=$backup.brickos-tmp; else original_text=$backup; fi
        [ "$(scan "$target.brickos-tmp")" = "0 1 0" ] &&
            [ "$(without_menu "$target.brickos-tmp" | digest)" = "$(awk '{ print }' "$original_text" | digest)" ] ||
            abort "the new $path differs in more than its RetroArch command"
        chmod a+x "$target.brickos-tmp"
        {
            echo "launcher=$path"
            echo "original_hash=$original"
            echo "installed_hash=$(hash_of "$target.brickos-tmp")"
            echo "build=$build"
            echo "library_hash=$candidate_hash"
            echo "installed_at=$(date +%s)"
        } > "$records/$name.record.brickos-tmp" || abort "cannot write the record of $path"
    done
    # Activation: backups, then launchers, then records.
    for path in $(echo "$changes" | cut -d ' ' -f 2); do
        name=$(name_of "$path")
        [ -f "$records/$name.backup.brickos-tmp" ] && mv "$records/$name.backup.brickos-tmp" "$records/$name.backup"
        mv "$root$path.brickos-tmp" "$root$path"
        mv "$records/$name.record.brickos-tmp" "$records/$name.record"
        echo "changed=$path"
        changed=$((changed + 1))
    done
    sync
    remove_old_libraries
    sync
    echo "result=installed"
    echo "launchers_changed=$changed"
    state
}

do_rollback() {
    running && fail "a game is running"
    local path name target backup original restored=0 left=0 stripped=0 parent_created
    if [ ! -f "$record" ] && [ -z "$(ls "$records" 2>/dev/null)" ]; then
        [ -d "$install" ] && fail "BrickOS files without an install record; remove $install by hand"
    fi
    [ -f "$record" ] && migrate
    for path in $(recorded); do
        classify "$path"
        name=$(name_of "$path"); target=$root$path; backup=$records/$name.backup
        original=$(field "$records/$name.record" original_hash)
        case "$st_state" in
            new|ineligible)
                # The launcher already has its original bytes.
                [ "$st_hash" = "$original" ] || { echo "left=$path the launcher changed after the install"; left=$((left + 1)); continue; } ;;
            current|update)
                stage "$backup" "$target" "$original" ;;
            *)
                echo "left=$path $st_reason; its backup stays at ${backup#"$root"}"
                left=$((left + 1))
                continue ;;
        esac
        rm -f "$records/$name.record" "$backup"
        echo "restored=$path"
        restored=$((restored + 1))
    done
    # A listed launcher with BrickOS's line but no record gets its original command back.
    for path in $launchers; do
        classify "$path"
        [ "$st_state" = orphan ] || continue
        target=$root$path
        without_menu "$target" > "$target.brickos-tmp" && chmod a+x "$target.brickos-tmp" &&
            mv "$target.brickos-tmp" "$target" || fail "cannot restore $path"
        echo "stripped=$path"
        stripped=$((stripped + 1))
    done
    if [ -d "$install" ]; then
        # The menu is going, so no build is current; a left launcher keeps its own build.
        rm -f "$install/current"
        remove_old_libraries
        if [ -z "$(ls "$records" 2>/dev/null)" ]; then
            parent_created=$(field "$record" parent_created)
            rm -rf "$install"
            [ "$parent_created" = 1 ] && rmdir "$install_parent" 2>/dev/null
        fi
    elif [ "$stripped" = 0 ]; then
        echo "result=nothing-installed"
        state
        return 0
    fi
    sync
    echo "result=rolled-back"
    echo "launchers_restored=$restored"
    echo "launchers_stripped=$stripped"
    echo "launchers_left=$left"
    state
}

case "$command" in
preflight) preflight ;;
install) do_install ;;
rollback) do_rollback ;;
*) echo "unknown command $command" >&2; exit 2 ;;
esac
