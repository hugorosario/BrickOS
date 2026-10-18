#!/bin/sh
# Generated from the upstream recipe for the ten release artifacts only.
# Compiler flags and source lists follow upstream; diagnostic binaries are excluded.
set -eu
: "${SYSROOT:?SYSROOT must be set by the toolchain image}"
mkdir -p bin/tmenu bin/gameswitcher

versions_hash=$(sha256sum versions.json | cut -d ' ' -f 1)
grep -qx "#define BRICKOS_VERSIONS_SOURCE_SHA256 \"$versions_hash\"" src/common/versions.generated.h || {
    echo 'Version header is stale: run node tools/component-versions.mjs generate' >&2
    exit 1
}

CC=aarch64-linux-gnu-gcc
COMMON="--sysroot=${SYSROOT} -std=c11 -O2 -Wall -Wextra -Werror -I${SYSROOT}/usr/include/SDL2 -L${SYSROOT}/usr/lib"
MAINUI_CORE="src/mainui/frontend.c src/inputd/config.c src/inputd/profile.c src/ledd/config.c src/mainui/extensions.c src/mainui/appearance.c src/mainui/appearance_fonts.c src/mainui/extension_runner.c src/mainui/json_tree.c src/mainui/boot_visuals.c src/mainui/visual_files.c src/mainui/loading_screens.c src/mainui/brickos_prefs.c src/mainui/brickos_return.c src/mainui/model.c src/mainui/json.c src/mainui/lists.c src/mainui/launch.c      src/mainui/settings.c src/mainui/actions.c src/mainui/catalog.c src/mainui/catalog-xml.c src/mainui/assets.c src/mainui/effects.c      src/mainui/network.c src/mainui/sound.c src/mainui/names.c src/mainui/presentation.c"

"$CC" $COMMON -D_REENTRANT -o bin/trimui_osdd      src/osdd/main.c src/osdd/daemon.c src/osdd/power.c src/osdd/toast.c src/osdd/state.c      src/osdd/render.c src/osdd/display.c src/osdd/music.c src/osdd/panel.c      src/osdd/input.c src/osdd/panel_render.c src/osdd/widgets.c      src/osdd/style.c src/osdd/profile.c src/osdd/theme.c src/mainui/appearance.c      src/tmenu/presentation.c src/mainui/brickos_prefs.c src/mainui/json.c      -lSDL2_image -lSDL2_ttf -lSDL2 -ldl -lpthread -lm

"$CC" $COMMON -D_REENTRANT -DBRICKOS_GAMESWITCHER_POWER -o bin/trimui_inputd src/inputd/main.c src/inputd/engine.c      src/inputd/config.c src/inputd/profile.c src/inputd/power.c src/inputd/power_source.c src/inputd/gpio.c      src/inputd/uinput.c src/gameswitcher/power_control.c src/gameswitcher/session.c src/gameswitcher/channel.c      src/gameswitcher/store.c src/mainui/json.c -ldl

"$CC" $COMMON -o bin/brickos-ledd src/ledd/main.c src/ledd/daemon.c src/ledd/reconcile.c src/ledd/driver.c      src/ledd/sensors.c src/ledd/policy.c src/ledd/config.c src/mainui/json.c

"$CC" $COMMON -D_REENTRANT -ffunction-sections -fdata-sections -Wl,--gc-sections -o bin/brickos-uninstall      src/uninstall/main.c src/uninstall/model.c src/uninstall/stage.c src/uninstall/render.c      src/mainui/assets.c src/mainui/json.c -lSDL2_ttf -lSDL2 -ldl -lpthread -lm

"$CC" $COMMON -D_REENTRANT -o bin/MainUI src/mainui/main.c src/mainui/input.c src/mainui/render.c src/mainui/device.c src/mainui/are.c src/mainui/are-bus.c src/mainui/are-device.c src/mainui/markers.c src/mainui/audio.c src/mainui/music_picture.c      $MAINUI_CORE -lSDL2_mixer -lSDL2_image -lSDL2_ttf -lSDL2 -ltinyalsa -ldl -lpthread -lm

