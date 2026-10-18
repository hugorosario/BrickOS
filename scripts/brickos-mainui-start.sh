#!/bin/sh
# BrickOS frontend start script
# Installed by scripts/mainui-install.sh and removed by its rollback. The firmware runs
# /mnt/SDCARD/System/starts/*.sh once per boot before its main loop. This script binds the
# official MainUI to a private path, then binds the BrickOS wrapper over
# /usr/trimui/bin/MainUI, so the loop's `MainUI` runs the wrapper, which falls back to the
# official frontend through the private path. Nothing under /usr is written; the binds last
# until the next boot.
# Creating @INSTALL_DIR@/DISABLED turns BrickOS off for the next boot.
dir=@INSTALL_DIR@
official=@OFFICIAL@
private=@PRIVATE@
mounts=@MOUNTS@
log=@BOOT_LOG@
note() { echo "$(cut -d ' ' -f 1 /proc/uptime) mainui-start: $*" >> "$log"; }
hash_of() { sha256sum "$1" 2>/dev/null | cut -d ' ' -f 1; }
# The entry that makes @SD_TREE@ an active SD tree, if any: anything other than a res
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

[ -f "$dir/DISABLED" ] && { note "DISABLED latch set"; exit 0; }
# An active SD trimui tree switches the loop to its SD branch, which would not run this MainUI.
entry=$(sd_tree_entry "@SD_TREE@") && { note "refused: @SD_TREE@ holds $entry"; exit 0; }
grep -q " $official " "$mounts" 2>/dev/null && { note "refused: something is mounted over $official"; exit 0; }
# The files must match what the install recorded: "version elf-hash wrapper-hash".
read -r version elf_hash wrapper_hash presentation < "$dir/current" 2>/dev/null
presentation=${presentation:-stock}
case "$presentation" in
    stock) ;;
    material-you) ;;
    *) note "refused: invalid presentation manifest"; exit 0 ;;
esac
elf=$dir/$version/MainUI
if [ -z "$version" ] || [ "$(hash_of "$elf")" != "$elf_hash" ] || [ "$(hash_of "$dir/wrapper.sh")" != "$wrapper_hash" ]; then
    note "refused: the installed files do not match their record"
    exit 0
fi
[ -f "$official" ] || { note "refused: $official is missing"; exit 0; }
mkdir -p "$(dirname "$private")" && : > "$private" || { note "refused: cannot create $private"; exit 0; }
if ! mount --bind "$official" "$private"; then
    note "refused: cannot bind $official to $private"
    exit 0
fi
if ! mount --bind "$dir/wrapper.sh" "$official"; then
    umount "$private" 2>/dev/null
    note "refused: cannot bind the wrapper over $official"
    exit 0
fi
note "active: version $version"
exit 0
