#!/bin/sh
# Render the rpi3-lock memory animation and join the three scenes:
#   tools/manim/render_rpi3.sh [quality: l|m|h]  -> doc/readme/rpi3/videos/memory-anim.mp4
set -e
cd "$(dirname "$0")/../.."
Q=${1:-m}
MEDIA=${MEDIA_DIR:-build/manim}
manim -q$Q --media_dir "$MEDIA" tools/manim/rpi3_memory_anim.py MemoryInit MmuSetup ProcessExec
dir=$(ls -d "$MEDIA"/videos/rpi3_memory_anim/*/ | head -1)
mkdir -p doc/readme/rpi3/videos
list="$MEDIA/rpi3-concat.txt"
: > "$list"
for s in MemoryInit MmuSetup ProcessExec; do
  echo "file '$PWD/$dir/$s.mp4'" >> "$list"
done
ffmpeg -v error -y -f concat -safe 0 -i "$list" -c copy doc/readme/rpi3/videos/memory-anim.mp4
ls -la doc/readme/rpi3/videos
