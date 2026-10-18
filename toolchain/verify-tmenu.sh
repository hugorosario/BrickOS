#!/bin/sh
# Checks the in-game menu library: an AArch64 shared object whose only exported
# definition is ShowMenu, besides the linker's _init and _fini, needing only
# libraries RetroArch has already loaded.
set -eu
lib=${1:-bin/tmenu/libtmenu.so}
sh toolchain/verify-elf.sh "$lib"
readelf -h "$lib" | grep -q 'Type:.*DYN' || { echo "Not a shared object: $lib" >&2; exit 1; }
exports=$(readelf -W --dyn-syms "$lib" | awk '$7 != "UND" && ($5 == "GLOBAL" || $5 == "WEAK") && ($4 == "FUNC" || $4 == "OBJECT") {print $8}' | grep -v -x -e _init -e _fini | sort)
[ "$exports" = ShowMenu ] || { echo "Unexpected exports: $exports" >&2; exit 1; }
for library in $(readelf -d "$lib" | sed -n 's/.*Shared library: \[\([^]]*\)\].*/\1/p'); do
    case "$library" in
    libSDL2-2.0.so.0|libSDL2_ttf-2.0.so.0|libSDL2_image-2.0.so.0|libdl.so.2|libpthread.so.0|libc.so.6) ;;
    *) echo "Dependency RetroArch does not load: $library" >&2; exit 1 ;;
    esac
done
echo "Verified exports: $exports"
