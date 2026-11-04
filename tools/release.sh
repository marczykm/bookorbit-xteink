#!/bin/sh
# usage: tools/release.sh   (publishes v<FW_VERSION from platformio.ini> to GitHub for the Xteink X4 and X3)
# One firmware runs on both (the device is detected at boot); each gets its own files:
#   bookorbit-<device>-<version>.hex      first install over USB: bootloader, partition table, app and fonts. Intel HEX
#                                         leaves the gaps alone, so the NVS partition (the device's factory data) is kept.
#   bookorbit-<device>-<version>-ota.bin  the app alone, for Settings > Update on the device
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
for D in x4 x3; do
  ~/.platformio/penv/bin/esptool --chip esp32c3 merge-bin --format hex -o "$OUT/bookorbit-$D-$V.hex" \
    0x0 $B/bootloader.bin 0x8000 $B/partitions.bin 0xe000 "$BA" 0x10000 $B/firmware.bin 0x710000 build/fontpack.bin
  cp $B/firmware.bin "$OUT/bookorbit-$D-$V-ota.bin"
done
[ "${PUBLISH:-1}" = 1 ] || exit 0  # PUBLISH=0 tools/release.sh only builds the files
git push origin main
git tag "v$V"
git push origin "v$V"
gh release create "v$V" "$OUT"/bookorbit-x4-$V.hex "$OUT"/bookorbit-x4-$V-ota.bin "$OUT"/bookorbit-x3-$V.hex "$OUT"/bookorbit-x3-$V-ota.bin \
  --title "BookOrbit for Xteink X4 and X3 $V" --notes "One firmware for the Xteink X4 and X3; the device is detected at boot.

First install over USB (back up the stock firmware first): esptool --chip esp32c3 write-flash 0x0 bookorbit-<x4|x3>-$V.hex
The HEX image keeps the NVS partition with the device's factory data. After that, updates come from Settings > Update on the device."
