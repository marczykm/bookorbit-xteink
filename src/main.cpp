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
#include "cover.h"
#include "pets.h"
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
int readerBatteryPercent() { return battery.readPercentage(); }  // for the reader's header
Config config;
BookOrbit api;

// Opening a book restarts the device into a Wi-Fi-free reading mode (the reader needs a clean heap); this survives the reset.
constexpr uint32_t kReadMagic = 0x424F4B31;
RTC_NOINIT_ATTR uint32_t readMagic;
RTC_NOINIT_ATTR char readPath[160];

RTC_NOINIT_ATTR float readServerPercent;  // progress the server holds for the book, -1 when unknown
constexpr uint32_t kLibraryMagic = 0x4C494252;
RTC_NOINIT_ATTR int32_t openScope;       // which list the book was opened from: a Scope value, plus its id and name
RTC_NOINIT_ATTR int32_t openScopeId;
RTC_NOINIT_ATTR char openScopeName[48];
RTC_NOINIT_ATTR uint32_t openLibraryMagic;  // set when closing a book so the device returns to the library, not Home
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

// No button for this long: show the sleep screen and deep-sleep. Chosen in Settings, saved on the SD card.
constexpr int kSleepOptions[] = {1, 2, 5, 10, 15, 30};  // minutes
constexpr const char* kSleepFile = "/BookOrbit/.sleep";
int sleepMinutes = 5;
static uint32_t sleepAfterMs() { return (uint32_t)sleepMinutes * 60 * 1000; }
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

constexpr int kPageSize = 9;
constexpr int kMargin = 16;
constexpr int kRowHeight = 70;
constexpr int kListTop = 64;

Book books[kPageSize];
int bookCount = 0, bookTotal = 0, page = 0, selected = 0;
Scope src = Scope::All;  // what the book list shows: everything, or one author / series / smart scope
long srcId = 0;
char srcName[48] = "";
Entity entities[kPageSize];  // the browse list of authors / series / smart scopes
int entCount = 0, entTotal = 0, entPage = 0, entSel = 0;
Scope entKind = Scope::Author, entLoaded = Scope::All;  // entLoaded: which kind `entities` holds (All = nothing yet)
int browseSel = 0;
int fastRefreshes = 0;
// Which screen is showing. Home is the entry point; Library is its only menu item for now.
enum class Screen { Home, Browse, Entities, Library, Settings, Picker, Networks, Keyboard, Confirm } screen = Screen::Home;
bool halfNext = false;  // the next full draw (a screen change) uses the gentler half refresh
int pickerKind = 0, pickerSel = 0, pickerTop = 0;  // Picker: 0 = companion list, 1 = sleep timer list
int kbStyle = 0;  // on-screen keyboard: 0 = key grid, 1 = classic T9 (multi-tap phone keypad)
constexpr const char* kKeyboardFile = "/BookOrbit/.keyboard";
int settingsSel = 0;  // Settings list: Wi-Fi on/off, network, server, username, password, companion, sleep timer
int homeSel = 0;  // Home menu, counted from the top: Reading now (when present), then Library
bool firstDraw = true;  // the first draw after power-on / wake / returning from a book is a half refresh too

// The library appears at once from the cache; Wi-Fi and sign-in finish in the background (see backgroundSync()).
enum class Net { Connecting, Online, Offline } net = Net::Offline;
uint32_t netStart = 0;
String offlineReason;

[[noreturn]] static void goToSleep();
static void drawSleepScreen();

// Test hook: single serial characters act as button presses (o=OK d=down u=up n=right p=left b=back).
static int injected = -1;

// Buttons are sampled by a small background task, so a tap is never missed while the main loop is busy (rendering a page,
// talking to the server): presses are counted here and consumed by pressed(), in order.
static volatile uint8_t pressCount[8];
static volatile uint8_t anyCount;
static volatile bool backReleased;

static void inputTask(void*) {
  for (;;) {
    input.update();
    for (uint8_t b = 0; b < 7; b++)
      if (input.wasPressed(b) && pressCount[b] < 4) pressCount[b] = pressCount[b] + 1;
    if (input.wasAnyPressed() && anyCount < 250) anyCount = anyCount + 1;
    if (input.wasReleased(InputManager::BTN_BACK)) backReleased = true;
    vTaskDelay(pdMS_TO_TICKS(8));
  }
}

