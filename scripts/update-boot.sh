#!/bin/sh
# BrickOS updater boot activation. Called before the regular package setup, never at runtime.
# A pending package stays in its own SD-card staging tree: its ../../../System manifest entry
# still resolves to that tree's bootstrap, so verification never depends on overwriting the
# active bootstrap first. Pending is removed last, making an interrupted commit retryable.
root=${BRICKOS_ROOT:-}
sd=$root/mnt/SDCARD
state=$sd/BrickOS/updates
runtime=$sd/BrickOS/settings/Updates
build=$(cat "$state/pending" 2>/dev/null)
case "$build" in ''|*[!0-9a-f]*) exit 0 ;; esac
[ "${#build}" = 12 ] || exit 0
[ -f "$sd/BrickOS/enabled" ] || exit 0
"$runtime/brickos-update" verify "$build" || {
    "$runtime/brickos-update" failed
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
            "$runtime/brickos-update" commit "$build"
            exit $? ;;
    esac
fi
# The setup restores its snapshot on failure. Leave the previous enabled build selected and
# stop automatic retries; the user can check the log and confirm another attempt.
"$runtime/brickos-update" failed
exit 1
