# bookorbit-xteink

Custom firmware for the Xteink X4 that reads books from a self-hosted BookOrbit server
(companion to `../bookorbit-android`). Built on the [FreeInk SDK](freeink-sdk/) (MIT, vendored from the
CrossPoint reader repo, commit 2ec01c9): display driver, input, battery, SD, EPUB engine.

## Status
- Library browser: Wi-Fi, sign-in, paged book list, download to `/BookOrbit/` (`http://` or `https://`).
- EPUB reader: FreeInkBook layout cached on the SD card (`/BookOrbit/.cache/`), page turns, resume position, images.
- Opening a book restarts into a Wi-Fi-free reading mode (the original X4 has no PSRAM; the reader needs ~170 KB of heap).
- Fonts: Noto Serif (regular, bold, italic; Latin + Latin Extended, so Polish works) from a flash partition. The library screens use the same Noto Serif regular face, so Polish titles show correctly (they fall back to an ASCII-only bitmap font if the font pack is missing).
- Instant start: at power-on and when returning from a book, the cached library page appears at once ("Connecting..." at the bottom) while Wi-Fi, sign-in, progress sync and the list refresh finish in the background; buttons keep working meanwhile.
- Header: a battery icon (filled to the charge level) replaces the percentage.
- Header dot: a white dot left of the Wi-Fi icon shows while the device is contacting the BookOrbit server (background sign-in and refresh, page changes, downloads).
- Header Wi-Fi icon: arcs show signal strength when online, just a dot while connecting, the full icon in grey with a white slash when offline.
- Covers: the library shows 8 books per page with a 48x72 cover. Thumbnails (`/books/{id}/thumbnail`, PNG or baseline JPEG) are fetched one at a time while the library is idle and online, shrunk to a dithered 1-bit image and cached in `/BookOrbit/.covers/` (432 bytes each). Books whose cover is missing or in an unsupported format (progressive JPEG, WebP) just show an empty frame.
- Offline library: every library page you load is cached in `/BookOrbit/.library/`. If Wi-Fi or the server is unavailable at start (or drops later), the cached list is shown with an "Offline" note; downloaded books (marked with a square) can be read, the rest say "Not downloaded". Holding Back switches Wi-Fi on or off; the choice is remembered (`/BookOrbit/.wifi_off`) across restarts, book opens and sleep.
- Sleep: a click on Power, or 5 minutes without a button press (library or reader), shows a "BookOrbit" sleep screen and the device deep-sleeps (the panel keeps the image). Power wakes it; an open book is saved and reopened on wake. Deep sleep drops the USB serial port, so Power is needed before flashing again.
- Reading statistics: while reading, a session (start/end time, active seconds, progress change) is saved to `/BookOrbit/.sessions` every 10 page turns, on Back and before sleep, and uploaded to `/books/files/{id}/sessions` at the next library sign-in. Progress is checkpointed to the outbox at the same moments. Time comes from NTP when the library is online; without a valid clock (never online since power-on) a session is not recorded. Pauses over 2 minutes count as 2 minutes. The session `source` is "android" (what the mock and Android client use); change `kSessionSource` in `src/main.cpp` if your server wants another value.
- Progress sync: closing a book (Back) queues its position in `/BookOrbit/.progress`; the library posts it to `/books/files/{id}/progress` after sign-in, retrying later if offline.
- Resume: opening a book fetches the server position and jumps to it when it is more than 1% ahead of the saved local one.
- HTTPS: certificates are checked against the built-in CA bundle (the clock is set over NTP first). For a self-signed server add `"insecureTls": true` to `/bookorbit.json`, which skips the check.
- Token refresh: a 401 triggers one `/auth/refresh` and a retry of the request.
- Not done: bold-italic face (no RAM for its glyph cache; skipped on purpose).

## Setup
Settings live in `/bookorbit.json` on the SD card (see `config.example.json`). To write them without a card reader:

    .venv/bin/python tools/provision.py PORT SSID WIFI_PASSWORD SERVER USERNAME PASSWORD

## Build / flash
Flash layout is in `partitions.csv` (7 MB app, 1 MB `fonts` partition). After flashing the firmware once, write the fonts:

    tools/flash_fonts.sh /dev/cu.usbmodemXXXX

Needs Python 3.10-3.14 (3.15 is unsupported by PlatformIO).

    python3.14 -m venv .venv && .venv/bin/pip install platformio pillow
    .venv/bin/pio run -e xteink_x4 -t upload --upload-port /dev/cu.usbmodemXXXX

Back up the stock firmware first: `esptool read_flash 0 0x1000000 backup/stock-x4-16MB.bin`.
Environment `xteink_x4c` is for the ESP32-S3 X4 Classic (untested).

## Testing without touching the device
`tools/drive.py PORT OUTDIR STEP...` resets the device, injects button presses over serial (`o` OK, `d`/`u` down/up,
`n`/`p` next/previous page, `b` back) and saves screenshots (`shot`) as PNG. `OPEN:/path.epub` opens any EPUB on the card.
`tools/mock_server` is `../bookorbit-android/tools/mock_server.py` (port 3000, demo/demo).

## Third-party notes
- Noto Serif (fonts/): SIL Open Font License 1.1, subset for size (see fonts/README.md).
- `freeink-sdk/libs/font/FreeInkFont/third_party/stb/stb_truetype.h` is patched locally: smaller rasterizer heap chunks so glyphs
  render in the original X4's fragmented 380 KB RAM.
- Reader RAM budget (src/reader.cpp): 96 KB layout scratch, 16 KB book, 30 KB glyph caches. The cover image of a large book can use almost all scratch.