static bool pressed(uint8_t button) {
  if (injected == button) { injected = -1; return true; }
  if (pressCount[button] == 0) return false;
  pressCount[button] = pressCount[button] - 1;
  return true;
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
      if (line == "TESTNOW") { extern void testFakeNow(); testFakeNow(); }  // test hook
      if (line == "SYNCMODAL") { extern void testSyncModal(); testSyncModal(); }  // test hook
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
      if (dx * dx + dy * dy <= 25) gfx::fillRect(kBusyDotX + dx, kBusyDotY + dy, 1, 1, true);
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
  gfx::fillRect(0, 46, gfx::W, 2, true);  // light header: dark text on the page, a rule below
  gfx::text(kMargin, 7, title, true);
  const int batX = gfx::W - kMargin - 37;
  gfx::batteryIcon(batX, 15, battery.readPercentage(), true);
  // Wi-Fi: strength arcs when online, just the dot while connecting, crossed out when offline.
  int arcs = net == Net::Offline ? 3 : 0;  // offline: the full icon, crossed out
  if (net == Net::Online && WiFi.status() == WL_CONNECTED) {
    const int rssi = WiFi.RSSI();
    arcs = rssi > -70 ? 3 : rssi > -82 ? 2 : 1;
  }
  gfx::wifiIcon(batX - 14 - 26, 13, arcs, net == Net::Offline, true, net == Net::Offline);
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

static void drawHints(bool back = true, bool ok = true, bool paging = true, bool settings = false) {
  const int top = gfx::H - 52, y = gfx::H - 42;
  gfx::fillRect(0, top, gfx::W, 2, true);
  const auto center = [](int i, int w) { return kButtonX[i] + (kButtonW - w) / 2; };
  // Back: a return arrow (shaft out to the right, down, and back left) so it isn't mistaken for the Left arrow.
  const int bx = center(0, 30), by = y + 6;
  if (back) {
    triangle(bx + 6, by, Dir::Left, true);
    gfx::fillRect(bx + 8, by - 1, 20, 3, true);
    gfx::fillRect(bx + 26, by - 1, 3, 15, true);
    gfx::fillRect(bx + 10, by + 11, 19, 3, true);
  }
  // OK: a solid tick.
  const int ox = kButtonX[1] + kButtonW / 2, oy = y + 14;
  if (ok) {
    for (int i = 0; i < 7; i++) gfx::fillRect(ox - 13 + i, oy - 1 + i, 4, 4, true);       // short stroke down-right
    for (int i = 0; i < 15; i++) gfx::fillRect(ox - 7 + i, oy + 5 - i * 17 / 15, 4, 4, true);  // long stroke up-right
  }
  if (paging) {
    triangle(kButtonX[2] + kButtonW / 2, y + 14, Dir::Left, true);
    triangle(kButtonX[3] + kButtonW / 2, y + 14, Dir::Right, true);
  }
  if (settings) {  // a gear above the fourth button
    const int gx = kButtonX[3] + kButtonW / 2, gy = y + 14;
    for (int dy = -13; dy <= 13; dy++)
      for (int dx = -13; dx <= 13; dx++) {
        const float d = sqrtf((float)(dx * dx + dy * dy));
        bool on = d <= 9.0f && d >= 4.5f;  // the ring with its hub hole
        if (!on && d > 9.0f && d <= 13.0f) {  // eight teeth
          float a = atan2f((float)dy, (float)dx) * 180.0f / (float)PI;  // -180..180
          a = fmodf(a + 360.0f + 22.5f, 45.0f);
          on = a > 11.0f && a < 34.0f;
        }
        if (on) gfx::fillRect(gx + dx, gy + dy, 1, 1, true);
      }
  }
}

// A small "saved on the device" icon: an arrow dropping into a tray.
static void drawDownloadedIcon(int x, int y, bool ink) {
  gfx::fillRect(x + 6, y, 4, 8, ink);                                          // arrow shaft
  for (int r = 0; r < 6; r++) gfx::fillRect(x + 8 - (6 - r), y + 7 + r, 2 * (6 - r), 1, ink);  // arrow head
  gfx::fillRect(x, y + 15, 16, 2, ink);                                         // tray bottom
  gfx::fillRect(x, y + 11, 2, 6, ink);                                          // tray sides
  gfx::fillRect(x + 14, y + 11, 2, 6, ink);
}

// Text in a normal row (black) or a highlighted one (white on the black highlight).
static void label(int x, int y, const char* s, bool highlighted) { gfx::text(x, y, s, !highlighted); }

static void drawList(bool full) {
  gfx::clear();
  char title[48];
  const int pages = max(1, (bookTotal + kPageSize - 1) / kPageSize);
  if (src == Scope::All) snprintf(title, sizeof(title), "Library  %d/%d", page + 1, pages);
  else snprintf(title, sizeof(title), "%s  %d/%d", gfx::ellipsize(srcName, 280), page + 1, pages);
  header(title);
  for (int i = 0; i < bookCount; i++) {
    const int y = kListTop + i * kRowHeight;
    const bool sel = i == selected;
    if (sel) gfx::fillRect(0, y, gfx::W, kRowHeight - 4, true);
    const int textX = kMargin + 4, textW = gfx::W - textX - 34;
    label(textX, y + 4, gfx::ellipsize(books[i].title, textW), sel);
    label(textX, y + 4 + gfx::lineHeight() - 2, gfx::ellipsize(books[i].author, textW), sel);
    if (SdMan.exists(BookOrbit::pathFor(books[i]).c_str())) drawDownloadedIcon(gfx::W - 16 - 14, y + 24 - 2, !sel);  // on the card
  }
  if (bookCount == 0) gfx::text(kMargin, 120, "No books found");
  if (net == Net::Connecting) gfx::text(kMargin, gfx::H - 96, "Connecting...", true);
  if (net == Net::Offline) gfx::text(kMargin, gfx::H - 96, "Offline - hold Back to connect", true);
  drawHints();
  gfx::flush(!full ? EInkDisplay::FAST_REFRESH : (firstDraw || halfNext) ? EInkDisplay::HALF_REFRESH : EInkDisplay::FULL_REFRESH);
  firstDraw = false;
  halfNext = false;
}

// ---- Home statistics (the same dashboard widgets as the Android app) ---------------------------------------------

constexpr const char* kStatsFile = "/BookOrbit/.stats.json";
constexpr const char* kStatTypes[] = {"reading-streak", "reading-goal", "reading-rhythm", "year-projection", "monthly-challenge", "library-overview"};

struct Stats {
  bool readingNow = false;
  long nowId = 0, nowFileId = 0;
  char nowTitle[96] = "", nowAuthor[64] = "";
  float nowProgress = 0;
  bool streak = false, goal = false, rhythm = false, projection = false, challenge = false, overview = false;
  int streakDays = 0, longest = 0;
  bool week[7] = {};
  int goalDone = 0, goalBooks = 0, goalYear = 0;
  int minutes[7] = {}, nDays = 0, avgMin = 0, consistency = 0;
  int projBooks = 0, projDone = 0;
  char trend[8] = "";
  char challengeTitle[48] = "";
  int chProgress = 0, chTarget = 1;
  bool chDone = false;
  int books = 0, authors = 0, series = 0, addedYear = 0;
  bool any() const { return readingNow || streak || goal || rhythm || projection || challenge || overview; }
} stats;

static void parseStats(const String& json) {
  stats = Stats();
  JsonDocument doc;
  if (deserializeJson(doc, json)) return;
  if (JsonObject d = doc["reading-now"]; !d.isNull()) {
    stats.readingNow = true;
    stats.nowId = d["id"] | 0L;
    stats.nowFileId = d["fileId"] | 0L;
    strlcpy(stats.nowTitle, d["title"] | "", sizeof(stats.nowTitle));
    strlcpy(stats.nowAuthor, d["author"] | "", sizeof(stats.nowAuthor));
    stats.nowProgress = d["progress"] | 0.0f;
  }
  if (JsonObject d = doc["reading-streak"]; !d.isNull()) {
    stats.streak = true;
    stats.streakDays = d["currentStreak"] | 0;
    stats.longest = d["longestStreak"] | 0;
    int i = 0;
    for (JsonVariant v : d["lastSevenDays"].as<JsonArray>()) if (i < 7) stats.week[i++] = v.as<bool>();
  }
  if (JsonObject d = doc["reading-goal"]; !d.isNull()) {
    stats.goal = true;
    stats.goalDone = d["completedBooks"] | 0;
    stats.goalBooks = d["goalBooks"] | 0;
    stats.goalYear = d["year"] | 0;
  }
  if (JsonObject d = doc["reading-rhythm"]; !d.isNull()) {
    stats.rhythm = true;
    for (JsonObject day : d["days"].as<JsonArray>()) if (stats.nDays < 7) stats.minutes[stats.nDays++] = (int)((day["readingSeconds"] | 0.0) / 60);
    stats.avgMin = (int)((d["avgSecondsPerDay"] | 0.0) / 60);
    stats.consistency = d["consistencyPercent"] | 0;
  }
  if (JsonObject d = doc["year-projection"]; !d.isNull()) {
    stats.projection = true;
    stats.projBooks = d["projectedBooks"] | 0;
    stats.projDone = d["booksCompletedYtd"] | 0;
    strlcpy(stats.trend, d["trend"] | "", sizeof(stats.trend));
  }
  if (JsonObject d = doc["monthly-challenge"]; !d.isNull()) {
    stats.challenge = true;
    strlcpy(stats.challengeTitle, d["title"] | "", sizeof(stats.challengeTitle));
    stats.chProgress = d["progress"] | 0;
    stats.chTarget = max(1, (int)(d["target"] | 1));
    stats.chDone = d["completed"] | false;
  }
  if (JsonObject d = doc["library-overview"]; !d.isNull()) {
    stats.overview = true;
    stats.books = d["totalBooks"] | 0;
    stats.authors = d["totalAuthors"] | 0;
    stats.series = d["totalSeries"] | 0;
    stats.addedYear = d["booksAddedThisYear"] | 0;
  }
}

static void loadStatsCache() {
  if (SdMan.exists(kStatsFile)) parseStats(SdMan.readFile(kStatsFile));
}

// Fetches fresh numbers (online only) and caches them; returns true when the displayed stats changed.
static bool refreshStats() {
  String json, error;
  if (!api.fetchWidgets(kStatTypes, sizeof(kStatTypes) / sizeof(kStatTypes[0]), json, error)) {
    Serial.printf("stats: %s\n", error.c_str());
    return false;
  }
  Book now;
  float nowProgress = 0;
  bool found = false;
  const bool contOk = api.fetchContinueReading(now, nowProgress, found, error);
  Serial.printf("continue reading: %s%s\n", contOk ? (found ? "found: " : "nothing in progress") : "request failed: ", contOk ? (found ? now.title : "") : error.c_str());
  if (contOk && found) {  // add the book being read to the cached document
    JsonDocument doc;
    deserializeJson(doc, json);
    doc["reading-now"]["id"] = now.id;
    doc["reading-now"]["fileId"] = now.fileId;
    doc["reading-now"]["title"] = now.title;
    doc["reading-now"]["author"] = now.author;
    doc["reading-now"]["progress"] = nowProgress;
    json = "";
    serializeJson(doc, json);
  }
  const String before = SdMan.exists(kStatsFile) ? SdMan.readFile(kStatsFile) : String();
  if (json == before) return false;
  SdMan.ensureDirectoryExists("/BookOrbit");
  SdMan.writeFile(kStatsFile, json);
  parseStats(json);
  Serial.println("stats refreshed");
  return true;
}

// Filled rectangle with rounded corners of radius r.
static void roundRect(int x, int y, int w, int h, int r, bool black) {
  for (int j = 0; j < h; j++) {
    const int fromEdge = j < r ? r - j : (j >= h - r ? j - (h - r) + 1 : 0);
    int inset = 0;
    if (fromEdge > 0) inset = r - (int)sqrtf((float)(r * r - (fromEdge - 0.5f) * (fromEdge - 0.5f)));
    gfx::fillRect(x + inset, y + j, w - 2 * inset, 1, black);
  }
}

// A 2 px outline with slightly rounded corners; `solid` fills it black (the highlighted state).
static void frame(int x, int y, int w, int h, int r = 8, bool solid = false) {
  roundRect(x, y, w, h, r, true);
  if (!solid) roundRect(x + 2, y + 2, w - 4, h - 4, max(0, r - 2), false);
}

static void card(int x, int y, int w, int h, const char* title) {
  frame(x, y, w, h);
  gfx::text(x + 10, y + 5, gfx::ellipsize(title, w - 20));
}

static void bar(int x, int y, int w, int h, float fraction, bool inverse = false) {
  if (inverse) {  // white outline and fill on a dark background
    roundRect(x, y, w, h, 4, false);
    roundRect(x + 2, y + 2, w - 4, h - 4, 2, true);
  } else {
    frame(x, y, w, h, 4);
  }
  const int fill = (int)((w - 8) * constrain(fraction, 0.0f, 1.0f));
  if (fill > 0) roundRect(x + 4, y + 4, fill, h - 8, 2, !inverse);
}

static void dot(int cx, int cy, int r, bool filled) {
  for (int dy = -r; dy <= r; dy++)
    for (int dx = -r; dx <= r; dx++) {
      const int d2 = dx * dx + dy * dy;
      if (filled ? d2 <= r * r : (d2 <= r * r && d2 > (r - 2) * (r - 2))) gfx::fillRect(cx + dx, cy + dy, 1, 1, true);
    }
}

// ---- Reading companion (the Android app's pet widget) -----------------------------------------------------------

constexpr const char* kPetFile = "/BookOrbit/.pet";
constexpr const char* kPetBaseFile = "/BookOrbit/.petbase";  // "<date> <books completed that morning>"
int petIndex = 0;

static void loadPet() {
  if (SdMan.exists(kPetFile)) petIndex = constrain(atoi(SdMan.readFile(kPetFile).c_str()), 0, pets::kCount - 1);
}

static void setPet(int index) {
  petIndex = constrain(index, 0, pets::kCount - 1);
  SdMan.ensureDirectoryExists("/BookOrbit");
  SdMan.writeFile(kPetFile, String(petIndex));
}

// Same rules as the Android app and the KOReader plugin: one pose for the current situation.
static pets::Pose petPose(int hour, bool finishedToday) {
  const int streak = stats.streakDays, longest = stats.longest;
  const int todayMin = stats.nDays ? stats.minutes[stats.nDays - 1] : 0;
  const bool milestone = streak == 7 || streak == 30 || streak == 50 || streak == 100 || streak == 200 || streak == 365;
  if (!stats.readingNow && streak == 0) return pets::Wait;
  if (todayMin <= 0) return pets::Sleep;
  if (finishedToday) return pets::Ribbon;
  if (milestone || (streak > 1 && streak >= longest)) return pets::Party;
  if (hour >= 22 || hour < 5) return pets::Yawn;
  return pets::Read;
}

// A book counts as finished today when the year's completed count grew since the day began.
static bool petFinishedToday(const char* today) {
  if (!stats.goal) return false;
  String saved = SdMan.exists(kPetBaseFile) ? SdMan.readFile(kPetBaseFile) : String();
  if (!saved.startsWith(today)) {
    SdMan.ensureDirectoryExists("/BookOrbit");
    SdMan.writeFile(kPetBaseFile, String(today) + " " + String(stats.goalDone));
    return false;
  }
  return stats.goalDone > atoi(saved.c_str() + strlen(today) + 1);
}

static String petDuration(int min) { return min >= 60 ? String(min / 60) + " h " + String(min % 60) + " min" : String(min) + " min"; }

static void drawPet(int x, int y, bool sel) {
  int hour = 12;
  char today[16] = "";
  if (clockValid()) {
    struct tm tmv;
    const time_t now = time(nullptr);
    localtime_r(&now, &tmv);
    hour = tmv.tm_hour;
    strftime(today, sizeof(today), "%Y-%m-%d", &tmv);
  }
  const bool finished = *today && petFinishedToday(today);
  const pets::Pose pose = petPose(hour, finished);
  const int streak = stats.streakDays, todayMin = stats.nDays ? stats.minutes[stats.nDays - 1] : 0;
  String line1, line2;
  switch (pose) {
    case pets::Read: line1 = "Reading with you"; line2 = petDuration(todayMin) + " today"; break;
    case pets::Sleep: line1 = "Fast asleep"; line2 = "Wakes up when you read"; break;
    case pets::Party: line1 = String(streak) + (streak == 1 ? " day" : " days") + " in a row!"; line2 = streak >= stats.longest ? "A new best" : "Your best is " + String(stats.longest); break;
    case pets::Ribbon: line1 = "Book finished!"; break;
    case pets::Yawn: line1 = "Past bedtime"; line2 = petDuration(todayMin) + " read today"; break;
    default: line1 = "Ready when you are"; line2 = "Open a book to start"; break;
  }
  const int w = gfx::W - 2 * kMargin;
  frame(x, y, w, 80, 8, sel);
  roundRect(x + 7, y + 7, 66, 66, 4, false);  // white plate under the sprite
  gfx::blit1bit(x + 8, y + 8, pets::kSprites[petIndex][pose], 64, 64);
  label(x + 84, y + 6, line1.c_str(), sel);
  if (line2.length()) label(x + 84, y + 30, line2.c_str(), sel);
}

void testFakeNow() {  // test hook: pretend a book is in progress
  stats.readingNow = true;
  stats.nowId = 359;  // a book of the real test library
  strlcpy(stats.nowTitle, "Wybaczam ci", sizeof(stats.nowTitle));
  strlcpy(stats.nowAuthor, "Remigiusz Mroz", sizeof(stats.nowAuthor));
  stats.nowProgress = 42;  // press a key afterwards to redraw
}

// The book being read: cover, title, author, progress. Highlighted (dark with light text) when selected.
static void drawReadingNow(int y, bool sel) {
  constexpr int kFull = gfx::W - 2 * kMargin;
  char buf[16];
  frame(kMargin, y, kFull, 96, 8, sel);
  label(kMargin + 68, y + 1, "Reading now", sel);
  roundRect(kMargin + 8, y + 11, cover::W + 4, cover::H + 4, 3, false);  // white plate so the cover shows on dark too
  roundRect(kMargin + 8, y + 11, cover::W + 4, cover::H + 4, 3, true);
  gfx::fillRect(kMargin + 10, y + 13, cover::W, cover::H, false);
  uint8_t bits[cover::kBytes];
  if (stats.nowId && cover::load(stats.nowId, bits)) gfx::blit1bit(kMargin + 10, y + 13, bits, cover::W, cover::H);
  label(kMargin + 68, y + 23, gfx::ellipsize(stats.nowTitle, kFull - 80), sel);
  label(kMargin + 68, y + 44, gfx::ellipsize(stats.nowAuthor, kFull - 80), sel);
  bar(kMargin + 68, y + 79, kFull - 68 - 70, 12, stats.nowProgress / 100.0f, sel);
  snprintf(buf, sizeof(buf), "%d%%", (int)stats.nowProgress);
  label(kMargin + kFull - 62, y + 64, buf, sel);
}

// Home items from the top: Reading now (when there is a book in progress), then Library. homeSel counts them.
static bool readingSelected() { return stats.readingNow && homeSel == 0; }
static bool librarySelected() { return homeSel == (stats.readingNow ? 1 : 0); }

// The stats panel under the Home menu: y from 146 down to 700.
static void drawStats(int y) {
  if (!stats.any()) {
    gfx::text(kMargin, y + 26, "Statistics appear after the first sync.");
    return;
  }
  constexpr int kHalf = 220, kRight = kMargin + kHalf + 8, kFull = gfx::W - 2 * kMargin;
  char buf[64];
  drawPet(kMargin, y, false);  // display only; the companion is chosen in Settings
  y += 86;
  if (stats.streak) {
    card(kMargin, y, kHalf, 124, "Reading streak");
    snprintf(buf, sizeof(buf), "%d day%s", stats.streakDays, stats.streakDays == 1 ? "" : "s");
    gfx::bigText(kMargin + 10, y + 30, buf, 32);
    for (int i = 0; i < 7; i++) dot(kMargin + 18 + i * 29, y + 84, 7, stats.week[i]);
    snprintf(buf, sizeof(buf), "Longest: %d", stats.longest);
    gfx::text(kMargin + 10, y + 92, buf);
  }
  if (stats.goal) {
    snprintf(buf, sizeof(buf), "%d goal", stats.goalYear);
    card(kRight, y, kHalf, 124, buf);
    if (stats.goalBooks > 0) {
      snprintf(buf, sizeof(buf), "%d / %d", stats.goalDone, stats.goalBooks);
      gfx::bigText(kRight + 10, y + 30, buf, 32);
      bar(kRight + 10, y + 74, kHalf - 20, 16, (float)stats.goalDone / stats.goalBooks);
      gfx::text(kRight + 10, y + 92, "books");
    } else {
      snprintf(buf, sizeof(buf), "%d read", stats.goalDone);
      gfx::bigText(kRight + 10, y + 30, buf, 32);
      gfx::text(kRight + 10, y + 78, "No goal set");
    }
  }
  y += 130;
  if (stats.rhythm) {
    card(kMargin, y, kFull, 106, "Reading rhythm");
    int mx = 1;
    for (int i = 0; i < stats.nDays; i++) mx = max(mx, stats.minutes[i]);
    for (int i = 0; i < stats.nDays; i++) {
      const int h = max(3, 36 * stats.minutes[i] / mx);
      roundRect(kMargin + 10 + i * 62, y + 70 - h, 50, h, 3, true);
    }
    snprintf(buf, sizeof(buf), "%d min/day, %d%% of days read", stats.avgMin, stats.consistency);
    gfx::text(kMargin + 10, y + 73, gfx::ellipsize(buf, kFull - 20));
  }
  y += 112;
  if (stats.projection) {
    snprintf(buf, sizeof(buf), "On pace for %d", stats.goalYear);
    card(kMargin, y, kHalf, 124, stats.goalYear ? buf : "On pace for the year");
    snprintf(buf, sizeof(buf), "%d books", stats.projBooks);
    gfx::bigText(kMargin + 10, y + 28, buf, 30);
    snprintf(buf, sizeof(buf), "%d done so far", stats.projDone);
    gfx::text(kMargin + 10, y + 64, buf);
    snprintf(buf, sizeof(buf), "Trending %s", stats.trend);
    if (*stats.trend && strcmp(stats.trend, "flat")) gfx::text(kMargin + 10, y + 92, buf);
  }
  if (stats.challenge) {
    card(kRight, y, kHalf, 124, "Monthly challenge");
    gfx::text(kRight + 10, y + 34, gfx::ellipsize(stats.challengeTitle, kHalf - 20));
    bar(kRight + 10, y + 72, kHalf - 20, 16, (float)stats.chProgress / stats.chTarget);
    snprintf(buf, sizeof(buf), stats.chDone ? "Completed" : "%d / %d", stats.chProgress, stats.chTarget);
    gfx::text(kRight + 10, y + 92, buf);
  }
}

// Home: a menu with a single item for now.
static void drawHome(bool full) {
  gfx::clear();
  header("BookOrbit");
  if (!stats.readingNow && homeSel > 0) homeSel = 0;
  int y = kListTop;
  if (stats.readingNow) {
    drawReadingNow(y, readingSelected());
    y += 102;
  }
  const bool sel = librarySelected();
  frame(kMargin, y, gfx::W - 2 * kMargin, kRowHeight - 4, 8, sel);
  // Library icon: books standing on a shelf, one leaning. Ink is white on the highlighted (black) row.
  const bool ink = !sel;
  const int ix = kMargin + 14, iy = y + 12, base = iy + 44;  // base = the shelf's top edge
  gfx::fillRect(ix - 2, base, 46, 3, ink);                      // shelf
  const int heights[3] = {32, 40, 28};
  for (int i = 0; i < 3; i++) {                                 // three upright books
    const int bx = ix + i * 11, bh = heights[i];  // 8 px wide, 3 px apart
    gfx::fillRect(bx, base - bh, 8, bh, ink);
  }
  for (int r = 0; r < 34; r++) gfx::fillRect(ix + 33 + (34 - r) * 7 / 34, base - 1 - r, 8, 1, ink);  // the leaning book
  if (stats.overview) {  // the library overview lives in the menu row
    char counts[64];
    snprintf(counts, sizeof(counts), "%d books, %d authors, %d series", stats.books, stats.authors, stats.series);
    label(kMargin + 74, y + 6, "Library", sel);
    label(kMargin + 74, y + 34, gfx::ellipsize(counts, gfx::W - kMargin - 12 - (kMargin + 74)), sel);
  } else {
    label(kMargin + 74, y + 20, "Library", sel);
  }
  drawStats(y + kRowHeight - 4 + 6);
  if (net == Net::Connecting) gfx::text(kMargin, gfx::H - 96, "Connecting...", true);
  if (net == Net::Offline) gfx::text(kMargin, gfx::H - 96, "Offline - hold Back to connect", true);
  drawHints(true, true, false, true);
  gfx::flush(!full ? EInkDisplay::FAST_REFRESH : (firstDraw || halfNext) ? EInkDisplay::HALF_REFRESH : EInkDisplay::FULL_REFRESH);
  firstDraw = false;
  halfNext = false;
}

// ---- Settings -------------------------------------------------------------------------------------------------

static const char* wifiValue() {
  if (net == Net::Online) return "On";
  if (net == Net::Connecting) return "Connecting...";
  return SdMan.exists(kWifiOffFlag) ? "Off" : "Not connected";
}

static void setSleep(int minutes) {
  sleepMinutes = minutes;
  SdMan.ensureDirectoryExists("/BookOrbit");
  SdMan.writeFile(kSleepFile, String(sleepMinutes));
}

static void drawSettings(bool full) {
  gfx::clear();
  header("Settings");
  // Rows are grouped under section headings; settingsSel counts rows only.
  const char* sectionOf[8] = {"Wi-Fi", nullptr, "Account", nullptr, nullptr, "Device", nullptr, nullptr};  // heading above the row
  const char* names[8] = {"WiFi radio", "Network", "Server", "Username", "Password", "Keyboard", "Companion", "Sleep after"};
  String sleepText = String(sleepMinutes) + " min";
  String stars;
  for (size_t k = 0; k < config.password.length() && k < 10; k++) stars += '*';
  const String values[8] = {wifiValue(), config.ssid, config.server, config.username, stars, kbStyle ? "Classic T9" : "Key grid", pets::kNames[petIndex], sleepText};
  constexpr int kRowH = 52, kRowPitch = 58, kHeadingH = 34;
  int y = 60;
  for (int i = 0; i < 8; i++) {
    if (sectionOf[i]) {  // a heading: the name in capitals, then a rule to the edge
      String h = sectionOf[i];
      h.toUpperCase();
      gfx::text(kMargin, y + 2, h.c_str());
      gfx::text(kMargin + 1, y + 2, h.c_str());  // a second pass makes it bolder
      const int w = gfx::textWidth(h.c_str()) + 10;
      gfx::fillRect(kMargin + w, y + 16, gfx::W - 2 * kMargin - w, 2, true);
      y += kHeadingH;
    }
    const bool sel = i == settingsSel;
    if (sel) roundRect(kMargin, y, gfx::W - 2 * kMargin, kRowH, 8, true); else frame(kMargin, y, gfx::W - 2 * kMargin, kRowH);
    gfx::text(kMargin + 14, y + 11, names[i], !sel);
    const int valueX = gfx::W - kMargin - 14;
    const char* v = gfx::ellipsize(values[i].c_str(), valueX - (kMargin + 14 + gfx::textWidth(names[i])) - 16);
    gfx::text(valueX - gfx::textWidth(v), y + 11, v, !sel);
    y += kRowPitch;
  }
  // About: read-only facts.
  gfx::text(kMargin, y + 2, "ABOUT");
  gfx::text(kMargin + 1, y + 2, "ABOUT");
  gfx::fillRect(kMargin + gfx::textWidth("ABOUT") + 10, y + 16, gfx::W - 2 * kMargin - gfx::textWidth("ABOUT") - 10, 2, true);
  char line[96];
  snprintf(line, sizeof(line), "Battery: %d%%", (int)battery.readPercentage());
  gfx::text(kMargin, y + kHeadingH, line);
  drawHints(true, true, false);
  gfx::flush(!full ? EInkDisplay::FAST_REFRESH : (firstDraw || halfNext) ? EInkDisplay::HALF_REFRESH : EInkDisplay::FULL_REFRESH);
  firstDraw = false;
  halfNext = false;
}

// A list to choose one value from (companion or sleep timer), opened from Settings.
constexpr int kPickerRows = 9, kPickerRowH = 70;

static int pickerCount() { return pickerKind == 0 ? pets::kCount : pickerKind == 2 ? 2 : (int)(sizeof(kSleepOptions) / sizeof(kSleepOptions[0])); }
static String pickerLabel(int i) { return pickerKind == 0 ? String(pets::kNames[i]) : pickerKind == 2 ? String(i == 0 ? "Key grid" : "Classic T9 (multi-tap)") : String(kSleepOptions[i]) + (kSleepOptions[i] == 1 ? " minute" : " minutes"); }
static int pickerCurrent() {
  if (pickerKind == 0) return petIndex;
  if (pickerKind == 2) return kbStyle;
  for (int i = 0; i < pickerCount(); i++) if (kSleepOptions[i] == sleepMinutes) return i;
  return 0;
}

static void openPicker(int kind) {
  pickerKind = kind;
  pickerSel = pickerCurrent();
  pickerTop = max(0, min(pickerSel - kPickerRows / 2, pickerCount() - kPickerRows));
  screen = Screen::Picker;
}

static void drawPicker(bool full) {
  gfx::clear();
  header(pickerKind == 0 ? "Companion" : pickerKind == 2 ? "Keyboard" : "Sleep after");
  const int n = pickerCount(), cur = pickerCurrent();
  for (int r = 0; r < kPickerRows && pickerTop + r < n; r++) {
    const int i = pickerTop + r, y = kListTop + r * kPickerRowH;
    const bool sel = i == pickerSel;
    if (sel) roundRect(kMargin, y, gfx::W - 2 * kMargin, kPickerRowH - 6, 8, true);
    else frame(kMargin, y, gfx::W - 2 * kMargin, kPickerRowH - 6);
    gfx::text(kMargin + 14, y + 16, pickerLabel(i).c_str(), !sel);
    if (i == cur) {  // a tick marks the value in use
      const int tx = gfx::W - kMargin - 40, ty = y + 22;
      for (int k = 0; k < 6; k++) gfx::fillRect(tx + k, ty + 6 + k, 3, 3, !sel);
      for (int k = 0; k < 13; k++) gfx::fillRect(tx + 5 + k, ty + 11 - k * 15 / 13, 3, 3, !sel);
    }
  }
  if (n > kPickerRows) {  // position among the entries
    char pos[16];
    snprintf(pos, sizeof(pos), "%d/%d", pickerSel + 1, n);
    gfx::text(gfx::W - kMargin - gfx::textWidth(pos), gfx::H - 96, pos);
  }
  drawHints(true, true, false);
  gfx::flush(!full ? EInkDisplay::FAST_REFRESH : (firstDraw || halfNext) ? EInkDisplay::HALF_REFRESH : EInkDisplay::FULL_REFRESH);
  firstDraw = false;
  halfNext = false;
}

// Screen changes are only scheduled here; loop() draws them once the panel is idle (pumpDisplay). Buttons therefore never
// wait for a refresh, and several quick presses collapse into one redraw of the latest state.
static bool redrawPending = false, redrawFull = false;
static void showList(bool full);

// ---- Wi-Fi network: scan, pick, type the password ----------------------------------------------------------------

struct ScanEntry {
  char ssid[33];
  int rssi;
  bool open;
};
constexpr int kMaxScan = 12;
ScanEntry scanList[kMaxScan];
int scanCount = 0, netSel = 0, netTop = 0;
String pendingSsid, kbText;
enum KbTarget { KbSsid, KbWifiPassword, KbServer, KbUsername, KbPassword };
int kbTarget = KbWifiPassword;  // what the keyboard is typing
int kbMode = 0;          // 0 lower case, 1 upper case (one letter), 2 symbols
int kbRow = 1, kbCol = 0;

// Joins the saved network again (used after the credentials change or a scan).
static void startWifi() {
  SdMan.remove(kWifiOffFlag);
  WiFi.disconnect(true);
  WiFi.mode(WIFI_STA);
    WiFi.setSleep(false);  // modem sleep delays inbound data by seconds on some access points
  WiFi.begin(config.ssid.c_str(), config.wifiPassword.c_str());
  net = Net::Connecting;
  netStart = millis();
}

// Stores the credentials in /bookorbit.json (other settings there are kept) and starts connecting with them.
static void saveWifiCreds(const String& ssid, const String& password) {
  JsonDocument doc;
  if (SdMan.exists("/bookorbit.json")) deserializeJson(doc, SdMan.readFile("/bookorbit.json"));
  doc["ssid"] = ssid;
  doc["wifiPassword"] = password;
  String json;
  serializeJsonPretty(doc, json);
  SdMan.writeFile("/bookorbit.json", json);
  config.ssid = ssid;
  config.wifiPassword = password;
  startWifi();
}

// A server address as typed -> the form we store (no trailing slash); empty when it is not a usable http(s):// address.
static String normalizeServer(String value) {
  value.trim();
  while (value.endsWith("/")) value.remove(value.length() - 1);
  if ((!value.startsWith("http://") && !value.startsWith("https://")) || value.length() < 12) return "";
  return value;
}

// Switching to another server removes everything that belongs to the old one: downloaded books (and their reading
// caches), the queued progress and sessions (their file ids mean nothing on the new server), library list, stats.
static void clearServerData() {
  SdMan.removeDir("/BookOrbit/.covers");
  SdMan.removeDir("/BookOrbit/.library");
  SdMan.removeDir("/BookOrbit/.cache");
  SdMan.remove(kStatsFile);
  SdMan.remove(kOutbox);
  SdMan.remove(kSessionOutbox);
  for (const String& name : SdMan.listFiles("/BookOrbit", 500)) {
    String lower = name;
    lower.toLowerCase();
    if (lower.endsWith(".epub") || lower.endsWith(".part")) SdMan.remove((String("/BookOrbit/") + name).c_str());
  }
  stats = Stats();
  bookCount = bookTotal = 0;
  page = selected = homeSel = 0;
}

String pendingServer;
int confirmSel = 0;  // 0 = Cancel, 1 = Switch and clear

// Server address, username and password: stored in /bookorbit.json, then the app signs in again with them.
static bool saveAccountField(int target, String value) {
  if (target == KbServer) {
    value = normalizeServer(value);
    if (value.isEmpty()) return false;
  }
  JsonDocument doc;
  if (SdMan.exists("/bookorbit.json")) deserializeJson(doc, SdMan.readFile("/bookorbit.json"));
  bool serverChanged = false;
  if (target == KbServer) {
    serverChanged = value != config.server;
    doc["server"] = value;
    config.server = value;
  } else if (target == KbUsername) {
    doc["username"] = value;
    config.username = value;
  } else {
    doc["password"] = value;
    config.password = value;
  }
  String json;
  serializeJsonPretty(doc, json);
  SdMan.writeFile("/bookorbit.json", json);
  api.configure(config.server, config.insecureTls);
  if (serverChanged) clearServerData();  // another server: its books and numbers do not apply
  if (!SdMan.exists(kWifiOffFlag)) {  // sign in again in the background
    if (WiFi.getMode() == WIFI_OFF) startWifi();
    net = Net::Connecting;
    netStart = millis();
  }
  return true;
}

static void scanForNetworks() {
  message("Wi-Fi", "Scanning for networks...", false);
  const bool wasOff = WiFi.getMode() == WIFI_OFF;
  if (wasOff) WiFi.mode(WIFI_STA);
    WiFi.setSleep(false);  // modem sleep delays inbound data by seconds on some access points
  int n = WiFi.scanNetworks(false, false);
  if (n < 0) {  // busy joining a network: stop that and scan again
    WiFi.disconnect();
    delay(300);
    n = WiFi.scanNetworks(false, false);
  }
  scanCount = 0;
  for (int i = 0; i < n; i++) {  // strongest first (the driver sorts by signal), one entry per name
    const String name = WiFi.SSID(i);
    if (name.isEmpty()) continue;
    bool dup = false;
    for (int k = 0; k < scanCount; k++) if (name == scanList[k].ssid) dup = true;
    if (dup || scanCount >= kMaxScan) continue;
    strlcpy(scanList[scanCount].ssid, name.c_str(), sizeof(scanList[0].ssid));
    scanList[scanCount].rssi = WiFi.RSSI(i);
    scanList[scanCount].open = WiFi.encryptionType(i) == WIFI_AUTH_OPEN;
    scanCount++;
  }
  WiFi.scanDelete();
  if (wasOff) WiFi.mode(WIFI_OFF);
}

static void openNetworks() {
  scanForNetworks();
  netSel = 0;
  netTop = 0;
  screen = Screen::Networks;
  showList(false);
}

static void drawNetworks(bool full) {
  gfx::clear();
  header("Wi-Fi network");
  const int n = scanCount + 1;  // the last row types a name by hand
  for (int r = 0; r < kPickerRows && netTop + r < n; r++) {
    const int i = netTop + r, y = kListTop + r * kPickerRowH;
    const bool sel = i == netSel;
    if (sel) roundRect(kMargin, y, gfx::W - 2 * kMargin, kPickerRowH - 6, 8, true);
    else frame(kMargin, y, gfx::W - 2 * kMargin, kPickerRowH - 6);
    if (i < scanCount) {
      const ScanEntry& e = scanList[i];
      const bool current = config.ssid == e.ssid;
      const char* extra = current ? "In use" : (e.open ? "Open" : "");
      gfx::text(kMargin + 14, y + 16, gfx::ellipsize(e.ssid, 300), !sel);
      if (*extra) gfx::text(gfx::W - kMargin - 14 - gfx::textWidth(extra), y + 16, extra, !sel);
    } else {
      gfx::text(kMargin + 14, y + 16, "Other network...", !sel);
    }
  }
  if (scanCount == 0) gfx::text(kMargin, gfx::H - 130, "No networks found.");
  drawHints(true, true, false);
  gfx::flush(!full ? EInkDisplay::FAST_REFRESH : EInkDisplay::FULL_REFRESH);
}

constexpr const char* kKbRows[3][4] = {
    {"1234567890", "qwertyuiop", "asdfghjkl-", "zxcvbnm._@"},
    {"1234567890", "QWERTYUIOP", "ASDFGHJKL-", "ZXCVBNM._@"},
    {"!@#$%^&*()", "-_=+[]{};:", "'\"<>,./?\\|", "~`:;#&*()+"},
};
constexpr int kKeyW = 44, kKeyH = 52, kKeyPitch = 58, kKbTop = 330, kKbX = 20;
// Bottom row: mode switch, space, cancel, done.
constexpr int kBottomX[4] = {20, 130, 268, 368}, kBottomW[4] = {100, 128, 90, 92};

static void t9Reset();
static void drawT9(bool full);

static void openKeyboard(int target, const String& startText) {
  kbTarget = target;
  kbText = startText;
  kbMode = 0;
  kbRow = 1;
  kbCol = 0;
  t9Reset();
  screen = Screen::Keyboard;
  showList(false);
}

static void drawKeyboard(bool full) {
  if (kbStyle == 1) {
    drawT9(full);
    return;
  }
  gfx::clear();
  static const char* const titles[] = {"Network name", "Wi-Fi password", "Server address", "Username", "Password"};
  header(titles[kbTarget]);
  if (kbTarget == KbWifiPassword) gfx::text(kMargin, 66, gfx::ellipsize(pendingSsid.c_str(), gfx::W - 2 * kMargin));
  if (kbTarget == KbServer) gfx::text(kMargin, 66, "e.g. http://192.168.1.4:3000");
  frame(kMargin, 110, gfx::W - 2 * kMargin, 60);
  // The text field shows the end of what was typed, with a cursor.
  String shown = kbText;
  while (shown.length() > 1 && gfx::textWidth(shown.c_str()) > gfx::W - 2 * kMargin - 40) shown.remove(0, 1);
  shown += "_";
  gfx::text(kMargin + 14, 124, shown.c_str());
  gfx::text(kMargin, 184, kbMode == 1 ? "Capital letter" : kbMode == 2 ? "Symbols" : "Back deletes");
  for (int r = 0; r < 4; r++) {
    const char* keys = kKbRows[kbMode][r];
    for (int c = 0; c < 10; c++) {
      const int x = kKbX + c * kKeyW, y = kKbTop + r * kKeyPitch;
      const bool sel = r == kbRow && c == kbCol;
      if (sel) roundRect(x + 2, y, kKeyW - 4, kKeyH, 6, true); else frame(x + 2, y, kKeyW - 4, kKeyH, 6);
      const char key[2] = {keys[c], 0};
      gfx::text(x + (kKeyW - gfx::textWidth(key)) / 2, y + 11, key, !sel);
    }
  }
  const char* modeNext = kbMode == 0 ? "ABC" : kbMode == 1 ? "123" : "abc";
  const char* labels[4] = {modeNext, "Space", "Cancel", "Done"};
  for (int k = 0; k < 4; k++) {
    const int y = kKbTop + 4 * kKeyPitch;
    const bool sel = kbRow == 4 && kbCol == k;
    if (sel) roundRect(kBottomX[k], y, kBottomW[k], kKeyH, 6, true); else frame(kBottomX[k], y, kBottomW[k], kKeyH, 6);
    gfx::text(kBottomX[k] + (kBottomW[k] - gfx::textWidth(labels[k])) / 2, y + 11, labels[k], !sel);
  }
  drawHints(true, true, true);
  gfx::flush(!full ? EInkDisplay::FAST_REFRESH : EInkDisplay::FULL_REFRESH);
}

static void keyboardMove(int dr, int dc) {
  if (dr) {
    const int old = kbRow;
    kbRow = constrain(kbRow + dr, 0, 4);
    if (kbRow == 4 && old != 4) kbCol = constrain(kbCol * 4 / 10, 0, 3);  // keep roughly the same x
    if (old == 4 && kbRow != 4) kbCol = constrain((kBottomX[kbCol] + kBottomW[kbCol] / 2 - kKbX) / kKeyW, 0, 9);
  }
  if (dc) kbCol = constrain(kbCol + dc, 0, kbRow == 4 ? 3 : 9);
}

// Cancel (done = false) or accept (done = true) what was typed. Returns true when the keyboard was closed.
static bool keyboardFinish(bool done) {
  if (!done) {
    screen = (kbTarget == KbSsid || kbTarget == KbWifiPassword) ? Screen::Networks : Screen::Settings;
    return true;
  }
  if (kbTarget == KbSsid) {
    if (kbText.isEmpty()) return false;
    pendingSsid = kbText;
    openKeyboard(KbWifiPassword, "");  // now the password
  } else if (kbTarget == KbWifiPassword) {
    saveWifiCreds(pendingSsid, kbText);
    screen = Screen::Settings;
  } else if (kbTarget == KbServer) {
    const String value = normalizeServer(kbText);
    if (value.isEmpty()) return false;  // not an http(s):// address
    if (value != config.server) {       // a different server: ask before clearing anything
      pendingServer = value;
      confirmSel = 0;
      screen = Screen::Confirm;
      return true;
    }
    screen = Screen::Settings;
  } else {
    if (!saveAccountField(kbTarget, kbText)) return false;
    screen = Screen::Settings;
  }
  return true;
}

// OK on a key of the grid keyboard. Returns true when the keyboard was closed.
static bool keyboardPress() {
  if (kbRow < 4) {
    kbText += kKbRows[kbMode][kbRow][kbCol];
    if (kbMode == 1) kbMode = 0;  // capitals are one-shot
    return false;
  }
  switch (kbCol) {
    case 0: kbMode = (kbMode + 1) % 3; return false;
    case 1: kbText += ' '; return false;
    case 2: return keyboardFinish(false);
    default: return keyboardFinish(true);
  }
}

// ---- Classic T9: a phone keypad where pressing a key repeatedly cycles through its letters (multi-tap) ----------------

constexpr const char* kT9Chars[12] = {".,'-_@/:1", "abc2", "def3", "ghi4", "jkl5", "mno6", "pqrs7", "tuv8", "wxyz9", "", " 0", "!?#$%&*()+=;\"~|<>[]{}^`"};
constexpr const char* kT9Labels[12] = {"1  .,@", "2  abc", "3  def", "4  ghi", "5  jkl", "6  mno", "7  pqrs", "8  tuv", "9  wxyz", "*", "0  space", "#  !?&"};
constexpr uint32_t kT9RepeatMs = 1100;  // the same key again within this time picks the next letter
int t9Row = 1, t9Col = 1, t9Mode = 0;   // 0 abc, 1 Abc (next letter capital), 2 ABC
int t9LastKey = -1, t9Index = 0;
bool t9Capital = false;
uint32_t t9LastTap = 0;

static void t9Reset() {
  t9Row = 1;
  t9Col = 1;
  t9Mode = 0;
  t9LastKey = -1;
}

static void t9Move(int dr, int dc) {
  t9LastKey = -1;  // moving away ends the current letter
  t9Row = constrain(t9Row + dr, 0, 4);
  t9Col = constrain(t9Col + dc, 0, t9Row == 4 ? 1 : 2);
}

static bool t9Press() {
  if (t9Row == 4) return keyboardFinish(t9Col == 1);  // Cancel | Done
  const int id = t9Row * 3 + t9Col;                   // 0..11 for the keys 1-9, *, 0, #
  if (id == 9) {                                      // * switches the letter case
    t9Mode = (t9Mode + 1) % 3;
    t9LastKey = -1;
    return false;
  }
  const char* chars = kT9Chars[id];
  const int n = strlen(chars);
  if (id == t9LastKey && kbText.length() && millis() - t9LastTap < kT9RepeatMs) {
    t9Index = (t9Index + 1) % n;
    kbText.remove(kbText.length() - 1);  // replace the previous letter with the next one on this key
  } else {
    t9Index = 0;
    t9Capital = t9Mode != 0;
    if (t9Mode == 1) t9Mode = 0;
  }
  char c = chars[t9Index];
  if (t9Capital && c >= 'a' && c <= 'z') c = c - 'a' + 'A';
  kbText += c;
  t9LastKey = id;
  t9LastTap = millis();
  return false;
}

static void drawT9(bool full) {
  gfx::clear();
  static const char* const titles[] = {"Network name", "Wi-Fi password", "Server address", "Username", "Password"};
  header(titles[kbTarget]);
  if (kbTarget == KbWifiPassword) gfx::text(kMargin, 66, gfx::ellipsize(pendingSsid.c_str(), gfx::W - 2 * kMargin));
  if (kbTarget == KbServer) gfx::text(kMargin, 66, "e.g. http://192.168.1.4:3000");
  frame(kMargin, 110, gfx::W - 2 * kMargin, 60);
  String shown = kbText;
  while (shown.length() > 1 && gfx::textWidth(shown.c_str()) > gfx::W - 2 * kMargin - 40) shown.remove(0, 1);
  shown += "_";
  gfx::text(kMargin + 14, 124, shown.c_str());
  gfx::text(kMargin, 184, "Press a key again for the next letter");
  constexpr int kCellW = 144, kCellH = 64, kPitchY = 72, kTop = 230, kLeft = 16;
  for (int r = 0; r < 4; r++)
    for (int c = 0; c < 3; c++) {
      const int id = r * 3 + c, x = kLeft + c * (kCellW + 8), y = kTop + r * kPitchY;
      const bool sel = r == t9Row && c == t9Col;
      if (sel) roundRect(x, y, kCellW, kCellH, 8, true); else frame(x, y, kCellW, kCellH, 8);
      String label = kT9Labels[id];
      if (id == 9) label = t9Mode == 0 ? "*  abc" : t9Mode == 1 ? "*  Abc" : "*  ABC";
      else if (t9Mode == 2) label.toUpperCase();
      gfx::text(x + (kCellW - gfx::textWidth(label.c_str())) / 2, y + 17, label.c_str(), !sel);
    }
  const char* labels[2] = {"Cancel", "Done"};
  for (int k = 0; k < 2; k++) {
    const int y = kTop + 4 * kPitchY, w = (3 * kCellW + 16) / 2 - 4;
    const int bx = kLeft + k * (w + 8);
    const bool sel = t9Row == 4 && t9Col == k;
    if (sel) roundRect(bx, y, w, kCellH, 8, true); else frame(bx, y, w, kCellH, 8);
    gfx::text(bx + (w - gfx::textWidth(labels[k])) / 2, y + 17, labels[k], !sel);
  }
  drawHints(true, true, true);
  gfx::flush(!full ? EInkDisplay::FAST_REFRESH : EInkDisplay::FULL_REFRESH);
}

// ---- Switching server: confirm, then clear --------------------------------------------------------------------------

// Draws `text` word-wrapped into `w` pixels from (x, y); returns the y below the last line.
static int wrapText(int x, int y, int w, const String& text) {
  String line;
  int start = 0;
  while (start <= (int)text.length()) {
    int sp = text.indexOf(' ', start);
    if (sp < 0) sp = text.length();
    const String word = text.substring(start, sp);
    const String tryLine = line.isEmpty() ? word : line + " " + word;
    if (gfx::textWidth(tryLine.c_str()) > w && !line.isEmpty()) {
      gfx::text(x, y, line.c_str());
      y += gfx::lineHeight();
      line = word;
    } else {
      line = tryLine;
    }
    start = sp + 1;
  }
  if (!line.isEmpty()) {
    gfx::text(x, y, line.c_str());
    y += gfx::lineHeight();
  }
  return y;
}

static void drawConfirm(bool full) {
  gfx::clear();
  header("Switch server?");
  int y = 70;
  gfx::text(kMargin, y, "From");
  gfx::text(kMargin + 80, y, gfx::ellipsize(config.server.c_str(), gfx::W - 2 * kMargin - 80));
  y += gfx::lineHeight() + 4;
  gfx::text(kMargin, y, "To");
  gfx::text(kMargin + 80, y, gfx::ellipsize(pendingServer.c_str(), gfx::W - 2 * kMargin - 80));
  y += gfx::lineHeight() + 20;
  y = wrapText(kMargin, y, gfx::W - 2 * kMargin,
               "Switching removes everything that came from the old server: downloaded books, statistics and reading "
               "positions waiting to sync. Books you copied to the card yourself are not touched. After switching, set your "
               "username and password for the new server.");
  const char* labels[2] = {"Cancel", "Switch and clear data"};
  for (int i = 0; i < 2; i++) {
    const int by = y + 24 + i * 72;
    const bool sel = i == confirmSel;
    if (sel) roundRect(kMargin, by, gfx::W - 2 * kMargin, 60, 8, true); else frame(kMargin, by, gfx::W - 2 * kMargin, 60);
    gfx::text(kMargin + (gfx::W - 2 * kMargin - gfx::textWidth(labels[i])) / 2, by + 15, labels[i], !sel);
  }
  drawHints(true, true, false);
  gfx::flush(!full ? EInkDisplay::FAST_REFRESH : EInkDisplay::FULL_REFRESH);
}

static void switchServer() {
  message("Switching server", "Removing the old server's data...\nThis can take a moment.", false);
  saveAccountField(KbServer, pendingServer);  // stores the address, clears the data and signs in again in the background
  screen = Screen::Settings;
  showList(false);
}

static void networkChosen() {
  if (netSel >= scanCount) {  // other network: type its name first
    pendingSsid = "";
    openKeyboard(KbSsid, "");
    return;
  }
  pendingSsid = scanList[netSel].ssid;
  if (scanList[netSel].open) {
    saveWifiCreds(pendingSsid, "");
    screen = Screen::Settings;
    showList(false);
  } else {
    openKeyboard(KbWifiPassword, "");
  }
}

// The Library submenu: all books, or browse by author, series or smart scope.
static void drawBrowse(bool full) {
  gfx::clear();
  header("Library");
  const char* names[4] = {"All", "Authors", "Series", "Smart scopes"};
  for (int i = 0; i < 4; i++) {
    const int y = kListTop + i * kRowHeight;
    const bool sel = i == browseSel;
    if (sel) gfx::fillRect(0, y, gfx::W, kRowHeight - 4, true);
    char sub[32] = "";
    const int n = i == 0 ? stats.books : i == 1 ? stats.authors : i == 2 ? stats.series : 0;
    if (n > 0) snprintf(sub, sizeof(sub), "%d %s", n, i == 0 ? "books" : i == 1 ? "authors" : "series");
    if (i == 3) strlcpy(sub, "Saved filters", sizeof(sub));
    label(kMargin + 4, y + 4, names[i], sel);
    label(kMargin + 4, y + 4 + gfx::lineHeight() - 2, sub, sel);
  }
  if (net == Net::Offline) gfx::text(kMargin, gfx::H - 96, "Offline - hold Back to connect", true);
  drawHints(true, true, false);
  gfx::flush(!full ? EInkDisplay::FAST_REFRESH : (firstDraw || halfNext) ? EInkDisplay::HALF_REFRESH : EInkDisplay::FULL_REFRESH);
  firstDraw = false;
  halfNext = false;
}

static void drawEntities(bool full) {
  gfx::clear();
  char title[48];
  const int pages = max(1, (entTotal + kPageSize - 1) / kPageSize);
  const char* what = entKind == Scope::Author ? "Authors" : entKind == Scope::Series ? "Series" : "Smart scopes";
  snprintf(title, sizeof(title), "%s  %d/%d", what, entPage + 1, pages);
  header(title);
  for (int i = 0; i < entCount; i++) {
    const int y = kListTop + i * kRowHeight;
    const bool sel = i == entSel;
    if (sel) gfx::fillRect(0, y, gfx::W, kRowHeight - 4, true);
    char sub[24];
    snprintf(sub, sizeof(sub), entities[i].count == 1 ? "%d book" : "%d books", entities[i].count);
    label(kMargin + 4, y + 4, gfx::ellipsize(entities[i].name, gfx::W - 2 * kMargin - 8), sel);
    label(kMargin + 4, y + 4 + gfx::lineHeight() - 2, sub, sel);
  }
  if (entCount == 0) gfx::text(kMargin, 120, "Nothing here");
  if (net == Net::Offline) gfx::text(kMargin, gfx::H - 96, "Offline - hold Back to connect", true);
  drawHints(true, true, true);
  gfx::flush(!full ? EInkDisplay::FAST_REFRESH : (firstDraw || halfNext) ? EInkDisplay::HALF_REFRESH : EInkDisplay::FULL_REFRESH);
  firstDraw = false;
  halfNext = false;
}

static void drawScreen(bool full) {  // draws whichever screen is current
  // Quick refreshes only: no periodic full-screen cleanup (a full refresh flashes the whole panel).
  if (screen == Screen::Home) drawHome(full);
  else if (screen == Screen::Browse) drawBrowse(full);
  else if (screen == Screen::Entities) drawEntities(full);
  else if (screen == Screen::Settings) drawSettings(full);
  else if (screen == Screen::Picker) drawPicker(full);
  else if (screen == Screen::Networks) drawNetworks(full);
  else if (screen == Screen::Keyboard) drawKeyboard(full);
  else if (screen == Screen::Confirm) drawConfirm(full);
  else drawList(full);
}

static void showList(bool full) {
  redrawPending = true;
  redrawFull |= full;
}

static void pumpDisplay() {
  if (!redrawPending || gfx::busy()) return;
  const bool full = redrawFull;
  redrawPending = redrawFull = false;
  drawScreen(full);
}

// ---- Actions -------------------------------------------------------------------------------------------------

static bool connectWifi(String& error) {
  WiFi.mode(WIFI_STA);
    WiFi.setSleep(false);  // modem sleep delays inbound data by seconds on some access points
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
static String cachePath(int p) {
  if (src == Scope::All) return "/BookOrbit/.library/9-" + String(p) + ".json";
  return "/BookOrbit/.library/s" + String((int)src) + "-" + String(srcId) + "-" + String(p) + ".json";
}

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
      const int n = api.listBooks(newPage, kPageSize, books, kPageSize, total, netError, src, srcId);
      if (n >= 0) {
        bookCount = n;
        bookTotal = total;
        page = newPage;
        selected = 0;
        saveCachedPage(newPage);
        return true;
      }
    }
    Serial.printf("library page %d: %s\n", newPage, netError.c_str());
    if (!loadCachedPage(newPage)) {
      error = netError;
      return false;
    }
    // Only a lost connection puts the app offline; a one-off bad response just shows the cached page this time.
    if (WiFi.status() != WL_CONNECTED || netError.startsWith("Cannot reach")) net = Net::Offline;
    offlineReason = netError;
    return true;
  }
  if (loadCachedPage(newPage)) return true;
  error = "This page is not cached.\nHold Back to connect.";
  return false;
}

