#!/bin/sh
cd "$(dirname "$0")" || exit 1
exec ./bin/quake3 \
    +set fs_basePath "$PWD" \
    +set fs_homePath "${Q3_HOME:-$HOME/.q3a}" \
    "$@"
