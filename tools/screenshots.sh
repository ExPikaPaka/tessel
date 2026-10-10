#!/bin/sh
# Renders the application window to PNG files without needing a display.
#
#   tools/screenshots.sh [binary] [output directory]
set -e

binary=${1:-build/tessel}
outdir=${2:-screenshots}
mkdir -p "$outdir"

sample="$outdir/.sample.png"
python3 "$(dirname "$0")/make_sample.py" "$sample" >/dev/null

export QT_QPA_PLATFORM=offscreen

"$binary" "$sample" --screenshot "$outdir/main-dark.png" --screenshot-delay 1200
"$binary" --light "$sample" --screenshot "$outdir/main-light.png" --screenshot-delay 1200

rm -f "$sample"
ls -l "$outdir"