static String entityCachePath(Scope kind, int p) { return "/BookOrbit/.library/e" + String((int)kind) + "-" + String(p) + ".json"; }

static bool loadCachedEntities(Scope kind, int p) {
  const String path = entityCachePath(kind, p);
  if (!SdMan.exists(path.c_str())) return false;
  JsonDocument doc;
  if (deserializeJson(doc, SdMan.readFile(path.c_str()))) return false;
  entTotal = doc["total"] | 0;
  entCount = 0;
  for (JsonObject o : doc["items"].as<JsonArray>()) {
    if (entCount >= kPageSize) break;
    Entity& e = entities[entCount++];
    e = Entity();
    e.id = o["id"] | 0L;
    strlcpy(e.name, o["name"] | "", sizeof(e.name));
    e.count = o["count"] | 0;
  }
  entKind = kind;
  entLoaded = kind;
  entPage = p;
  entSel = 0;
  return true;
}

// One page of authors / series / smart scopes, from the server when online (and cached), else from the cache.
static bool loadEntities(Scope kind, int p, String& error) {
  if (net == Net::Online) {
    String netError;
    int total = 0;
    if (ensureWifi(netError)) {
      const int n = api.listEntities(kind, p, kPageSize, entities, kPageSize, total, netError);
      if (n >= 0) {
        entCount = n;
        entTotal = total;
        entKind = kind;
        entLoaded = kind;
        entPage = p;
        entSel = 0;
        JsonDocument doc;
        doc["total"] = total;
        for (int i = 0; i < n; i++) {
          doc["items"][i]["id"] = entities[i].id;
          doc["items"][i]["name"] = entities[i].name;
          doc["items"][i]["count"] = entities[i].count;
        }
        String json;
        serializeJson(doc, json);
        SdMan.ensureDirectoryExists("/BookOrbit");
        SdMan.ensureDirectoryExists("/BookOrbit/.library");
        SdMan.writeFile(entityCachePath(kind, p).c_str(), json);
        return true;
      }
    }
    Serial.printf("browse %d page %d: %s\n", (int)kind, p, netError.c_str());
    if (loadCachedEntities(kind, p)) return true;
    error = netError;
    return false;
  }
  if (loadCachedEntities(kind, p)) return true;
  error = "This list is not cached.\nHold Back to connect.";
  return false;
}

