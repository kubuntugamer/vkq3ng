#!/bin/sh
cd "$(dirname "$0")" || exit 1
exec ./bin/quake3 \
    +set fs_basePath "$PWD" \
    +set fs_homePath "${Q3_HOME:-$HOME/.q3a}" \
    +set vm_game 0 \
    +set vm_cgame 0 \
    +set vm_ui 0 \
    "$@"
