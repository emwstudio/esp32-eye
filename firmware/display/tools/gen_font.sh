#!/usr/bin/env bash
# 生成 LVGL 中文字体（供 lv_font_load 从 LittleFS 加载）。
# 用法: tools/gen_font.sh [覆盖字符集文件] && pio run --target uploadfs
# 需要本机 node: 自动通过 npx 调用 lv_font_conv。
set -e
cd "$(dirname "$0")/.."

FONT_FILE="${FONT_FILE:-/System/Library/Fonts/Hiragino Sans GB.ttc}"
CHARS_FILE="${1:-tools/cjk_chars.txt}"
SIZE=20

if [ ! -f "$CHARS_FILE" ]; then
  echo "缺少字符集文件 $CHARS_FILE" >&2
  exit 1
fi

SYMBOLS=$(cat "$CHARS_FILE" | tr -d '\n')
mkdir -p data/font
npx -y lv_font_conv \
  --font "$FONT_FILE" \
  --size $SIZE --bpp 4 --format bin \
  --symbols "$SYMBOLS" \
  -o data/font/cjk20.bin
echo "字体已生成: data/font/cjk20.bin，执行 pio run --target uploadfs 烧录"
