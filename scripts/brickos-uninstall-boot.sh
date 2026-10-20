#!/bin/sh
# BrickOS uninstall start script
# The one-shot removal of all of BrickOS. The Uninstall BrickOS app copies this script from its
# folder (uninstall.sh) to /mnt/SDCARD/System/starts/0-brickos-uninstall.sh and restarts the
# Brick. The firmware runs System/starts/*.sh once per boot, in name order and waiting for each,
# before its daemon loop; this name sorts before every other BrickOS start script, so it runs
# while no BrickOS daemon, MainUI bind or GameSwitcher watchdog exists yet. The start scripts
# it removes were in this boot's list, so the firmware skips them, and the boot goes on as a
# stock boot: the official daemons and the official MainUI.
#
# The steps, in order, each logged with its result and skipped when already done. On a card the
# release package set up, the setup's copy holds MainUI's and GameSwitcher's helpers, and step 3
# removes both, so steps 1 and 2 are left to it; they stay for cards set up by the earlier tools.
#   1. GameSwitcher, when BrickOS/gameswitcher/activation-record exists: its own install.sh
#      rollback from its runtime folder on the card. It restores MainUI's wrapper.sh and current,
#      so it runs before step 2. A record with no installer copy is a failure.
#   2. The BrickOS MainUI, when its start script or BrickOS/mainui/ exists: the app's copy of
#      scripts/mainui-install.sh rollback, with a config whose root is empty.
#   3. The setup's components: BrickOS/setup/setup.sh rollback --boot, else the setup.sh of the
#      only package on the card, else the removal by marker of the boot script
#      (brickos-00-setup.sh): the component start scripts that carry their marker line, the
#      menu lines, GameSwitcher's launchers whose backup is still there, and GameSwitcher's app.
#   4. Launcher lines that still carry the in-game menu's marker, as in a launcher edited by
#      hand after the install, get the standard RetroArch command back, and launchers still in
#      GameSwitcher's form get their stock bytes back from its backup; each one is logged.
#   5. The RetroArch loading screen, by MainUI's TRIMUI rule: app_loading_bg.png and
#      app_loading_NN.png in /mnt/SDCARD/trimui/res/skin/, then skin, res and trimui when empty.
#   6. The setup's boot script (marker checked), /mnt/SDCARD/BrickOS, the app folder and
#      GameSwitcher's (markers checked), the app's lines in Recent, and this script, last.
# The first step that fails or refuses stops the run before step 6: the log goes to
# BrickOS/uninstall.log, BrickOS/uninstall-result holds result=failed and a one-line reason=
# for the app to show, this script removes itself, and the boot goes on with what remains. A
# power cut leaves this script in place, and the next boot runs it again. Stock data, other
# apps, themes and start scripts BrickOS did not write are never touched, except that Recent
# loses the lines that open the app. Nothing under
# /usr/trimui or /etc is written, and no stock version or hash is checked.
#
# Every run logs each step and its result to /tmp/brickos-uninstall.log. BRICKOS_ROOT prefixes
# every path, for host tests. BRICKOS_UNINSTALL_STOP_AFTER (host tests only) names a step after
# which the script stops as at a power cut: gameswitcher, mainui, setup, launchers,
# loading-screen, boot-script, brickos or app.
root=${BRICKOS_ROOT:-}
stop_after=${BRICKOS_UNINSTALL_STOP_AFTER:-}
sd=$root/mnt/SDCARD
brickos=$sd/BrickOS
starts=$sd/System/starts
self=$starts/0-brickos-uninstall.sh
app=$sd/Apps/BrickOSUninstall
gameswitcher=$brickos/gameswitcher
tmp_log=$root/tmp/brickos-uninstall.log
log=$brickos/uninstall.log
result_file=$brickos/uninstall-result
trash=$sd/BrickOS.brickos-removing
work=$root/tmp/brickos-uninstall.work
menu_marker='# BrickOS in-game menu'
gameswitcher_app=$sd/Apps/GameSwitcher
# The setup's copy holds these helpers when the release package installed MainUI and GameSwitcher.
setup_gameswitcher=$brickos/setup/gameswitcher/gameswitcher-setup.sh
setup_mainui=$brickos/setup/mainui/mainui-install.sh

mkdir -p "$root/tmp" "$work"
header="== $(date '+%Y-%m-%d %H:%M:%S %Z') uninstall"
: > "$work/run"
echo "$header" >> "$tmp_log"