static void downloadProgress(size_t done, size_t total) {
  static uint32_t last = 0;
  if (millis() - last < 3000) return;
  last = millis();
  Serial.printf("download %u/%u bytes\n", (unsigned)done, (unsigned)total);
}

static void waitForKey() {
  anyCount = 0;
  do {
    delay(20);
  } while (anyCount == 0 && injected < 0);
  injected = -1;
}

// A framed box over whatever is on screen, for short status notices.
static void syncModal(const char* title, const String& body) { gfx::modal(title, body.c_str()); }

static void backgroundSync();

void testSyncModal() { syncModal("In sync", "Server position: 12%"); }  // test hook

// Opens the selected book, downloading it first when it isn't on the SD card yet.
static void openBook(const Book& b) {
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
  openLibraryMagic = screen == Screen::Library ? kLibraryMagic : 0;  // Back from the book returns to where it was opened
  openScope = (int32_t)src;
  openScopeId = (int32_t)srcId;
  strlcpy(openScopeName, srcName, sizeof(openScopeName));
  readFileId = b.fileId;
  readServerPercent = -1;
  if (b.fileId && net != Net::Offline) {
    syncModal("Syncing", "Signing in...");
    const uint32_t t0 = millis();
    while (net == Net::Connecting && millis() - t0 < 25000) {  // still signing in: wait, so the server's position isn't missed
      backgroundSync();
      delay(50);
    }
    if (net == Net::Online) {
      syncModal("Syncing", "Checking your reading position...");
      float serverPct;
      if (ensureWifi(error) && api.getProgress(b.fileId, serverPct, error)) {
        readServerPercent = serverPct;
        char line[48];
        snprintf(line, sizeof(line), "Server position: %.0f%%", serverPct);
        syncModal("In sync", line);
      } else {
        syncModal("Sync failed", "Using the position on this device.");
      }
      Serial.printf("server progress for file %ld: %.1f%% (%s)\n", b.fileId, readServerPercent, error.c_str());
    } else {
      syncModal("Offline", "Using the position on this device.");
    }
    delay(900);
  }
  strlcpy(readPath, path.c_str(), sizeof(readPath));
  Serial.printf("restarting into reader: %s\n", readPath);
  delay(100);
  ESP.restart();
}

