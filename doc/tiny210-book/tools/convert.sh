#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname "$0")/../../.." && pwd)
OUT="$ROOT/doc/tiny210-book/chapters"
mkdir -p "$OUT"

convert_md() {
  src=$1
  dst=$2
  prefix=$3
  # 章节编号由 LaTeX 统一生成；ASCII 树形字符避免等宽字体缺字。
  sed -E \
    -e 's/^(#{2,6}) [0-9]+(\.[0-9]+)*\.? /\1 /' \
    -e 's/├──/|--/g' \
    -e 's/└──/`--/g' \
    -e 's/│/|/g' \
    "$src" | DIAGRAM_PREFIX="$prefix" pandoc --from=gfm --to=latex --top-level-division=chapter \
      --listings --lua-filter "$ROOT/doc/tiny210-book/tools/mermaid-to-tikz.lua" \
      -o "$dst"

  # Pandoc 的自然宽度 l/l/l 表格对长路径不友好；改成可自动换行的固定比例列。
  sed \
    -e 's/@{}ll@{}/@{}p{0.34\\linewidth}p{0.60\\linewidth}@{}/g' \
    -e 's/@{}lll@{}/@{}p{0.18\\linewidth}p{0.26\\linewidth}p{0.50\\linewidth}@{}/g' \
    -e 's/@{}llll@{}/@{}p{0.15\\linewidth}p{0.20\\linewidth}p{0.20\\linewidth}p{0.35\\linewidth}@{}/g' \
    "$dst" > "$dst.tmp"
  mv "$dst.tmp" "$dst"
}

convert_md "$ROOT/doc/readme/tiny210/README.md" "$OUT/01-overview.tex" overview
convert_md "$ROOT/doc/readme/tiny210/boot.md" "$OUT/02-boot.tex" boot
convert_md "$ROOT/doc/readme/tiny210/interrupts.md" "$OUT/03-interrupts.tex" interrupts
convert_md "$ROOT/doc/readme/tiny210/lcd.md" "$OUT/04-lcd.tex" lcd
convert_md "$ROOT/doc/readme/tiny210/wifi.md" "$OUT/05-wifi.tex" wifi
convert_md "$ROOT/doc/readme/tiny210/filesystem.md" "$OUT/06-filesystem.tex" filesystem
convert_md "$ROOT/doc/readme/tiny210/drivers.md" "$OUT/07-drivers.tex" drivers
convert_md "$ROOT/doc/readme/tiny210/bringup-notes.md" "$OUT/08-bringup.tex" bringup
