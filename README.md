# bookorbit-xteink

Custom firmware for the Xteink X4 that reads books from a self-hosted BookOrbit server
(companion to `../bookorbit-android`). Built on the [FreeInk SDK](freeink-sdk/) (MIT, vendored from the
CrossPoint reader repo, commit 2ec01c9): display driver, input, battery, SD, EPUB engine.

## Status
- Library browser: Wi-Fi, sign-in, paged book list, download to `/BookOrbit/` (`http://` or `https://`).
- EPUB reader: FreeInkBook layout cached on the SD card (`/BookOrbit/.cache/`), page turns, resume position, images.
- Library submenu: Home > Library opens All / Authors / Series / Smart scopes. All is the paged book list; the others list authors, series or the user's saved filters (page with Left/Right), and OK shows that entry's books. Back goes up one level. Lists are cached on the SD card for offline browsing.
- Reader page: a header with the chapter's name and the battery, and a footer with a progress axis (a line for the book, a tick at the start of each chapter, a dot at the current position) with a rough time to the end of the chapter on its right (~1200 characters a minute).
- Reader menu: OK while reading opens a menu with Contents, Text size and Margins. Contents lists the book's table of contents (current entry starred); OK jumps to the start of that chapter. Text size offers 18-30 px (default 22) and re-lays out the chapter at the same place in the text; it is saved in `/BookOrbit/.fontsize` for all books and each size keeps its own page cache. Margins offers narrow/normal/wide/wider side margins (8/20/32/48 px; `/BookOrbit/.margin`), also kept for all books. Back goes up a level, then closes the menu.
- Opening a book restarts into a Wi-Fi-free reading mode (the original X4 has no PSRAM; the reader needs ~170 KB of heap).
- Fonts: Noto Serif (regular, bold, italic; Latin + Latin Extended, so Polish works) from a flash partition. The library screens use the same Noto Serif regular face, so Polish titles show correctly (they fall back to an ASCII-only bitmap font if the font pack is missing).
- Instant start: at power-on and when returning from a book, the cached library page appears at once while Wi-Fi, sign-in, progress sync and the list refresh finish in the background; buttons keep working meanwhile.
- Header: light (dark text and icons on the page, a rule below); a battery icon (filled to the charge level) replaces the percentage.
- Header dot: a dark dot to the left of the battery (and of the "not synced" icon) shows while the device is contacting the BookOrbit server (background sign-in and refresh, page changes, downloads).
- Home, third button (sync icon): sends reading progress and sessions and refreshes the statistics on demand, switching Wi-Fi on for it if needed.
- Header "not synced" icon: crossed-out sync arrows appear next to the battery while reading progress or sessions are waiting to be sent, or the last sync failed; they disappear after a successful sync.
- Wi-Fi is off by default and managed by the device: it switches on for the start-up sync (sign-in, progress and session upload, stats and library refresh), when a page or book is not cached, when downloading, and when opening a book (to fetch the server's reading position), and stays on for 2 minutes after the last request (so browsing and downloading in a row don't reconnect each time) and then switches off. While connected but quiet (3 s after a request) the radio is in modem sleep (`WIFI_PS_MIN_MODEM`); it returns to full speed for the next request. There is no manual switch. The header Wi-Fi icon is hidden for now (the code for it is still in `gfx::wifiIcon`).
- Home: from the top, the Reading now card, the Library row (with counts), then three rows of tall, slightly rounded cards: the companion beside the reading rhythm, reading streak beside the yearly goal, and the on-pace projection beside the monthly challenge (the Android app's dashboard widgets). The Reading now card shows the book you are reading now (cover, title, progress); the Library row shows the library overview. Numbers come from `/dashboard/widgets/batch` after each sign-in and are cached in `/BookOrbit/.stats.json`, so Home shows them instantly and offline. Up/Down moves between Library and the Reading now card; OK on that card opens the book (downloading it first if needed). Back from the library returns to Home, and closing a book returns to the screen it was opened from.
- Companion: the Android app's reading-companion widget (15 characters plus Claude and Chrome's offline dinosaur, 6 poses, 64x64 sprites converted to 1 bit by `tools/make_pets.py` from the plugin art; Claude's and the dinosaur's sprites are drawn by `tools/make_claude_json.py` and `tools/make_dino_json.py`). The pose follows your reading as in the app: asleep until you have read today, reading, party on streak milestones, a ribbon when a book was finished today, yawning after 22:00. The companion is chosen in Settings (the card on Home is display only); the choice is saved. Add `"timezone"` (a POSIX TZ string, see `config.example.json`) to `/bookorbit.json` for the right local bedtime. The library counts (books, authors, series) now sit under "Library" in the menu row.
- Wi-Fi setup on the device: Settings > Network scans for nearby networks (strongest first, "Other network..." for a hidden one). Pick one and type the password on the on-screen keyboard: Up/Down/Left/Right move over the keys, OK types, Back deletes, and the bottom row has ABC/123 (capital for one letter, symbols), Space, Cancel and Done. Done saves the name and password into `/bookorbit.json` (other settings there are kept) and connects at once; open networks are saved without a password.
- Server and account: Settings > Server, Username and Password open the same keyboard (the server starts as `http://`; it must be `http://` or `https://`). Saving writes `/bookorbit.json` and signs in again at once; changing the server clears the cached library and statistics. The password row shows asterisks, but the keyboard shows what you type.
- Switching servers: saving a different server address first asks for confirmation (Cancel is preselected), then removes everything from the old server: downloaded books in `/BookOrbit/` and their reading caches, queued progress and sessions, the library list and statistics. Books you copied to the card yourself, and the settings files, are not touched. Afterwards set the username and password for the new server.
- Keyboard style: Settings > Keyboard chooses the on-screen keyboard: the key grid, or a classic phone T9 keypad (multi-tap: press a key again within about a second for its next letter; `*` switches abc/Abc/ABC, `0` is space, `1` and `#` hold punctuation). It is multi-tap only, there is no predictive dictionary. The choice is saved in `/BookOrbit/.keyboard`.
- Settings: the gear above the fourth button on Home (Right) opens a list grouped under WI-FI, ACCOUNT, DEVICE and ABOUT headings: the network, server address, username and password, keyboard style, companion, and how long until the device sleeps (1, 2, 5, 10, 15 or 30 minutes, saved in `/BookOrbit/.sleep`), plus the battery level. Up/Down select; OK opens a list or the on-screen keyboard (Up/Down choose, OK confirms, Back cancels, a tick marks the current value). Back returns to Home.
- Responsiveness: panel refreshes are asynchronous, screen changes are drawn when the panel is idle (quick presses collapse into one redraw), and a background task samples the buttons so taps are never lost while a page renders.
- Server compatibility: any 2xx answer counts as success (the real server replies 201 to sign-in), and chunked responses are decoded (`BodyStream` in `src/bookorbit.cpp`).
- Memory: the 48 KB refresh buffer that makes screen updates asynchronous is freed while the radio is moving data (`setAsyncShadowAllowed`, a small patch to FreeInkDisplay). Without that the Wi-Fi stack was starved of RAM and responses from a real server stalled for 15-25 seconds per request.
- Offline library: every library page you load is cached in `/BookOrbit/.library/`. Cached pages are shown without switching Wi-Fi on; pages that are not cached connect on demand. Downloaded books (marked with an arrow icon) can be read without any connection.
- Sleep screen: light, with the chosen companion asleep above "BookOrbit".
- Sync before sleep: when the device goes to sleep (Power click or idle timeout), from the library or from the reader, it first sends waiting reading progress and sessions and refreshes the statistics (switching Wi-Fi on for it), with a short "Syncing" modal. Nothing happens when there is nothing to send.
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