static void openSelected() {
  if (bookCount > 0) openBook(books[selected]);
}

// The book shown in the Home "Reading now" card.
static void openReadingNow() {
  Book b;
  b.id = stats.nowId;
  b.fileId = stats.nowFileId;
  strlcpy(b.title, stats.nowTitle, sizeof(b.title));
  strlcpy(b.author, stats.nowAuthor, sizeof(b.author));
  openBook(b);
}

static void drawSleepScreen() {
  gfx::loadUiFont();
  gfx::clear();  // light page, dark text
  // The companion, asleep, enlarged three times.
  loadPet();
  constexpr int kScale = 3, kSize = 64 * kScale, kPetY = 188;
  const int petX = (gfx::W - kSize) / 2;
  const uint8_t* sprite = pets::kSprites[petIndex][pets::Sleep];
  for (int y = 0; y < 64; y++)
    for (int x = 0; x < 64; x++)
      if (sprite[y * 8 + x / 8] & (0x80 >> (x & 7))) gfx::fillRect(petX + x * kScale, kPetY + y * kScale, kScale, kScale, true);
  gfx::bigTextCentered(gfx::H / 2 + 40, "BookOrbit", 80);
  gfx::bigTextCentered(gfx::H / 2 + 160, "Zzz...", 28);
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
  gfx::waitIdle();
  display.deepSleep();
  freeink::PowerManager::deepSleepUntilPowerButton();
}

