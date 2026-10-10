#!/bin/sh
# Builds Tessel if needed and runs it, passing any arguments through.
set -e
root=$(cd "$(dirname "$0")" && pwd)
build="$root/build"
if [ ! -f "$build/build.ninja" ] && [ ! -f "$build/Makefile" ]; then
    cmake -S "$root" -B "$build" -DCMAKE_BUILD_TYPE=Release
fi
cmake --build "$build" --parallel
exec "$build/tessel" "$@"
