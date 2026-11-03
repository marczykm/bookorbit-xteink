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
- Header: light (dark text and icons on the page, a rule below); a battery icon (filled to the charge level) replaces the percentage.
- Header dot: a dark dot left of the Wi-Fi icon shows while the device is contacting the BookOrbit server (background sign-in and refresh, page changes, downloads).
- Header Wi-Fi icon: arcs show signal strength when online, just a dot while connecting, the full icon in grey with a solid slash when offline.
- Home: a menu (Library) above a statistics panel with the Android app's dashboard widgets (slightly rounded cards): the book you are reading now (cover, author, progress), reading streak, yearly goal, reading rhythm, on-pace projection, monthly challenge and library overview. Numbers come from `/dashboard/widgets/batch` after each sign-in and are cached in `/BookOrbit/.stats.json`, so Home shows them instantly and offline. Up/Down moves between Library and the Reading now card; OK on that card opens the book (downloading it first if needed). Back from the library returns to Home, and closing a book returns to the screen it was opened from.
- Companion: the Android app's reading-companion widget (15 characters, 6 poses, 64x64 sprites converted to 1 bit by `tools/make_pets.py` from the plugin art). The pose follows your reading as in the app: asleep until you have read today, reading, party on streak milestones, a ribbon when a book was finished today, yawning after 22:00. The companion is chosen in Settings (the card on Home is display only); the choice is saved. Add `"timezone"` (a POSIX TZ string, see `config.example.json`) to `/bookorbit.json` for the right local bedtime. The library counts (books, authors, series) now sit under "Library" in the menu row.
- Wi-Fi setup on the device: Settings > Network scans for nearby networks (strongest first, "Other network..." for a hidden one). Pick one and type the password on the on-screen keyboard: Up/Down/Left/Right move over the keys, OK types, Back deletes, and the bottom row has ABC/123 (capital for one letter, symbols), Space, Cancel and Done. Done saves the name and password into `/bookorbit.json` (other settings there are kept) and connects at once; open networks are saved without a password.
- Server and account: Settings > Server, Username and Password open the same keyboard (the server starts as `http://`; it must be `http://` or `https://`). Saving writes `/bookorbit.json` and signs in again at once; changing the server clears the cached library, covers and statistics. The password row shows asterisks, but the keyboard shows what you type.
- Keyboard style: Settings > Keyboard chooses the on-screen keyboard: the key grid, or a classic phone T9 keypad (multi-tap: press a key again within about a second for its next letter; `*` switches abc/Abc/ABC, `0` is space, `1` and `#` hold punctuation). It is multi-tap only, there is no predictive dictionary. The choice is saved in `/BookOrbit/.keyboard`.
- Settings: the gear above the fourth button on Home (Right) opens a list: Wi-Fi on/off, companion, and how long until the device sleeps (1, 2, 5, 10, 15 or 30 minutes, saved in `/BookOrbit/.sleep`), plus the server address and battery level. Up/Down select; OK toggles Wi-Fi, or opens a list for the companion and the sleep time (Up/Down choose, OK confirms, Back cancels, a tick marks the current value). Back returns to Home.
- Responsiveness: panel refreshes are asynchronous, screen changes are drawn when the panel is idle (quick presses collapse into one redraw), and a background task samples the buttons so taps are never lost while a page renders.
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