static void closeBook() {
  reader::uiFont(true);  // the reader's own face draws the modal (the UI font isn't loaded while reading)
  syncModal("Closing book", "Saving your position...");
  sessionTick();
  checkpoint();
  reader::close();
  readFileId = 0;
  ESP.restart();  // back to where the book was opened from (see openBook)
}

// The reader menu (OK while reading): Contents, Text size and Margins. Up/Down pick, OK chooses, Back goes up a level.
enum class RMenu { None, Main, Size, Margin, Toc };
static RMenu readerMenu = RMenu::None;
static int readerMenuSel = 0, tocTop = 0;
static int mainItems[3], mainCount = 0;  // 0 = Contents, 1 = Text size, 2 = Margins
constexpr const char* kMarginNames[] = {"Narrow", "Normal", "Wide", "Wider"};

static void drawMenuBox(const char* title, int rows, int h, int& x, int& y, int& w) {
  w = 230;
  x = (gfx::W - w) / 2;
  y = 250;
  gfx::waitIdle();
  gfx::fadeBackground();
  frame(x, y, w, h, 12);
  gfx::bigText(x + 18, y + 8, title, 18);
  gfx::fillRect(x + 10, y + 34, w - 20, 2, true);
}

static void drawReaderMenu() {
  constexpr int rowH = 34;
  int x, y, w;
  if (readerMenu == RMenu::Main) {
    drawMenuBox("Menu", mainCount, 50 + mainCount * rowH, x, y, w);
    for (int i = 0; i < mainCount; i++) {
      const int ry = y + 40 + i * rowH;
      const bool sel = i == readerMenuSel;
      if (sel) roundRect(x + 8, ry, w - 16, rowH - 4, 6, true);
      gfx::bigText(x + 22, ry + 4, mainItems[i] == 0 ? "Contents" : mainItems[i] == 1 ? "Text size" : "Margins", 18, !sel);
    }
  } else if (readerMenu == RMenu::Size) {
    constexpr int n = sizeof(reader::kFontSizes) / sizeof(reader::kFontSizes[0]);
    drawMenuBox("Text size", n, 50 + n * rowH, x, y, w);
    for (int i = 0; i < n; i++) {
      const int ry = y + 40 + i * rowH;
      const bool sel = i == readerMenuSel;
      if (sel) roundRect(x + 8, ry, w - 16, rowH - 4, 6, true);
      char row[24];
      snprintf(row, sizeof(row), "%d px%s", reader::kFontSizes[i], reader::kFontSizes[i] == reader::fontSize() ? "  *" : "");
      gfx::bigText(x + 22, ry + 4, row, 18, !sel);
    }
  } else if (readerMenu == RMenu::Margin) {
    constexpr int n = sizeof(reader::kMargins) / sizeof(reader::kMargins[0]);
    drawMenuBox("Margins", n, 50 + n * rowH, x, y, w);
    for (int i = 0; i < n; i++) {
      const int ry = y + 40 + i * rowH;
      const bool sel = i == readerMenuSel;
      if (sel) roundRect(x + 8, ry, w - 16, rowH - 4, 6, true);
      char row[24];
      snprintf(row, sizeof(row), "%s%s", kMarginNames[i], reader::kMargins[i] == reader::margin() ? "  *" : "");
      gfx::bigText(x + 22, ry + 4, row, 18, !sel);
    }
  } else if (readerMenu == RMenu::Toc) {
    constexpr int visible = 14, rh = 48;
    const int n = reader::tocCount();
    if (readerMenuSel < tocTop) tocTop = readerMenuSel;
    if (readerMenuSel >= tocTop + visible) tocTop = readerMenuSel - visible + 1;
    gfx::waitIdle();
    gfx::clear();
    gfx::fillRect(0, 46, gfx::W, 2, true);
    gfx::text(kMargin, 7, "Contents");
    const int cur = reader::tocCurrent();
    for (int r = 0; r < visible && tocTop + r < n; r++) {
      const int i = tocTop + r, ry = 56 + r * rh;
      const bool sel = i == readerMenuSel;
      if (sel) roundRect(kMargin - 6, ry, gfx::W - 2 * kMargin + 12, rh - 4, 6, true);
      const int indent = min(2, (int)reader::tocDepth(i)) * 18;
      const int tw = gfx::W - 2 * kMargin - indent - 24;
      gfx::text(kMargin + 6 + indent, ry + 8, gfx::ellipsize(reader::tocTitle(i), tw), !sel);
      if (i == cur) gfx::text(gfx::W - kMargin - 12, ry + 8, "*", !sel);
    }
  }
  gfx::flush(EInkDisplay::FAST_REFRESH);
}

