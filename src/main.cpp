// BookOrbit for the Xteink X4: connect to a BookOrbit server, browse the library, download EPUBs to the SD card.
#include <Arduino.h>
#include <BatteryMonitor.h>
#include <EInkDisplay.h>
#include <InputManager.h>
#include <SDCardManager.h>
#include <PowerManager.h>
#include <WiFi.h>
#include <esp_heap_caps.h>

#include "bookorbit.h"
#include "config.h"
#include "gfx.h"
#include "reader.h"
#include <ArduinoJson.h>

#if FREEINK_DEVICE_X4CLASSIC
#include <XteinkDetect.h>
// X4 Classic display pins (ESP32-S3)
constexpr int EPD_SCLK = 12, EPD_MOSI = 11, EPD_CS = 13, EPD_DC = 14, EPD_RST = 10, EPD_BUSY = 18;
#else
// Original X4 display pins (ESP32-C3)
constexpr int EPD_SCLK = 8, EPD_MOSI = 10, EPD_CS = 21, EPD_DC = 4, EPD_RST = 5, EPD_BUSY = 6;
#endif

EInkDisplay display(EPD_SCLK, EPD_MOSI, EPD_CS, EPD_DC, EPD_RST, EPD_BUSY);
InputManager input;
BatteryMonitor battery;
Config config;
BookOrbit api;

// Opening a book restarts the device into a Wi-Fi-free reading mode (the reader needs a clean heap); this survives the reset.
constexpr uint32_t kReadMagic = 0x424F4B31;
RTC_NOINIT_ATTR uint32_t readMagic;
RTC_NOINIT_ATTR char readPath[160];

RTC_NOINIT_ATTR float readServerPercent;  // progress the server holds for the book, -1 when unknown
RTC_NOINIT_ATTR long readFileId;  // server file id of the open book, 0 when unknown

// Progress made offline waits in this file ("<fileId> <percent>" per line) until the library has Wi-Fi again.
// Present while the user has switched Wi-Fi off, so the choice survives restarts (opening a book restarts the device).
constexpr const char* kWifiOffFlag = "/BookOrbit/.wifi_off";

constexpr const char* kOutbox = "/BookOrbit/.progress";

static void queueProgress(long fileId, float percent) {
  if (!fileId) return;
  String out;
  String old = SdMan.exists(kOutbox) ? SdMan.readFile(kOutbox) : String();
  int start = 0;
  while (start < (int)old.length()) {  // keep the other books' entries
    int nl = old.indexOf('\n', start);
    if (nl < 0) nl = old.length();
    const String line = old.substring(start, nl);
    if (line.length() && atol(line.c_str()) != fileId) out += line + "\n";
    start = nl + 1;
  }
  out += String(fileId) + " " + String(percent, 1) + "\n";
  SdMan.ensureDirectoryExists("/BookOrbit");
  SdMan.writeFile(kOutbox, out);
}

static void flushProgress() {
  if (!SdMan.exists(kOutbox)) return;
  const String old = SdMan.readFile(kOutbox);
  String left;
  int start = 0;
  while (start < (int)old.length()) {
    int nl = old.indexOf('\n', start);
    if (nl < 0) nl = old.length();
    const String line = old.substring(start, nl);
    start = nl + 1;
    const long id = atol(line.c_str());
    const int sp = line.indexOf(' ');
    if (!id || sp < 0) continue;
    String error;
    if (api.postProgress(id, line.substring(sp + 1).toFloat(), error)) {
      Serial.printf("progress synced: file %ld at %s%%\n", id, line.substring(sp + 1).c_str());
    } else {
      Serial.printf("progress sync failed: %s\n", error.c_str());
      left += line + "\n";
    }
  }
  if (left.isEmpty()) SdMan.remove(kOutbox); else SdMan.writeFile(kOutbox, left);
}

constexpr uint32_t kSleepAfterMs = 5 * 60 * 1000;  // no button for this long: show the sleep screen and deep-sleep
uint32_t lastActivity = 0;

