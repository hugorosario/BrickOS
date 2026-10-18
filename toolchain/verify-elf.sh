#!/bin/sh
set -eu

: "${SYSROOT:?SYSROOT must be set by the toolchain image}"
test "$#" -gt 0 || { echo 'usage: verify-elf.sh FILE...' >&2; exit 2; }

for binary do
    test -s "$binary" || { echo "Missing or empty artifact: $binary" >&2; exit 1; }
    header=$(readelf -h "$binary") || { echo "Not an ELF artifact: $binary" >&2; exit 1; }
    printf '%s\n' "$header" | grep -q 'Class:.*ELF64' || {
        echo "Not ELF64: $binary" >&2; exit 1;
    }
    printf '%s\n' "$header" | grep -q 'Machine:.*AArch64' || {
        echo "Not AArch64: $binary" >&2; exit 1;
    }
    needed=$(readelf -d "$binary" 2>/dev/null | sed -n 's/.*Shared library: \[\([^]]*\)\].*/\1/p')
    for library in $needed; do
        case "$library" in
            */*|*..*) echo "Unsafe dynamic dependency: $library" >&2; exit 1 ;;
        esac
        if ! find "${SYSROOT}/lib" "${SYSROOT}/usr/lib" -name "$library" -print -quit | grep -q .; then
            echo "Target dependency missing from sysroot: $library" >&2
            exit 1
        fi
    done
    echo "Verified $(file -b "$binary")"
    echo "NEEDED: ${needed:-none}"
done
