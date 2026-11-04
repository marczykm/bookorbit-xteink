#!/bin/sh
# usage: tools/release.sh   (publishes v<FW_VERSION from platformio.ini> to GitHub: the full flash image and the OTA update image)
set -e
cd "$(dirname "$0")/.."
V=$(sed -n 's/.*FW_VERSION="\([0-9.]*\)".*/\1/p' platformio.ini | head -1)
[ -n "$V" ] || { echo "FW_VERSION not found in platformio.ini"; exit 1; }
[ -z "$(git status --porcelain)" ] || { echo "commit your changes first"; exit 1; }
.venv/bin/pio run -e xteink_x4
.venv/bin/python tools/make_fontpack.py
B=.pio/build/xteink_x4
BA=$(find ~/.platformio -name boot_app0.bin | head -1)
OUT=build/release
mkdir -p "$OUT"
~/.platformio/penv/bin/esptool --chip esp32c3 merge-bin -o "$OUT/bookorbit-x4-$V.bin" --flash-mode dio --flash-size 16MB \
  0x0 $B/bootloader.bin 0x8000 $B/partitions.bin 0xe000 "$BA" 0x10000 $B/firmware.bin 0x710000 build/fontpack.bin
cp $B/firmware.bin "$OUT/bookorbit-x4-$V-ota.bin"
git push origin main
git tag "v$V"
git push origin "v$V"
gh release create "v$V" "$OUT/bookorbit-x4-$V.bin" "$OUT/bookorbit-x4-$V-ota.bin" --title "BookOrbit for Xteink X4 $V" \
  --notes "Flash the full image over USB the first time (esptool --chip esp32c3 write-flash 0x0 bookorbit-x4-$V.bin); later updates come from Settings > Update on the device."
