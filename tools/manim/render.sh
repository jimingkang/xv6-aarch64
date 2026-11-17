#!/bin/sh
# Render the memory animation (720p30) and join the three scenes:
#   tools/manim/render.sh [quality: l|m|h]  -> doc/readme/tiny210/videos/memory-anim.mp4
set -e
cd "$(dirname "$0")/../.."
Q=${1:-m}
MEDIA=${MEDIA_DIR:-build/manim}
manim -q$Q --media_dir "$MEDIA" tools/manim/memory_anim.py MemoryInit MmuSetup ProcessExec
dir=$(ls -d "$MEDIA"/videos/memory_anim/*/ | head -1)
mkdir -p doc/readme/tiny210/videos
list="$MEDIA/concat.txt"
: > "$list"
for s in MemoryInit MmuSetup ProcessExec; do
  echo "file '$PWD/$dir/$s.mp4'" >> "$list"
done
ffmpeg -v error -y -f concat -safe 0 -i "$list" -c copy doc/readme/tiny210/videos/memory-anim.mp4
ls -la doc/readme/tiny210/videos
