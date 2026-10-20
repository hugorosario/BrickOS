#!/bin/sh
# BrickOS updater boot activation. Called before the regular package setup, never at runtime.
# A pending package stays in its own SD-card staging tree: its ../../../System manifest entry
# still resolves to that tree's bootstrap, so verification never depends on overwriting the
# active bootstrap first. Pending is removed last, making an interrupted commit retryable.
#
# Exit status: 0 when no update was pending or it was installed and committed; 3 when the
# package's setup installed it but the commit failed, so the boot keeps the new build without
# running the enabled build's setup again, and the next boot retries the commit (at most three
# boots, then the failure is recorded and the enabled build is installed again); 1 otherwise.
root=${BRICKOS_ROOT:-}
sd=$root/mnt/SDCARD
state=$sd/BrickOS/updates
runtime=$sd/BrickOS/settings/Updates
build=$(cat "$state/pending" 2>/dev/null)
case "$build" in ''|*[!0-9a-f]*) exit 0 ;; esac
[ "${#build}" = 12 ] || exit 0
[ -f "$sd/BrickOS/enabled" ] || exit 0
# The update belongs to the build that was enabled when it was prepared: a package installed by
# hand since then wins, and the update is dropped.
from=$(cat "$state/pending-from" 2>/dev/null)
current=$(head -n 1 "$sd/BrickOS/enabled" 2>/dev/null | tr -cd '0-9a-f')
if [ -n "$from" ] && [ "$from" != "$current" ]; then
    "$runtime/brickos-update" discard
    rm -f "$state/commit-attempts"
    exit 1
fi
"$runtime/brickos-update" verify "$build" || {
    "$runtime/brickos-update" failed
    rm -f "$state/commit-attempts"
    exit 1
}
package=$state/packages/$build/BrickOS/package/$build
out=$root/tmp/brickos-update-install.log
sh "$package/setup.sh" install --boot > "$out" 2>&1
status=$?
result=$(sed -n 's/^result=//p' "$out" | tail -n 1)
if [ "$status" = 0 ]; then
    case "$result" in
        installed|updated|up-to-date)
            # Setup's transaction has completed. Its new updater can now commit the verified
            # bootstrap and enabled marker. On failure pending remains for the next boot.
            if "$runtime/brickos-update" commit "$build"; then
                rm -f "$state/commit-attempts"
                exit 0
            fi
            attempts=$(cat "$state/commit-attempts" 2>/dev/null | tr -cd '0-9')
            attempts=$((${attempts:-0} + 1))
            if [ "$attempts" -lt 3 ]; then
                echo "$attempts" > "$state/commit-attempts"
                exit 3
            fi
            rm -f "$state/commit-attempts"
            "$runtime/brickos-update" failed
            exit 1 ;;
    esac
fi
# The setup restores its snapshot on failure. Leave the previous enabled build selected and
# stop automatic retries; the user can check the log and confirm another attempt.
"$runtime/brickos-update" failed
rm -f "$state/commit-attempts"
exit 1
