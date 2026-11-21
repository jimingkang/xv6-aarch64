#!/bin/sh
# Render the SDIO Wi-Fi animation from a WIFI_TRACE=1 log and join the scenes:
#   tools/sdio_wifi_diagrams/manim/render_sdio_wifi.sh [quality: l|m|h] [log]
#     -> doc/readme/rpi3/videos/sdio-wifi-anim.mp4
# Default log: tools/sdio_wifi_diagrams/manim/sample-wifi-trace.log (示例，非实机).
# Fonts: ANIM_CJK / ANIM_MONO (default PingFang SC / Menlo on macOS).
set -e
cd "$(dirname "$0")/../../.."
Q=${1:-m}
if [ -n "$2" ]; then SDIO_WIFI_LOG=$(cd "$(dirname "$2")" && pwd)/$(basename "$2"); export SDIO_WIFI_LOG; fi
MEDIA=${MEDIA_DIR:-build/manim}
SCENES="Bringup Join DataPath"
manim -q$Q --media_dir "$MEDIA" tools/sdio_wifi_diagrams/manim/sdio_wifi_anim.py $SCENES
dir=$(ls -d "$MEDIA"/videos/sdio_wifi_anim/*/ | head -1)
case $dir in /*) ;; *) dir=$PWD/$dir ;; esac
mkdir -p doc/readme/rpi3/videos
list="$MEDIA/sdio-wifi-concat.txt"
: > "$list"
for s in $SCENES; do
  echo "file '$dir$s.mp4'" >> "$list"
done
ffmpeg -v error -y -f concat -safe 0 -i "$list" -c copy doc/readme/rpi3/videos/sdio-wifi-anim.mp4
ls -la doc/readme/rpi3/videos/sdio-wifi-anim.mp4