// ---- Reading sessions (statistics) -----------------------------------------------------------------------------
// The reader has no network, so a session is written to the SD card at checkpoints (every 10 page turns, on Back and
// before sleep) and uploaded with the progress at the next library start. Re-saving a session replaces its earlier copy.
constexpr const char* kSessionOutbox = "/BookOrbit/.sessions";
constexpr uint32_t kIdleLimitMs = 2 * 60 * 1000;  // a pause longer than this counts as 2 minutes of reading
constexpr int kCheckpointTurns = 10;
constexpr int kCleanupTurns = 15;  // page turns between ghosting-cleanup refreshes while reading
constexpr const char* kSessionSource = "android";  // value the server's session API accepts (the Android client's)

struct ReadSession {
  bool valid = false;
  char id[37] = "";
  time_t startedAt = 0;
  float startPercent = 0;
  uint32_t activeMs = 0, lastTick = 0;
  int turns = 0;
} sess;

static bool clockValid() { return time(nullptr) > 1700000000; }

static String isoTime(time_t t) {
  char buf[24];
  struct tm tmv;
  gmtime_r(&t, &tmv);
  strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tmv);
  return buf;
}

static void newUuid(char* out) {
  uint8_t b[16];
  esp_fill_random(b, sizeof(b));
  b[6] = (b[6] & 0x0F) | 0x40;
  b[8] = (b[8] & 0x3F) | 0x80;
  snprintf(out, 37, "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x", b[0], b[1], b[2], b[3], b[4], b[5], b[6],
           b[7], b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15]);
}

// Starts tracking when a book opens; without a valid clock (never online since power-on) there are no timestamps, so no session.
static void beginSession(long fileId) {
  sess = ReadSession();
  if (!fileId || !clockValid()) {
    Serial.printf("session: not tracked (%s)\n", fileId ? "clock not set" : "no file id");
    return;
  }
  sess.valid = true;
  newUuid(sess.id);
  sess.startedAt = time(nullptr);
  sess.startPercent = reader::progressPercent();
  sess.lastTick = millis();
}

// Call on every button press while reading: adds the time since the last press, capped at the idle limit.
static void sessionTick() {
  if (!sess.valid) return;
  const uint32_t now = millis();
  sess.activeMs += min<uint32_t>(now - sess.lastTick, kIdleLimitMs);
  sess.lastTick = now;
}

static void queueSession(long fileId, const String& line, const char* id) {
  String out, old = SdMan.exists(kSessionOutbox) ? SdMan.readFile(kSessionOutbox) : String();
  int start = 0;
  while (start < (int)old.length()) {  // drop the earlier copy of this session
    int nl = old.indexOf('\n', start);
    if (nl < 0) nl = old.length();
    const String l = old.substring(start, nl);
    if (l.length() && l.indexOf(id) < 0) out += l + "\n";
    start = nl + 1;
  }
  out += line + "\n";
  SdMan.ensureDirectoryExists("/BookOrbit");
  SdMan.writeFile(kSessionOutbox, out);
}

// Saves what is known so far: the position for progress sync, and the session for the statistics.
static void checkpoint() {
  const float percent = reader::progressPercent();
  queueProgress(readFileId, percent);
  if (!sess.valid) return;
  const uint32_t active = sess.activeMs + min<uint32_t>(millis() - sess.lastTick, kIdleLimitMs);
  if (active < 5000) return;
  JsonDocument doc;
  doc["fileId"] = readFileId;
  JsonObject o = doc["session"].to<JsonObject>();
  o["sessionId"] = sess.id;
  o["startedAt"] = isoTime(sess.startedAt);
  o["endedAt"] = isoTime(time(nullptr));
  o["durationSeconds"] = active / 1000;
  o["progressDelta"] = percent - sess.startPercent;
  o["endProgress"] = percent;
  o["sessionType"] = "read";
  o["source"] = kSessionSource;
  String line;
  serializeJson(doc, line);
  queueSession(readFileId, line, sess.id);
  Serial.printf("session checkpoint: %us, %.1f%% -> %.1f%%\n", (unsigned)(active / 1000), sess.startPercent, percent);
}