# Logs a line, in /tmp as the run goes and in the run's copy for BrickOS/uninstall.log.
say() { echo "$*" >> "$work/run"; echo "$*" >> "$tmp_log"; }
device() { echo "${1#"$root"}"; }
uptime_s() { cut -d ' ' -f 1 /proc/uptime 2>/dev/null; }
start() { say "start=$1 uptime=$(uptime_s)"; }
# Logs a step's report, each line prefixed with the step's name.
report() { sed "s/^/$1: /" "$2" > "$work/report"; cat "$work/report" >> "$work/run"; cat "$work/report" >> "$tmp_log"; }
# The test-only power cut: the run ends here, its script still in place.
checkpoint() {
    [ "$stop_after" = "$1" ] || return 0
    say "stopped=after $1 (test)"
    exit 0
}

# Appends the run to BrickOS/uninstall.log, which keeps its last 32 KiB from a whole line once
# it passes 64 KiB.
keep_log() {
    { echo "$header"; cat "$work/run"; } >> "$log"
    if [ "$(($(wc -c < "$log")))" -gt 65536 ]; then
        tail -c 32768 "$log" | sed '1d' > "$log.brickos-tmp" && mv "$log.brickos-tmp" "$log"
    fi
}

# Ends a run whose step failed or refused: BrickOS stays for a retry, the app shows the reason,
# and the boot goes on without this script.
failed() {
    local step=$1 reason
    reason=$(echo "$2" | tr '\n' ' ' | sed 's/  */ /g; s/ $//')
    say "$step=failed"
    say "result=failed"
    say "reason=$reason"
    mkdir -p "$brickos"
    keep_log
    printf 'result=failed\nstep=%s\nreason=%s\n' "$step" "$reason" > "$result_file.brickos-tmp" &&
        mv "$result_file.brickos-tmp" "$result_file"
    rm -f "$self"
    rm -rf "$work"
    sync
    exit 0
}

# A step's reason: its last reason= line, else its last line.
reason_of() {
    local reason
    reason=$(sed -n 's/^reason=//p' "$1" | tail -n 1)
    [ -n "$reason" ] || reason=$(grep . "$1" | tail -n 1)
    echo "${reason:-no reason given}"
}

# The package folders on the card, one per line.
packages() { local dir; for dir in "$brickos"/package/*/; do [ -f "$dir/setup.sh" ] && echo "${dir%/}"; done; }

# Puts back the standard RetroArch command in every launcher line that carries the menu's
# marker, keeping the rest of each launcher byte for byte. Prints the launchers it changed, and
# those whose marked line is not BrickOS's form. When a launcher cannot be written, it prints
# launcher_failed= with its path and returns 1.
restore_menu_lines() {
    local file
    for file in "$sd"/Emus/*/launch*.sh; do
        grep -q "$menu_marker\$" "$file" 2>/dev/null || continue
        # The copy keeps the launcher's mode; only its contents are replaced.
        if ! { cp -p "$file" "$file.brickos-tmp" &&
            sed 's|^LD_LIBRARY_PATH=/mnt/SDCARD/BrickOS/tmenu/[0-9a-f]*:\$LD_LIBRARY_PATH \(HOME=.*\) # BrickOS in-game menu$|\1|' \
                "$file" > "$file.brickos-tmp" && mv "$file.brickos-tmp" "$file"; }; then
            rm -f "$file.brickos-tmp"
            echo "launcher_failed=$(device "$file")"
            return 1
        fi
        if grep -q "$menu_marker\$" "$file"; then
            echo "launcher_left=$(device "$file") its BrickOS line was changed by hand"
        else
            echo "launcher_restored=$(device "$file")"
        fi
    done
    return 0
}