"$CC" $COMMON -D_REENTRANT -shared -fPIC -fvisibility=hidden -o bin/tmenu/libtmenu.so      src/tmenu/showmenu.c src/tmenu/host.c src/tmenu/menu.c src/tmenu/input.c src/tmenu/render.c      src/tmenu/settings.c src/tmenu/system.c src/tmenu/json.c src/tmenu/theme.c src/osdd/profile.c      src/tmenu/material.c src/tmenu/presentation.c src/mainui/brickos_prefs.c src/mainui/json.c      -lSDL2_image -lSDL2_ttf -lSDL2 -ldl -lpthread

"$CC" $COMMON -o bin/gameswitcher/GameSwitcher src/gameswitcher/main.c src/gameswitcher/store.c      src/gameswitcher/model.c src/gameswitcher/motion.c src/gameswitcher/render.c src/gameswitcher/platform.c      src/gameswitcher/relay.c src/gameswitcher/channel.c src/gameswitcher/policy.c src/mainui/appearance.c      src/mainui/json.c src/mainui/settings.c src/mainui/model.c src/osdd/display.c -lSDL2_ttf -lSDL2 -lm

"$CC" $COMMON -shared -fPIC -fvisibility=hidden -DBRICKOS_GAMESWITCHER -o bin/gameswitcher/libtmenu.so      src/gameswitcher/observer.c src/gameswitcher/bridge.c src/gameswitcher/standby.c src/gameswitcher/session.c      src/gameswitcher/channel.c src/gameswitcher/checkpoint.c src/gameswitcher/store.c src/gameswitcher/policy.c      src/mainui/json.c src/gameswitcher/arbiter.c src/gameswitcher/gesture.c src/gameswitcher/gesture_reader.c      src/gameswitcher/app_process.c src/gameswitcher/guard.c src/gameswitcher/relay.c src/osdd/display.c src/tmenu/showmenu.c      src/tmenu/host.c src/tmenu/menu.c src/tmenu/input.c src/tmenu/render.c src/tmenu/settings.c src/tmenu/system.c      src/tmenu/json.c src/tmenu/theme.c src/osdd/profile.c src/tmenu/material.c src/tmenu/presentation.c      src/mainui/brickos_prefs.c -lSDL2_image -lSDL2_ttf -lSDL2 -ldl -lpthread

"$CC" $COMMON -o bin/gameswitcher/gs-session src/gameswitcher/supervisor.c src/gameswitcher/app_process.c      src/gameswitcher/channel.c src/gameswitcher/session.c src/gameswitcher/power_control.c src/gameswitcher/standby.c      src/gameswitcher/store.c src/gameswitcher/policy.c src/gameswitcher/clock.c src/gameswitcher/lifecycle.c      src/gameswitcher/guard.c src/gameswitcher/relay.c src/mainui/json.c src/osdd/display.c -ldl

"$CC" $COMMON -o bin/brickos-update src/update/main.c src/update/files.c src/update/package.c src/update/release.c src/update/versions.c src/mainui/json_tree.c -lz

sh toolchain/verify-tmenu.sh bin/tmenu/libtmenu.so
gameswitcher_exports=$(readelf -W --dyn-syms bin/gameswitcher/libtmenu.so | \
    awk '$7 != "UND" && ($5 == "GLOBAL" || $5 == "WEAK") && ($4 == "FUNC" || $4 == "OBJECT") {print $8}' | \
    grep -v -x -e _init -e _fini | sort)
[ "$gameswitcher_exports" = "$(printf 'ShowMenu\ndlsym')" ] ||
    { echo "GameSwitcher: unexpected session library exports: $gameswitcher_exports" >&2; exit 1; }
sh toolchain/verify-elf.sh bin/trimui_osdd bin/trimui_inputd bin/brickos-ledd bin/brickos-uninstall bin/MainUI bin/tmenu/libtmenu.so bin/gameswitcher/GameSwitcher bin/gameswitcher/libtmenu.so bin/gameswitcher/gs-session bin/brickos-update
