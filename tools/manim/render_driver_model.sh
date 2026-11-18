#!/bin/sh
# Render the driver-model animation and join the four scenes:
#   tools/manim/render_driver_model.sh [quality: l|m|h]
#     -> doc/readme/tiny210/videos/driver-model-anim.mp4
# Fonts: ANIM_CJK / ANIM_MONO (default Hiragino Sans GB / Menlo on macOS).
set -e
cd "$(dirname "$0")/../.."
Q=${1:-m}
MEDIA=${MEDIA_DIR:-build/manim}
SCENES="BusInit PlatformMatch UsbMatch DataFlow"
manim -q$Q --media_dir "$MEDIA" tools/manim/driver_model_anim.py $SCENES
dir=$(ls -d "$MEDIA"/videos/driver_model_anim/*/ | head -1)
case $dir in /*) ;; *) dir=$PWD/$dir ;; esac
mkdir -p doc/readme/tiny210/videos
list="$MEDIA/driver-concat.txt"
: > "$list"
for s in $SCENES; do
  echo "file '$dir$s.mp4'" >> "$list"
done
ffmpeg -v error -y -f concat -safe 0 -i "$list" -c copy doc/readme/tiny210/videos/driver-model-anim.mp4
ls -la doc/readme/tiny210/videos/driver-model-anim.mp4