# Puts back each GameSwitcher launcher from its backup, as the boot script's marker fallback
# does. Prints the launchers it restored, and those whose backup is gone, which stay.
restore_gameswitcher_launchers() {
    local file launcher
    for file in "$gameswitcher"/launchers/*.record; do
        [ -f "$file" ] || continue
        launcher=$root$(sed -n 's/^launcher=//p' "$file")
        grep -qx '# BrickOS GameSwitcher launcher' "$launcher" 2>/dev/null || continue
        if cp "${file%.record}.backup" "$launcher.brickos-tmp" 2>/dev/null && mv "$launcher.brickos-tmp" "$launcher"; then
            echo "launcher_restored=$(device "$launcher")"
        else
            rm -f "$launcher.brickos-tmp"
            echo "launcher_left=$(device "$launcher") GameSwitcher's backup of it is gone"
        fi
    done
}

say "root=${root:-/}"

# 1. GameSwitcher.
start gameswitcher
if [ -f "$setup_gameswitcher" ]; then
    say "gameswitcher=with-setup"
elif [ -e "$gameswitcher/activation-record" ]; then
    version=$(cat "$gameswitcher/runtime/CURRENT" 2>/dev/null)
    installer=
    case "$version" in
        ''|*[!0-9a-f]*) ;;
        *) [ -f "$gameswitcher/runtime/$version/install.sh" ] && installer=$gameswitcher/runtime/$version/install.sh ;;
    esac
    # A rollback cut short removes CURRENT before it moves the record away; its only runtime
    # still holds the installer.
    if [ -z "$installer" ] && [ "$(ls "$gameswitcher"/runtime/*/install.sh 2>/dev/null | wc -l)" = 1 ]; then
        installer=$(ls "$gameswitcher"/runtime/*/install.sh)
    fi
    [ -n "$installer" ] ||
        failed gameswitcher "GameSwitcher is installed but its installer is missing from $(device "$gameswitcher/runtime"), so its launchers cannot be restored"
    say "gameswitcher_installer=$(device "$installer")"
    sh "$installer" rollback "$sd" > "$work/gameswitcher" 2>&1
    status=$?
    report gameswitcher "$work/gameswitcher"
    if [ "$status" != 0 ] || [ -e "$gameswitcher/activation-record" ]; then
        failed gameswitcher "$(grep 'refused' "$work/gameswitcher" | tail -n 1 | grep . || reason_of "$work/gameswitcher")"
    fi
    say "gameswitcher=rolled-back"
else
    say "gameswitcher=absent"
fi
checkpoint gameswitcher

# 2. The BrickOS MainUI.
start mainui
if [ -f "$setup_mainui" ]; then
    say "mainui=with-setup"
elif [ -e "$starts/brickos-mainui.sh" ] || [ -d "$brickos/mainui" ]; then
    [ -f "$app/mainui-install.sh" ] || failed mainui "the MainUI rollback helper $(device "$app/mainui-install.sh") is missing"
    echo "root='$root'" > "$work/mainui.config"
    sh "$app/mainui-install.sh" rollback "$work/mainui.config" > "$work/mainui" 2>&1
    report mainui "$work/mainui"
    [ "$(sed -n 's/^result=//p' "$work/mainui" | tail -n 1)" = rolled-back ] || failed mainui "$(reason_of "$work/mainui")"
    say "mainui=rolled-back"
else
    say "mainui=absent"
fi
checkpoint mainui

# 3. The setup's components.
start setup
setup=
if [ -f "$brickos/setup/setup.sh" ]; then
    setup=$brickos/setup/setup.sh
elif [ "$(packages | wc -l)" = 1 ]; then
    setup=$(packages)/setup.sh
fi
if [ -n "$setup" ]; then
    say "setup_script=$(device "$setup")"
    sh "$setup" rollback --boot > "$work/setup" 2>&1
    report setup "$work/setup"
    [ "$(sed -n 's/^result=//p' "$work/setup" | tail -n 1)" = rolled-back ] || failed setup "$(reason_of "$work/setup")"
    say "setup=rolled-back"
else
    # No copy of the setup: what the boot script's marker fallback identifies.
    removed=0
    for file in "$starts/brickos-inputd.sh" "$starts/brickos-osdd.sh" "$starts/brickos-ledd.sh"; do
        grep -qx -e '# BrickOS input daemon start script' -e '# BrickOS OSD start script' \
            -e '# BrickOS LED daemon start script' "$file" 2>/dev/null || continue
        rm -f "$file" || failed setup "cannot remove $(device "$file")"
        say "removed=$(device "$file")"
        removed=$((removed + 1))
    done
    restore_menu_lines > "$work/setup-lines" ||
        failed setup "cannot write $(sed -n 's/^launcher_failed=//p' "$work/setup-lines")"
    restore_gameswitcher_launchers >> "$work/setup-lines"
    if grep -qx '# BrickOS GameSwitcher app' "$gameswitcher_app/launch.sh" 2>/dev/null; then
        rm -rf "$gameswitcher_app" || failed setup "cannot remove $(device "$gameswitcher_app")"
        echo "removed=$(device "$gameswitcher_app")" >> "$work/setup-lines"
    fi
    removed=$((removed + $(grep -c . "$work/setup-lines")))
    while IFS= read -r line; do say "$line"; done < "$work/setup-lines"
    if [ "$removed" = 0 ]; then say "setup=absent"; else say "setup=removed-by-marker"; fi
fi
checkpoint setup

# 4. Menu lines left in launchers edited by hand.
start launchers
restore_menu_lines > "$work/launchers" ||
    failed launchers "cannot write $(sed -n 's/^launcher_failed=//p' "$work/launchers")"
# A GameSwitcher launcher left by the setup still runs GameSwitcher's entry, which step 6 removes,
# so it gets its stock bytes back from its backup while that is still there.
restore_gameswitcher_launchers >> "$work/launchers"
while IFS= read -r line; do say "$line"; done < "$work/launchers"
say "launchers_restored=$(grep -c '^launcher_restored=' "$work/launchers")"
checkpoint launchers

# 5. The RetroArch loading screen, as MainUI's TRIMUI choice removes it: the images, any of
# MainUI's leftovers of them, then the folders that are left empty.
start loading-screen
skin=$sd/trimui/res/skin
removed=0
for file in "$skin"/*; do
    [ -f "$file" ] && [ ! -L "$file" ] || continue
    name=$(echo "${file##*/}" | tr 'A-Z' 'a-z')
    case "$name" in
        app_loading_bg.png|app_loading_[0-9][0-9].png) ;;
        app_loading_bg.png.brickos-tmp|app_loading_[0-9][0-9].png.brickos-tmp) ;;
        app_loading_bg.png.brickos-old|app_loading_[0-9][0-9].png.brickos-old) ;;
        *) continue ;;
    esac
    rm -f "$file" || failed loading-screen "cannot remove $(device "$file")"
    removed=$((removed + 1))