static void flushSessions() {
  if (!SdMan.exists(kSessionOutbox)) return;
  const String old = SdMan.readFile(kSessionOutbox);
  String left;
  int start = 0;
  while (start < (int)old.length()) {
    int nl = old.indexOf('\n', start);
    if (nl < 0) nl = old.length();
    const String line = old.substring(start, nl);
    start = nl + 1;
    if (line.isEmpty()) continue;
    JsonDocument doc;
    if (deserializeJson(doc, line)) continue;  // unreadable: drop it
    String body, error;
    serializeJson(doc["session"], body);
    bool rejected = false;
    if (api.postSession(doc["fileId"] | 0L, body, rejected, error)) {
      Serial.printf("session synced: file %ld, %us\n", (long)(doc["fileId"] | 0L), (unsigned)(doc["session"]["durationSeconds"] | 0));
    } else if (rejected) {
      Serial.printf("session rejected, dropped: %s\n", error.c_str());
    } else {
      Serial.printf("session sync failed: %s\n", error.c_str());
      left += line + "\n";
    }
  }
  if (left.isEmpty()) SdMan.remove(kSessionOutbox); else SdMan.writeFile(kSessionOutbox, left);
}

constexpr int kPageSize = 10;
constexpr int kMargin = 16;
constexpr int kRowHeight = 68;
constexpr int kListTop = 64;

Book books[kPageSize];
int bookCount = 0, bookTotal = 0, page = 0, selected = 0;
int fastRefreshes = 0;
// The library appears at once from the cache; Wi-Fi and sign-in finish in the background (see backgroundSync()).
enum class Net { Connecting, Online, Offline } net = Net::Offline;
uint32_t netStart = 0;
String offlineReason;

[[noreturn]] static void goToSleep();
static void drawSleepScreen();

// Test hook: single serial characters act as button presses (o=OK d=down u=up n=right p=left b=back).
static int injected = -1;
static bool pressed(uint8_t button) {
  if (injected == button) { injected = -1; return true; }
  return input.wasPressed(button);
}

// Serial commands: "F" dumps the framebuffer; "CFG:<json>" saves the settings file to the SD card and reboots.
static void handleSerial() {
  static String line;
  while (Serial.available()) {
    const char c = Serial.read();
    if (c == '\n' || c == '\r') {
      if (line.startsWith("OPEN:")) {  // test hook: read any EPUB on the card
        readMagic = kReadMagic;
        readFileId = 0;
        readServerPercent = -1;
        strlcpy(readPath, line.substring(5).c_str(), sizeof(readPath));
        delay(100);
        ESP.restart();
      }
      if (line == "SLEEP") goToSleep();
      if (line == "WIFITOGGLE") { extern void testToggleWifi(); testToggleWifi(); }  // test hook
      if (line == "BUSYDOT") { extern void testBusyDot(); testBusyDot(); }  // test hook
      if (line == "SLEEPSCREEN") drawSleepScreen();  // test hook: draw it without sleeping
      if (line.startsWith("CFG:")) {
        const bool ok = SdMan.writeFile("/bookorbit.json", line.substring(4));
        Serial.println(ok ? "config saved, restarting" : "config write failed");
        if (ok) {
          delay(200);
          ESP.restart();
        }
      }
      line = "";
    } else if (line.length() == 0 && c == 'F') {
      gfx::dumpFramebuffer();
    } else if (line.length() == 0 && strchr("odunpb", c)) {
      injected = c == 'o' ? InputManager::BTN_CONFIRM : c == 'd' ? InputManager::BTN_DOWN : c == 'u' ? InputManager::BTN_UP
               : c == 'n' ? InputManager::BTN_RIGHT : c == 'p' ? InputManager::BTN_LEFT : InputManager::BTN_BACK;
    } else if (line.length() < 512) {
      line += c;
    }
  }
}

// ---- Screens -------------------------------------------------------------------------------------------------

// A dot in the header while the device is talking to the BookOrbit server.
static bool busy = false;
constexpr int kBusyDotX = gfx::W - kMargin - 37 - 14 - 26 - 14, kBusyDotY = 24;