static void closeReaderMenu(bool redraw) {
  readerMenu = RMenu::None;
  reader::uiFont(false);
  if (redraw) reader::draw(false);
}

static void openReaderMenu() {
  mainCount = 0;
  if (reader::tocCount() > 0) mainItems[mainCount++] = 0;
  mainItems[mainCount++] = 1;
  mainItems[mainCount++] = 2;
  readerMenu = RMenu::Main;
  readerMenuSel = 0;
  reader::uiFont(true);
  drawReaderMenu();
}

static void readerMenuLoop() {
  const bool down = pressed(InputManager::BTN_DOWN) || pressed(InputManager::BTN_RIGHT);
  const bool up = !down && (pressed(InputManager::BTN_UP) || pressed(InputManager::BTN_LEFT));
  int count = readerMenu == RMenu::Main ? mainCount : readerMenu == RMenu::Size ? (int)(sizeof(reader::kFontSizes) / sizeof(reader::kFontSizes[0])) : readerMenu == RMenu::Margin ? (int)(sizeof(reader::kMargins) / sizeof(reader::kMargins[0])) : reader::tocCount();
  if (down || up) {
    readerMenuSel = (readerMenuSel + (down ? 1 : count - 1)) % max(1, count);
    drawReaderMenu();
  } else if (pressed(InputManager::BTN_CONFIRM)) {
    if (readerMenu == RMenu::Main) {
      if (mainItems[readerMenuSel] == 0) {
        readerMenu = RMenu::Toc;
        readerMenuSel = max(0, reader::tocCurrent());
        tocTop = 0;
      } else if (mainItems[readerMenuSel] == 2) {
        readerMenu = RMenu::Margin;
        readerMenuSel = 1;
        for (int i = 0; i < (int)(sizeof(reader::kMargins) / sizeof(reader::kMargins[0])); i++)
          if (reader::kMargins[i] == reader::margin()) readerMenuSel = i;
      } else {
        readerMenu = RMenu::Size;
        readerMenuSel = 2;
        for (int i = 0; i < (int)(sizeof(reader::kFontSizes) / sizeof(reader::kFontSizes[0])); i++)
          if (reader::kFontSizes[i] == reader::fontSize()) readerMenuSel = i;
      }
      drawReaderMenu();
    } else if (readerMenu == RMenu::Margin) {
      const int px = reader::kMargins[readerMenuSel];
      readerMenu = RMenu::None;
      reader::uiFont(false);
      if (!reader::setMargin(px)) reader::draw(false);
    } else if (readerMenu == RMenu::Size) {
      const int px = reader::kFontSizes[readerMenuSel];
      readerMenu = RMenu::None;
      reader::uiFont(false);
      if (!reader::setFontSize(px)) reader::draw(false);
    } else {
      const int entry = readerMenuSel;
      readerMenu = RMenu::None;
      reader::uiFont(false);
      if (!reader::openToc(entry)) reader::draw(false);
    }
  } else if (pressed(InputManager::BTN_BACK)) {
    if (readerMenu == RMenu::Main) {
      closeReaderMenu(true);
    } else {
      readerMenu = RMenu::Main;
      readerMenuSel = 0;
      drawReaderMenu();
    }
  }
}

