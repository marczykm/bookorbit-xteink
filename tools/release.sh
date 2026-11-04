#!/bin/sh
# usage: tools/release.sh   (publishes v<FW_VERSION from platformio.ini> to GitHub for the Xteink X4 and X3)
# Like CrossPoint: one app image for both devices (detected at boot), on the stock Xteink partition table, fonts built in.
#   bookorbit-x3-x4-<version>.bin   installs over USB at 0x10000 (after erasing otadata) and is what Settings > Update fetches
# Firmware 0.0.4-0.0.5 (its own partition layout) looks for bookorbit-<x4|x3>-<version>-ota.bin, so copies under those
# names are published too.
#   bookorbit-x4pro-<version>.bin   the Xteink X4 Pro (ESP32-S3) image, installed and updated the same way
set -e
cd "$(dirname "$0")/.."
V=$(sed -n 's/.*FW_VERSION="\([0-9.]*\)".*/\1/p' platformio.ini | head -1)
[ -n "$V" ] || { echo "FW_VERSION not found in platformio.ini"; exit 1; }
[ -z "$(git status --porcelain)" ] || { echo "commit your changes first"; exit 1; }
.venv/bin/pio run -e xteink_x4
.venv/bin/pio run -e xteink_x4pro
OUT=build/release
mkdir -p "$OUT"
cp .pio/build/xteink_x4/firmware.bin "$OUT/bookorbit-x3-x4-$V.bin"
cp "$OUT/bookorbit-x3-x4-$V.bin" "$OUT/bookorbit-x4-$V-ota.bin"
cp "$OUT/bookorbit-x3-x4-$V.bin" "$OUT/bookorbit-x3-$V-ota.bin"
cp .pio/build/xteink_x4pro/firmware.bin "$OUT/bookorbit-x4pro-$V.bin"
[ "${PUBLISH:-1}" = 1 ] || exit 0  # PUBLISH=0 tools/release.sh only builds the files
git push origin main
git tag "v$V"
git push origin "v$V"
gh release create "v$V" "$OUT/bookorbit-x3-x4-$V.bin" "$OUT/bookorbit-x4-$V-ota.bin" "$OUT/bookorbit-x3-$V-ota.bin" \
  "$OUT/bookorbit-x4pro-$V.bin" \
  --title "BookOrbit for Xteink X4, X3 and X4 Pro $V" --notes "One firmware for the Xteink X4 and X3 (the device is detected at boot), built like CrossPoint: an app image for the stock Xteink partition table, fonts included. The X4 Pro (ESP32-S3, touchscreen and frontlight) has its own image, bookorbit-x4pro-$V.bin.

Install over USB (back up the stock firmware first):
esptool --chip esp32c3 erase-region 0xe000 0x2000
esptool --chip esp32c3 write-flash 0x10000 bookorbit-x3-x4-$V.bin
The first command makes the device start from the first app slot (the old firmware may have been running from the second);
nothing else outside the app is touched, so this works from the stock firmware or CrossPoint and keeps the factory data. Later updates come from Settings > Update on the device.

The -ota.bin copies are for devices on BookOrbit 0.0.4-0.0.5.

Xteink X4 Pro (not yet tested on the device):
esptool --chip esp32s3 erase-region 0xe000 0x2000
esptool --chip esp32s3 write-flash 0x10000 bookorbit-x4pro-$V.bin
Touch: tap rows, keys and menus; swipe to turn pages; on a book page tap the left or right edge to turn pages and the middle for the menu. The Home key is Back; hold it for the light (brightness and warmth)."