static void drawBusyDot() {
  for (int dy = -5; dy <= 5; dy++)
    for (int dx = -5; dx <= 5; dx++)
      if (dx * dx + dy * dy <= 25) gfx::fillRect(kBusyDotX + dx, kBusyDotY + dy, 1, 1, false);
}

// Turning it on shows the dot straight away; callers redraw their screen when the request is done.
static void setBusy(bool on) {
  busy = on;
  if (on) {
    drawBusyDot();
    gfx::flush(EInkDisplay::FAST_REFRESH);
  }
}

void testBusyDot() { setBusy(true); busy = false; }  // test hook: show the dot without a request

static void header(const char* title) {
  gfx::fillRect(0, 0, gfx::W, 48, true);
  gfx::text(kMargin, 7, title, false);
  const int batX = gfx::W - kMargin - 37;
  gfx::batteryIcon(batX, 15, battery.readPercentage(), false);
  // Wi-Fi: strength arcs when online, just the dot while connecting, crossed out when offline.
  int arcs = net == Net::Offline ? 3 : 0;  // offline: the full icon, crossed out
  if (net == Net::Online && WiFi.status() == WL_CONNECTED) {
    const int rssi = WiFi.RSSI();
    arcs = rssi > -70 ? 3 : rssi > -82 ? 2 : 1;
  }
  gfx::wifiIcon(batX - 14 - 26, 13, arcs, net == Net::Offline, false, net == Net::Offline);
  if (busy) drawBusyDot();
}

// Centered multi-line message under the header; lines are separated by '\n'.
static void message(const char* title, const String& body, bool full = true) {
  gfx::clear();
  header(title);
  int y = 120;
  String rest = body;
  while (rest.length()) {
    int nl = rest.indexOf('\n');
    String line = nl < 0 ? rest : rest.substring(0, nl);
    rest = nl < 0 ? "" : rest.substring(nl + 1);
    gfx::text(kMargin, y, gfx::ellipsize(line.c_str(), gfx::W - 2 * kMargin));
    y += gfx::lineHeight() + 6;
  }
  gfx::flush(full ? EInkDisplay::FULL_REFRESH : EInkDisplay::FAST_REFRESH);
}

// ---- Button hints: icons for the buttons instead of text codes -------------------------------------------------

enum class Dir { Up, Down, Left, Right };

static void triangle(int cx, int cy, Dir d, bool black) {  // 13 px wide/high pointing arrow
  for (int i = 0; i < 13; i++) {
    const int half = i / 2;  // i = 0 is the tip, widening to the base
    switch (d) {
      case Dir::Up: gfx::fillRect(cx - half, cy - 6 + i, half * 2 + 1, 1, black); break;
      case Dir::Down: gfx::fillRect(cx - half, cy + 6 - i, half * 2 + 1, 1, black); break;
      case Dir::Left: gfx::fillRect(cx - 6 + i, cy - half, 1, half * 2 + 1, black); break;
      case Dir::Right: gfx::fillRect(cx + 6 - i, cy - half, 1, half * 2 + 1, black); break;
    }
  }
}

// Hints sit directly above the four buttons along the bottom edge: Back, OK, Left, Right.
constexpr int kButtonX[4] = {25, 130, 245, 350}, kButtonW = 106;

static void drawHints() {
  const int top = gfx::H - 52, y = gfx::H - 42;
  gfx::fillRect(0, top, gfx::W, 2, true);
  const auto center = [](int i, int w) { return kButtonX[i] + (kButtonW - w) / 2; };
  // Back: a return arrow (shaft out to the right, down, and back left) so it isn't mistaken for the Left arrow.
  const int bx = center(0, 30), by = y + 6;
  triangle(bx + 6, by, Dir::Left, true);
  gfx::fillRect(bx + 8, by - 1, 20, 3, true);
  gfx::fillRect(bx + 26, by - 1, 3, 15, true);
  gfx::fillRect(bx + 10, by + 11, 19, 3, true);
  // OK: a solid tick.
  const int ox = kButtonX[1] + kButtonW / 2, oy = y + 14;
  for (int i = 0; i < 7; i++) gfx::fillRect(ox - 13 + i, oy - 1 + i, 4, 4, true);       // short stroke down-right
  for (int i = 0; i < 15; i++) gfx::fillRect(ox - 7 + i, oy + 5 - i * 17 / 15, 4, 4, true);  // long stroke up-right
  triangle(kButtonX[2] + kButtonW / 2, y + 14, Dir::Left, true);
  triangle(kButtonX[3] + kButtonW / 2, y + 14, Dir::Right, true);
}