done
for dir in "$skin" "$sd/trimui/res" "$sd/trimui"; do rmdir "$dir" 2>/dev/null; done
say "loading_screen_removed=$removed"
if [ -e "$sd/trimui" ]; then say "trimui=kept"; else say "trimui=absent"; fi
checkpoint loading-screen

# 6. The files left: the boot script, BrickOS, the app, and this script.
start delete
if [ -f "$starts/brickos-00-setup.sh" ]; then
    if grep -qx '# BrickOS setup start script' "$starts/brickos-00-setup.sh"; then
        rm -f "$starts/brickos-00-setup.sh"
        say "removed=$(device "$starts/brickos-00-setup.sh")"
    else
        say "left=$(device "$starts/brickos-00-setup.sh") was not written by BrickOS"
    fi
fi
checkpoint boot-script
# The folder is renamed first, so a power cut never leaves a BrickOS folder half removed for
# the steps above to read.
if [ -e "$brickos" ]; then
    rm -rf "$trash"
    mv "$brickos" "$trash" 2>/dev/null || rm -rf "$brickos"
fi
rm -rf "$trash"
[ ! -e "$brickos" ] && [ ! -e "$trash" ] || failed delete "cannot remove $(device "$brickos")"
say "removed=$(device "$brickos")"
checkpoint brickos
if [ -e "$app" ] && grep -qx '# BrickOS uninstall app' "$app/launch.sh" 2>/dev/null; then
    rm -rf "$app"
    say "removed=$(device "$app")"
fi
rm -rf "$app.brickos-tmp" "$app.brickos-old"
if [ -e "$gameswitcher_app" ] && grep -qx '# BrickOS GameSwitcher app' "$gameswitcher_app/launch.sh" 2>/dev/null; then
    rm -rf "$gameswitcher_app"
    say "removed=$(device "$gameswitcher_app")"
fi
rm -rf "$gameswitcher_app.brickos-tmp" "$gameswitcher_app.brickos-old"
# Recent forgets the app, which no longer opens: its lines go, and the other lines keep their
# bytes and order. A Recent without them is not rewritten. A failed rewrite keeps Recent as it
# was and does not stop the run.
recent=$sd/Roms/recentlist.json
app_entry='"launch"[[:space:]]*:[[:space:]]*"/mnt/SDCARD/Apps/BrickOSUninstall/launch.sh"'
if [ -f "$recent" ] && grep -q "$app_entry" "$recent"; then
    if sed "\#$app_entry#d" "$recent" > "$recent.brickos-tmp" && mv -f "$recent.brickos-tmp" "$recent"; then
        say "recent=removed"
    else
        rm -f "$recent.brickos-tmp"
        say "recent=kept, $(device "$recent") could not be rewritten"
    fi
fi
checkpoint app
say "result=finished"
say "uptime=$(uptime_s)"
rm -rf "$work"
rm -f "$self"
sync
exit 0