static void readerLoop() {
  if (readerMenu != RMenu::None) {
    readerMenuLoop();
    return;
  }
  if (pressed(InputManager::BTN_CONFIRM)) {
    openReaderMenu();
  } else if (pressed(InputManager::BTN_RIGHT) || pressed(InputManager::BTN_DOWN)) {
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
    handleSerial();
    if (pressed(InputManager::BTN_POWER)) ESP.restart();
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
  xTaskCreate(inputTask, "input", 3072, nullptr, 2, nullptr);
#if FREEINK_DEVICE_X4CLASSIC
  freeink::applyXteinkDisplayController();
#endif
  // The SD card must be mounted before the display driver claims the shared SPI bus.
  const bool sdOk = SdMan.begin();
  if (sdOk && SdMan.exists(kKeyboardFile)) kbStyle = atoi(SdMan.readFile(kKeyboardFile).c_str()) == 1 ? 1 : 0;
  if (sdOk && SdMan.exists(kSleepFile)) {
    const int m = atoi(SdMan.readFile(kSleepFile).c_str());
    for (int opt : kSleepOptions) if (opt == m) sleepMinutes = m;
  }
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
  if (SdMan.exists("/BookOrbit/.covers") && !SdMan.exists("/BookOrbit/.covers/.single")) {  // older versions cached a cover per library book
    SdMan.removeDir("/BookOrbit/.covers");
    SdMan.ensureDirectoryExists("/BookOrbit/.covers");
    SdMan.writeFile("/BookOrbit/.covers/.single", "1");
    for (const String& name : SdMan.listFiles("/BookOrbit/.library", 100))
      if (name.startsWith("8-")) SdMan.remove((String("/BookOrbit/.library/") + name).c_str());
  }
  loadStatsCache();
  loadPet();
  screen = openLibraryMagic == kLibraryMagic ? Screen::Library : Screen::Home;
  if (screen == Screen::Library && openScope >= 1 && openScope <= 3) {  // back to the author / series / scope the book was opened from
    src = (Scope)openScope;
    srcId = openScopeId;
    strlcpy(srcName, openScopeName, sizeof(srcName));
  }
  openLibraryMagic = 0;
  String error;
  if (!config.load(error)) fatal("Setup needed", error + "\nSee config.example.json");

  BookOrbit::setActivityHook([](bool active) {  // free the refresh shadow while the radio moves data
    static int depth = 0;
    if (active) {
      if (depth++ == 0) gfx::setLowMemory(true);
    } else if (depth > 0 && --depth == 0) {
      gfx::setLowMemory(false);
    }
  });
  api.configure(config.server, config.insecureTls);
  if (config.timezone.length()) {
    setenv("TZ", config.timezone.c_str(), 1);
    tzset();
  }
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
    WiFi.setSleep(false);  // modem sleep delays inbound data by seconds on some access points
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
    refreshStats();
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
    refreshStats();
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


// The cover of the Reading now book is fetched once (one small download, decoded on the spot) while Home is idle and online.
static void fetchReadingCover() {
  static uint32_t retryAfter = 0;
  const long id = stats.nowId;
  if (!stats.readingNow || !id || net != Net::Online || millis() < retryAfter || cover::cached(id)) return;
  String error;
  if (api.fetchThumbnail(id, cover::stagingPath(), error)) {
    gfx::setLowMemory(true);  // the decoder wants the RAM the refresh shadow would use
    const bool ok = cover::convertStaged(id, error);  // an unusable image is marked, not retried
    gfx::setLowMemory(false);
    Serial.printf("cover %ld: %s\n", id, ok ? "cached" : error.c_str());
    if (ok) showList(false);
  } else {
    Serial.printf("cover %ld failed: %s\n", id, error.c_str());
    if (error.indexOf("404") >= 0) cover::markNone(id);
    else retryAfter = millis() + 60000;
  }
}

// Holding Back switches Wi-Fi off (library stays usable from the cache) or back on.
static void toggleWifi() {
  if (net == Net::Offline) {
    net = Net::Connecting;
    netStart = millis();
    SdMan.remove(kWifiOffFlag);
    WiFi.mode(WIFI_STA);
    WiFi.setSleep(false);  // modem sleep delays inbound data by seconds on some access points
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

// True once per short click of Back (on release, unless the press turned out to be a hold).
static bool holdFired = false;
static bool backClicked() {
  if (injected == InputManager::BTN_BACK) { injected = -1; return true; }  // test hook
  if (!backReleased) return false;
  backReleased = false;
  const bool wasHold = holdFired;
  holdFired = false;
  return !wasHold;
}

// True once per press, when Back has been held for 800 ms.
static bool backHeld() {
  static bool handled = false;
  if (!input.isPressed(InputManager::BTN_BACK)) { handled = false; return false; }
  if (handled || input.getHeldTime() < 800) return false;
  handled = true;
  holdFired = true;
  return true;
}

void loop() {
  handleSerial();
  if (anyCount || injected >= 0) {
    anyCount = 0;
    lastActivity = millis();
  }
  if (millis() - lastActivity > sleepAfterMs()) goToSleep();
  // A click on Power sleeps, but only once the button has been seen released since boot, so the press
  // that woke the device doesn't put it straight back to sleep.
  static bool powerArmed = false;
  if (!input.isPowerButtonPhysicallyPressed()) powerArmed = true;
  const bool powerClicked = pressed(InputManager::BTN_POWER);  // consumed even while unarmed, so the waking press is dropped
  if (powerArmed && powerClicked) goToSleep();

  if (reader::isOpen()) {
    readerLoop();
    delay(10);
    return;
  }
  pumpDisplay();
  backgroundSync();
  if (screen == Screen::Home) {
    if (backHeld()) {
      toggleWifi();
    } else if (pressed(InputManager::BTN_RIGHT)) {
      screen = Screen::Settings;
      settingsSel = 0;
      showList(false);
    } else if (pressed(InputManager::BTN_DOWN) && homeSel < (stats.readingNow ? 1 : 0)) {
      homeSel++;
      showList(false);
    } else if (pressed(InputManager::BTN_UP) && homeSel > 0) {
      homeSel--;
      showList(false);
    } else if (pressed(InputManager::BTN_CONFIRM)) {
      if (readingSelected()) {
        openReadingNow();
      } else {
        screen = Screen::Browse;
        showList(false);
      }
    } else {
      fetchReadingCover();  // idle: fetch the Reading now cover once
    }
    delay(10);
    return;
  }
  if (screen == Screen::Confirm) {
    if (backClicked()) {
      screen = Screen::Settings;
      showList(false);
    } else if (pressed(InputManager::BTN_DOWN) && confirmSel == 0) {
      confirmSel = 1;
      showList(false);
    } else if (pressed(InputManager::BTN_UP) && confirmSel == 1) {
      confirmSel = 0;
      showList(false);
    } else if (pressed(InputManager::BTN_CONFIRM)) {
      if (confirmSel == 1) {
        switchServer();
      } else {
        screen = Screen::Settings;
        showList(false);
      }
    }
    delay(10);
    return;
  }
  if (screen == Screen::Keyboard) {
    const bool t9 = kbStyle == 1;
    if (backClicked()) {  // Back deletes the last character
      if (kbText.length()) kbText.remove(kbText.length() - 1);
      t9LastKey = -1;
      showList(false);
    } else if (pressed(InputManager::BTN_UP)) {
      if (t9) t9Move(-1, 0); else keyboardMove(-1, 0);
      showList(false);
    } else if (pressed(InputManager::BTN_DOWN)) {
      if (t9) t9Move(1, 0); else keyboardMove(1, 0);
      showList(false);
    } else if (pressed(InputManager::BTN_LEFT)) {
      if (t9) t9Move(0, -1); else keyboardMove(0, -1);
      showList(false);
    } else if (pressed(InputManager::BTN_RIGHT)) {
      if (t9) t9Move(0, 1); else keyboardMove(0, 1);
      showList(false);
    } else if (pressed(InputManager::BTN_CONFIRM)) {
      if (t9) t9Press(); else keyboardPress();
      showList(false);
    }
    delay(10);
    return;
  }
  if (screen == Screen::Networks) {
    if (backClicked()) {  // leave: rejoin the saved network that the scan may have interrupted
      screen = Screen::Settings;
      if (net != Net::Online && !SdMan.exists(kWifiOffFlag)) startWifi();
      showList(false);
    } else if (pressed(InputManager::BTN_DOWN) && netSel < scanCount) {
      netSel++;
      if (netSel >= netTop + kPickerRows) netTop = netSel - kPickerRows + 1;
      showList(false);
    } else if (pressed(InputManager::BTN_UP) && netSel > 0) {
      netSel--;
      if (netSel < netTop) netTop = netSel;
      showList(false);
    } else if (pressed(InputManager::BTN_CONFIRM)) {
      networkChosen();
    }
    delay(10);
    return;
  }
  if (screen == Screen::Picker) {
    if (backHeld()) {
      toggleWifi();
    } else if (backClicked()) {  // leave without changing anything
      screen = Screen::Settings;
      showList(false);
    } else if (pressed(InputManager::BTN_DOWN) && pickerSel < pickerCount() - 1) {
      pickerSel++;
      if (pickerSel >= pickerTop + kPickerRows) pickerTop = pickerSel - kPickerRows + 1;
      showList(false);
    } else if (pressed(InputManager::BTN_UP) && pickerSel > 0) {
      pickerSel--;
      if (pickerSel < pickerTop) pickerTop = pickerSel;
      showList(false);
    } else if (pressed(InputManager::BTN_CONFIRM)) {
      if (pickerKind == 0) {
        setPet(pickerSel);
      } else if (pickerKind == 2) {
        kbStyle = pickerSel;
        SdMan.ensureDirectoryExists("/BookOrbit");
        SdMan.writeFile(kKeyboardFile, String(kbStyle));
      } else {
        setSleep(kSleepOptions[pickerSel]);
      }
      screen = Screen::Settings;
      showList(false);
    }
    delay(10);
    return;
  }
  if (screen == Screen::Settings) {
    if (backHeld()) {
      toggleWifi();
    } else if (backClicked()) {
      screen = Screen::Home;
      showList(false);
    } else if (pressed(InputManager::BTN_DOWN) && settingsSel < 7) {
      settingsSel++;
      showList(false);
    } else if (pressed(InputManager::BTN_UP) && settingsSel > 0) {
      settingsSel--;
      showList(false);
    } else if (pressed(InputManager::BTN_CONFIRM)) {
      if (settingsSel == 0) {
        toggleWifi();  // redraws by itself
      } else if (settingsSel == 1) {
        openNetworks();
      } else if (settingsSel == 2) {
        openKeyboard(KbServer, config.server.isEmpty() ? String("http://") : config.server);
      } else if (settingsSel == 3) {
        openKeyboard(KbUsername, config.username);
      } else if (settingsSel == 4) {
        openKeyboard(KbPassword, config.password);
      } else {
        openPicker(settingsSel == 5 ? 2 : settingsSel == 6 ? 0 : 1);
        showList(false);
      }
    }
    delay(10);
    return;
  }
  if (screen == Screen::Browse) {
    if (backHeld()) {
      toggleWifi();
    } else if (backClicked()) {
      screen = Screen::Home;
      showList(false);
    } else if (pressed(InputManager::BTN_DOWN) && browseSel < 3) {
      browseSel++;
      showList(false);
    } else if (pressed(InputManager::BTN_UP) && browseSel > 0) {
      browseSel--;
      showList(false);
    } else if (pressed(InputManager::BTN_CONFIRM)) {
      String error;
      if (net == Net::Online) setBusy(true);
      bool ok;
      if (browseSel == 0) {
        src = Scope::All;
        srcId = 0;
        srcName[0] = 0;
        ok = loadPage(0, error);
        if (ok) screen = Screen::Library;
      } else {
        ok = loadEntities((Scope)browseSel, 0, error);
        if (ok) screen = Screen::Entities;
      }
      busy = false;
      if (ok) showList(false); else message("Error", error + "\n\nPress any button");
    }
    delay(10);
    return;
  }
  if (screen == Screen::Entities) {
    String error;
    if (backHeld()) {
      toggleWifi();
    } else if (backClicked()) {
      screen = Screen::Browse;
      showList(false);
    } else if (pressed(InputManager::BTN_DOWN) && entSel + 1 < entCount) {
      entSel++;
      showList(false);
    } else if (pressed(InputManager::BTN_UP) && entSel > 0) {
      entSel--;
      showList(false);
    } else if (pressed(InputManager::BTN_CONFIRM) && entCount > 0) {
      src = entKind;
      srcId = entities[entSel].id;
      strlcpy(srcName, entities[entSel].name, sizeof(srcName));
      if (net == Net::Online) setBusy(true);
      const bool ok = loadPage(0, error);
      busy = false;
      if (ok) {
        screen = Screen::Library;
        showList(false);
      } else {
        src = Scope::All;
        message("Error", error + "\n\nPress any button");
      }
    } else if (pressed(InputManager::BTN_RIGHT) && (entPage + 1) * kPageSize < entTotal) {
      if (net == Net::Online) setBusy(true);
      const bool ok = loadEntities(entKind, entPage + 1, error);
      busy = false;
      if (ok) showList(false); else message("Error", error + "\n\nPress any button");
    } else if (pressed(InputManager::BTN_LEFT) && entPage > 0) {
      if (net == Net::Online) setBusy(true);
      const bool ok = loadEntities(entKind, entPage - 1, error);
      busy = false;
      if (ok) showList(false); else message("Error", error + "\n\nPress any button");
    }
    delay(10);
    return;
  }
  String error;
  if (backHeld()) {
    toggleWifi();
  } else if (pressed(InputManager::BTN_DOWN) && selected + 1 < bookCount) {
    selected++;
    showList(false);
  } else if (pressed(InputManager::BTN_UP) && selected > 0) {
    selected--;
    showList(false);
  } else if (backClicked()) {
    if (src == Scope::All) {
      screen = Screen::Browse;
    } else {
      entKind = src;  // after returning from a book the list is gone: reload its first page
      if (entLoaded != src && !loadEntities(src, 0, error)) screen = Screen::Browse; else screen = Screen::Entities;
    }
    showList(false);
  } else if (pressed(InputManager::BTN_CONFIRM)) {
    openSelected();
  } else if (pressed(InputManager::BTN_RIGHT) && (page + 1) * kPageSize < bookTotal) {
    if (net == Net::Online) setBusy(true);
    const bool ok = loadPage(page + 1, error);
    busy = false;
    if (ok) showList(false); else message("Error", error + "\n\nPress any button");
  } else if (pressed(InputManager::BTN_LEFT) && page > 0) {
    if (net == Net::Online) setBusy(true);
    const bool ok = loadPage(page - 1, error);
    busy = false;
    if (ok) showList(false); else message("Error", error + "\n\nPress any button");
  }
  delay(10);
}