static void drawList(bool full) {
  gfx::clear();
  char title[48];
  const int pages = max(1, (bookTotal + kPageSize - 1) / kPageSize);
  snprintf(title, sizeof(title), "Library  %d/%d", page + 1, pages);
  header(title);
  for (int i = 0; i < bookCount; i++) {
    const int y = kListTop + i * kRowHeight;
    const bool sel = i == selected;
    if (sel) gfx::fillRect(0, y, gfx::W, kRowHeight - 4, true);
    gfx::text(kMargin, y + 2, gfx::ellipsize(books[i].title, gfx::W - 2 * kMargin), !sel);
    gfx::text(kMargin, y + 2 + gfx::lineHeight() - 2, gfx::ellipsize(books[i].author, gfx::W - 2 * kMargin), !sel);
    if (SdMan.exists(BookOrbit::pathFor(books[i]).c_str())) gfx::fillRect(gfx::W - 24, y + 20, 12, 12, !sel);  // downloaded
  }
  if (bookCount == 0) gfx::text(kMargin, 120, "No books found");
  if (net == Net::Connecting) gfx::text(kMargin, gfx::H - 96, "Connecting...", true);
  if (net == Net::Offline) gfx::text(kMargin, gfx::H - 96, "Offline - hold Back to connect", true);
  drawHints();
  // The first draw after power-on / wake / returning from a book uses the gentler half refresh: far less flashing than a full one.
  static bool firstDraw = true;
  gfx::flush(!full ? EInkDisplay::FAST_REFRESH : firstDraw ? EInkDisplay::HALF_REFRESH : EInkDisplay::FULL_REFRESH);
  firstDraw = false;
}

static void showList(bool full) {
  // Fast refreshes leave ghosting; clean up every few.
  if (!full && ++fastRefreshes >= 8) full = true;
  if (full) fastRefreshes = 0;
  drawList(full);
}

// ---- Actions -------------------------------------------------------------------------------------------------

static bool connectWifi(String& error) {
  WiFi.mode(WIFI_STA);
  WiFi.begin(config.ssid.c_str(), config.wifiPassword.c_str());
  const uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED) {
    if (millis() - start > 20000) {
      error = "Could not join Wi-Fi \"" + config.ssid + "\"";
      return false;
    }
    delay(200);
  }
  return true;
}

static bool ensureWifi(String& error) {
  return WiFi.status() == WL_CONNECTED || connectWifi(error);
}

// The library list is cached per page on the SD card so it can be browsed without a connection.
static String cachePath(int p) { return "/BookOrbit/.library/p" + String(p) + ".json"; }

static void saveCachedPage(int p) {
  JsonDocument doc;
  doc["total"] = bookTotal;
  for (int i = 0; i < bookCount; i++) {
    doc["books"][i]["id"] = books[i].id;
    doc["books"][i]["fileId"] = books[i].fileId;
    doc["books"][i]["title"] = books[i].title;
    doc["books"][i]["author"] = books[i].author;
  }
  String json;
  serializeJson(doc, json);
  SdMan.ensureDirectoryExists("/BookOrbit");
  SdMan.ensureDirectoryExists("/BookOrbit/.library");
  SdMan.writeFile(cachePath(p).c_str(), json);
}

