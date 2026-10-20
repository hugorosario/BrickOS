#!/bin/sh
# BrickOS GameSwitcher settings
# Settings > GameSwitcher's rows. The release package's setup installs this folder with
# GameSwitcher; it never writes OFF, excluded-systems or settings.json, so the choices survive
# updates.
#   status | set 0|1          the switch: set 0 creates BrickOS/gameswitcher/OFF, set 1 removes it.
#                             While OFF exists the launchers GameSwitcher took run with the BrickOS
#                             in-game menu, and MainUI hides GameSwitcher's app and desktop card.
#   get KEY | put KEY VALUE   a setting of GameSwitcher's settings.json, through gs-session
#                             (history_limit as the row's choice: no limit, 10, 20 or 50)
#   info                      the history and play time placeholders
#   clear | reset-time        clear the history or reset play time, after a confirmation
#   system-status NAME | system NAME 0|1   whether an Emus folder uses GameSwitcher (1) or is
#                             listed in excluded-systems (0)
# Texts follow BRICKOS_LANGUAGE: Portuguese for pt, English otherwise.
sd=${BRICKOS_SD_ROOT:-/mnt/SDCARD}
base=$sd/BrickOS/gameswitcher
off=$base/OFF
excluded=$base/excluded-systems
pt=no; [ "${BRICKOS_LANGUAGE:-}" = pt ] && pt=yes
say() { if [ "$pt" = yes ]; then echo "$2"; else echo "$1"; fi; }
settings() { sh "$base/entry.sh" settings "$@"; }
limits='0 10 20 50'
plain() { case "$1" in ''|*[!A-Za-z0-9._-]*|.|..) return 1 ;; esac; }

case "${1:-}" in
    status) if [ -e "$off" ]; then echo '{{state=0}}'; else echo '{{state=1}}'; fi ;;
    set)
        case "${2:-}" in
            0) mkdir -p "${off%/*}" && : > "$off" && sync ;;
            1) rm -f "$off" && sync ;;
            *) exit 2 ;;
        esac ;;
    get)
        value=$(settings get | sed -n "s/^${2:-}=//p")
        case "$value" in ''|*[!0-9]*) exit 1 ;; esac
        if [ "$2" = history_limit ]; then
            index=0
            for limit in 10 20 50; do [ "$value" -gt 0 ] && index=$((index + 1)) && [ "$value" -le "$limit" ] && break; done
            value=$index
        fi
        echo "{{state=$value}}" ;;
    put)
        key=${2:-} value=${3:-}
        case "$value" in ''|*[!0-9]*) exit 2 ;; esac
        if [ "$key" = history_limit ]; then
            # shellcheck disable=SC2086
            set -- $limits; shift "$value" 2>/dev/null || exit 2
            value=${1:-}; [ -n "$value" ] || exit 2
        fi
        output=$(settings set "$key" "$value" 2>&1) && exit 0
        case "$output" in
            refused=critical_percent) say 'The critical level must stay below the warning' 'O nível crítico tem de ficar abaixo do aviso' ;;
            refused=warning_percent) say 'The warning must stay above the critical level' 'O aviso tem de ficar acima do nível crítico' ;;
            *) say 'The setting was not saved' 'A definição não foi guardada'; echo "$output" ;;
        esac
        exit 1 ;;
    info)
        output=$(settings info) || exit 1
        games=$(echo "$output" | sed -n 's/^games=//p')
        bytes=$(echo "$output" | sed -n 's/^bytes=//p')
        play=$(echo "$output" | sed -n 's/^play_ms=//p')
        for number in "$games" "$bytes" "$play"; do case "$number" in ''|*[!0-9]*) exit 1 ;; esac; done
        space=$(awk -v b="$bytes" -v pt="$pt" 'BEGIN {
            if (b < 1048576) text = sprintf("%d KB", int((b + 1023) / 1024));
            else if (b < 1073741824) text = sprintf("%d MB", int(b / 1048576 + 0.5));
            else { text = sprintf("%.1f GB", b / 1073741824); if (pt == "yes") sub(/\./, ",", text) }
            print text }')
        minutes=$((play / 60000)) hours=$((play / 3600000))
        if [ "$hours" -gt 0 ]; then time="$hours h $((minutes % 60)) min"; else time="$minutes min"; fi
        if [ "$games" = 1 ]; then count=$(say '1 game' '1 jogo'); else count="$games $(say games jogos)"; fi
        echo "{{gs_history=$count, $space}}"
        echo "{{gs_play=$time}}" ;;
    clear)
        # One short line: the dialog's title is the row's label, and it shows no second line.
        echo "confirm:$(say 'Delete every checkpoint?' 'Apagar todos os checkpoints?')"
        read -r answer
        [ "$answer" = A ] || exit 0
        settings clear >/dev/null || { say 'The history was not cleared' 'O histórico não foi limpo'; exit 1; }
        say 'History cleared' 'Histórico limpo' ;;
    reset-time)
        echo "confirm:$(say 'Reset all play time?' 'Repor todo o tempo de jogo?')"
        read -r answer
        [ "$answer" = A ] || exit 0
        settings reset-time >/dev/null || { say 'Play time was not reset' 'O tempo de jogo não foi reposto'; exit 1; }
        say 'Play time reset' 'Tempo de jogo reposto' ;;
    system-status)
        plain "${2:-}" || exit 2
        if [ -f "$excluded" ] && grep -qxF -- "$2" "$excluded"; then echo '{{state=0}}'; else echo '{{state=1}}'; fi ;;
    system)
        plain "${2:-}" || exit 2
        case "${3:-}" in 0|1) ;; *) exit 2 ;; esac
        if [ "$3" = 0 ] && [ -f "$excluded" ] && grep -qxF -- "$2" "$excluded"; then exit 0; fi
        mkdir -p "$base" || exit 1
        { [ -f "$excluded" ] && grep -vxF -- "$2" "$excluded"; [ "$3" = 0 ] && echo "$2"; } > "$excluded.tmp"
        if [ -s "$excluded.tmp" ]; then mv "$excluded.tmp" "$excluded"; else rm -f "$excluded.tmp" "$excluded"; fi
        sync ;;
    *) exit 2 ;;
esac
