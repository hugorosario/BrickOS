#!/bin/sh
# BrickOS updater settings
case "${1:-}" in
    status) exec ./brickos-update status ;;
    auto-status) ./brickos-update status | sed -n 's/{{update_auto=\([01]\)}}/{{state=\1}}/p' ;;
    auto) exec ./brickos-update auto "${2:-}" ;;
    *) exit 2 ;;
esac