static bool loadCachedPage(int p) {
  if (!SdMan.exists(cachePath(p).c_str())) return false;
  JsonDocument doc;
  if (deserializeJson(doc, SdMan.readFile(cachePath(p).c_str()))) return false;
  bookTotal = doc["total"] | 0;
  bookCount = 0;
  for (JsonObject o : doc["books"].as<JsonArray>()) {
    if (bookCount >= kPageSize) break;
    Book& b = books[bookCount++];
    b = Book();
    b.id = o["id"] | 0L;
    b.fileId = o["fileId"] | 0L;
    strlcpy(b.title, o["title"] | "(untitled)", sizeof(b.title));
    strlcpy(b.author, o["author"] | "", sizeof(b.author));
  }
  page = p;
  selected = 0;
  return true;
}

static bool loadPage(int newPage, String& error) {
  if (net == Net::Online) {
    String netError;
    int total = 0;
    if (ensureWifi(netError)) {
      const int n = api.listBooks(newPage, kPageSize, books, kPageSize, total, netError);
      if (n >= 0) {
        bookCount = n;
        bookTotal = total;
        page = newPage;
        selected = 0;
        saveCachedPage(newPage);
        return true;
      }
    }
    if (!loadCachedPage(newPage)) {
      error = netError;
      return false;
    }
    net = Net::Offline;  // connection lost: carry on from the cache
    offlineReason = netError;
    return true;
  }
  if (loadCachedPage(newPage)) return true;
  error = "This page is not cached.\nHold Back to connect.";
  return false;
}

static void downloadProgress(size_t done, size_t total) {
  static uint32_t last = 0;
  if (millis() - last < 3000) return;
  last = millis();
  Serial.printf("download %u/%u bytes\n", (unsigned)done, (unsigned)total);
}

static void waitForKey() {
  do {
    delay(20);
    input.update();
  } while (!input.wasAnyPressed() && injected < 0);
  injected = -1;
}

// Opens the selected book, downloading it first when it isn't on the SD card yet.
static void openSelected() {
  if (bookCount == 0) return;
  const Book& b = books[selected];
  String path = BookOrbit::pathFor(b), error;
  if (net != Net::Online && !SdMan.exists(path.c_str())) {
    message("Not downloaded", net == Net::Connecting ? "This book is not on the SD card.\nStill connecting, try again\nin a moment.\n\nPress any button" : "This book is not on the SD card.\nHold Back in the list to connect.\n\nPress any button", false);
    waitForKey();
    showList(false);
    return;
  }
  if (!SdMan.exists(path.c_str())) {
    busy = true;  // shown by the "Downloading" screen
    message("Downloading", String(gfx::ellipsize(b.title, 440)) + "\nPlease wait...", false);
    if (!ensureWifi(error) || !api.download(b, path, error, downloadProgress)) {
      busy = false;
      message("Download failed", error + "\n\nPress any button", false);
      Serial.printf("download failed: %s\n", error.c_str());
      waitForKey();
      showList(false);
      return;
    }
  }
  readMagic = kReadMagic;
  readFileId = b.fileId;
  readServerPercent = -1;
  float serverPct;
  if (net == Net::Online && b.fileId && ensureWifi(error) && api.getProgress(b.fileId, serverPct, error)) readServerPercent = serverPct;
  strlcpy(readPath, path.c_str(), sizeof(readPath));
  Serial.printf("restarting into reader: %s\n", readPath);
  delay(100);
  ESP.restart();
}

static void drawSleepScreen() {
  gfx::loadUiFont();
  gfx::clear(true);  // inverted: white text on black
  gfx::bigTextCentered(gfx::H / 2 - 60, "BookOrbit", 80, false);
  gfx::bigTextCentered(gfx::H / 2 + 60, "Zzz...", 28, false);
  gfx::flush(EInkDisplay::HALF_REFRESH);  // a full refresh flashes the panel for ~4 s
}

// Draws the sleep screen and deep-sleeps; the Power button wakes the device (a fresh boot).
// An open book is saved first and reopened on wake.
[[noreturn]] static void goToSleep() {
  Serial.println("sleeping");
  if (reader::isOpen()) {
    sessionTick();
    checkpoint();
    reader::close();  // saves the position, frees the heap
    readMagic = kReadMagic;
    readServerPercent = -1;
    // readPath and readFileId are still set from opening this book
  }
  if (WiFi.getMode() != WIFI_OFF) WiFi.mode(WIFI_OFF);
  drawSleepScreen();
  display.deepSleep();
  freeink::PowerManager::deepSleepUntilPowerButton();
}

