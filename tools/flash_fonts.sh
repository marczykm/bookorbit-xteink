#!/bin/sh
# usage: tools/flash_fonts.sh PORT   (partition offset must match partitions.csv)
cd "$(dirname "$0")/.." || exit 1
.venv/bin/python tools/make_fontpack.py && \
~/.platformio/penv/bin/esptool --port "$1" write-flash 0x710000 build/fontpack.bin