static void closeBook() {
  sessionTick();
  checkpoint();
  reader::close();
  readFileId = 0;
  ESP.restart();  // back to the library, with Wi-Fi
}

static void readerLoop() {
  if (pressed(InputManager::BTN_RIGHT) || pressed(InputManager::BTN_DOWN)) {
    sessionTick();
    if (++fastRefreshes >= kCleanupTurns) { reader::next(true); fastRefreshes = 0; } else reader::next(false);
    if (++sess.turns % kCheckpointTurns == 0) checkpoint();
  } else if (pressed(InputManager::BTN_LEFT) || pressed(InputManager::BTN_UP)) {
    sessionTick();
    if (++fastRefreshes >= kCleanupTurns) fastRefreshes = 0;
    reader::prev();
    if (++sess.turns % kCheckpointTurns == 0) checkpoint();
  } else if (pressed(InputManager::BTN_BACK)) {
    closeBook();
  }
}

[[noreturn]] static void fatal(const char* title, const String& detail) {
  Serial.printf("%s: %s\n", title, detail.c_str());
  message(title, detail + "\n\nPress Power to retry");
  while (true) {
    input.update();
    handleSerial();
    if (input.wasPressed(InputManager::BTN_POWER)) ESP.restart();
    delay(50);
  }
}

// ---- Arduino entry points ------------------------------------------------------------------------------------

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("bookorbit-xteink");
  lastActivity = millis();

  BoardConfig::releaseSdRail();
  input.begin();
#if FREEINK_DEVICE_X4CLASSIC
  freeink::applyXteinkDisplayController();
#endif
  // The SD card must be mounted before the display driver claims the shared SPI bus.
  const bool sdOk = SdMan.begin();
  display.begin();
  gfx::begin(&display);

  if (readMagic != kReadMagic) gfx::loadUiFont();  // library mode only; the reader needs that heap

  if (!sdOk) fatal("No SD card", "Insert a FAT32 card.");

  if (readMagic == kReadMagic) {  // reading mode: no Wi-Fi, clean heap
    readMagic = 0;
    String openError;
    Serial.printf("reading mode, free heap %u, largest block %u\n", (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMaxAllocHeap());
    if (reader::open(readPath, openError, readServerPercent)) {
      beginSession(readFileId);
      reader::draw(true);
      return;
    }
    fatal("Cannot open book", openError);
  }
  String error;
  if (!config.load(error)) fatal("Setup needed", error + "\nSee config.example.json");

  api.configure(config.server, config.insecureTls);
  if (loadCachedPage(0)) {  // show the library right away and connect in the background
    if (SdMan.exists(kWifiOffFlag)) {  // the user turned Wi-Fi off: stay offline until they turn it on
      net = Net::Offline;
      Serial.printf("library: %d books (cached, Wi-Fi off)\n", bookCount);
      showList(true);
      return;
    }
    net = Net::Connecting;
    netStart = millis();
    WiFi.mode(WIFI_STA);
    WiFi.begin(config.ssid.c_str(), config.wifiPassword.c_str());
    Serial.printf("library: %d books (cached, connecting in the background)\n", bookCount);
    showList(true);
    return;
  }
  // First run, nothing cached yet: connect before showing anything.
  net = Net::Connecting;
  message("Connecting", "Wi-Fi: " + config.ssid, false);
  bool online = connectWifi(error);
  if (online) {
    Serial.printf("wifi ok, ip %s\n", WiFi.localIP().toString().c_str());
    configTime(0, 0, "pool.ntp.org", "time.google.com");
    message("Connecting", "Signing in to\n" + config.server, false);
    online = api.login(config.username, config.password, error);
  }
  if (online) {
    flushProgress();
    flushSessions();
    net = Net::Online;
    online = loadPage(0, error);
  }
  if (!online) fatal("Offline", error + "\n\nNo cached library yet.");
  Serial.printf("library: %d books (page %d)\n", bookTotal, bookCount);
  showList(true);
}

// Runs from loop() while the library is up: finishes Wi-Fi, sign-in and the list refresh without blocking the buttons for long.
static void backgroundSync() {
  if (net != Net::Connecting) return;
  String error;
  if (WiFi.status() != WL_CONNECTED) {
    if (millis() - netStart > 20000) {
      net = Net::Offline;
      offlineReason = "Could not join Wi-Fi \"" + config.ssid + "\"";
      Serial.printf("offline: %s\n", offlineReason.c_str());
      showList(false);
    }
    return;
  }
  Serial.printf("wifi ok, ip %s\n", WiFi.localIP().toString().c_str());
  configTime(0, 0, "pool.ntp.org", "time.google.com");  // wall-clock time for reading sessions
  const int keep = selected, keepPage = page;
  setBusy(true);
  if (api.login(config.username, config.password, error)) {
    flushProgress();
    flushSessions();
    net = Net::Online;
    if (loadPage(keepPage, error)) selected = min(keep, max(0, bookCount - 1));
    Serial.printf("library: %d books (page %d) refreshed\n", bookTotal, bookCount);
  } else {
    net = Net::Offline;
    offlineReason = error;
    Serial.printf("offline: %s\n", error.c_str());
  }
  busy = false;
  showList(false);
}

// Holding Back switches Wi-Fi off (library stays usable from the cache) or back on.
static void toggleWifi() {
  if (net == Net::Offline) {
    net = Net::Connecting;
    netStart = millis();
    SdMan.remove(kWifiOffFlag);
    WiFi.mode(WIFI_STA);
    WiFi.begin(config.ssid.c_str(), config.wifiPassword.c_str());
    Serial.println("wifi on");
  } else {
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
    net = Net::Offline;
    SdMan.ensureDirectoryExists("/BookOrbit");
    SdMan.writeFile(kWifiOffFlag, "1");
    Serial.println("wifi off");
  }
  showList(false);
}

void testToggleWifi() { toggleWifi(); }

// True once per press, when Back has been held for 800 ms.
static bool backHeld() {
  static bool handled = false;
  if (!input.isPressed(InputManager::BTN_BACK)) { handled = false; return false; }
  if (handled || input.getHeldTime() < 800) return false;
  handled = true;
  return true;
}

void loop() {
  input.update();
  handleSerial();
  if (input.wasAnyPressed() || injected >= 0) lastActivity = millis();
  if (millis() - lastActivity > kSleepAfterMs) goToSleep();
  // A click on Power sleeps, but only once the button has been seen released since boot, so the press
  // that woke the device doesn't put it straight back to sleep.
  static bool powerArmed = false;
  if (!input.isPowerButtonPhysicallyPressed()) powerArmed = true;
  if (powerArmed && input.wasPressed(InputManager::BTN_POWER)) goToSleep();

  if (reader::isOpen()) {
    readerLoop();
    delay(10);
    return;
  }
  backgroundSync();
  String error;
  if (backHeld()) {
    toggleWifi();
  } else if (pressed(InputManager::BTN_DOWN) && selected + 1 < bookCount) {
    selected++;
    showList(false);
  } else if (pressed(InputManager::BTN_UP) && selected > 0) {
    selected--;
    showList(false);
  } else if (pressed(InputManager::BTN_CONFIRM)) {
    openSelected();
  } else if (pressed(InputManager::BTN_RIGHT) && (page + 1) * kPageSize < bookTotal) {
    if (net == Net::Online) setBusy(true);
    const bool ok = loadPage(page + 1, error);
    busy = false;
    if (ok) showList(true); else message("Error", error + "\n\nPress any button");
  } else if (pressed(InputManager::BTN_LEFT) && page > 0) {
    if (net == Net::Online) setBusy(true);
    const bool ok = loadPage(page - 1, error);
    busy = false;
    if (ok) showList(true); else message("Error", error + "\n\nPress any button");
  }
  delay(10);
}
