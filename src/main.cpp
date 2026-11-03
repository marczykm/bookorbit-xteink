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
Config config;
BookOrbit api;

// Opening a book restarts the device into a Wi-Fi-free reading mode (the reader needs a clean heap); this survives the reset.
constexpr uint32_t kReadMagic = 0x424F4B31;
RTC_NOINIT_ATTR uint32_t readMagic;
RTC_NOINIT_ATTR char readPath[160];

RTC_NOINIT_ATTR float readServerPercent;  // progress the server holds for the book, -1 when unknown
constexpr uint32_t kLibraryMagic = 0x4C494252;
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

constexpr int kPageSize = 8;
constexpr int kMargin = 16;
constexpr int kRowHeight = 80;
constexpr int kListTop = 64;

Book books[kPageSize];
int bookCount = 0, bookTotal = 0, page = 0, selected = 0;
int fastRefreshes = 0;
// Which screen is showing. Home is the entry point; Library is its only menu item for now.
enum class Screen { Home, Library, Settings, Picker } screen = Screen::Home;
bool halfNext = false;  // the next full draw (a screen change) uses the gentler half refresh
int pickerKind = 0, pickerSel = 0, pickerTop = 0;  // Picker: 0 = companion list, 1 = sleep timer list
int settingsSel = 0;  // Settings list: 0 = Wi-Fi, 1 = companion, 2 = sleep timer
int homeSel = 0;  // Home menu: 0 = Library, 1 = the Reading now card
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
      if (line == "CLEARCOVERS") { SdMan.removeDir("/BookOrbit/.covers"); ESP.restart(); }  // test hook
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

// Text in a normal row (black) or a highlighted one (white on the black highlight).
static void label(int x, int y, const char* s, bool highlighted) { gfx::text(x, y, s, !highlighted); }

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
    // Cover: a white plate with a frame (so it also shows on the black selection band), then the cached 1-bit image.
    gfx::fillRect(kMargin, y + 1, cover::W + 2, cover::H + 2, false);
    gfx::fillRect(kMargin, y + 1, cover::W + 2, cover::H + 2, true);
    gfx::fillRect(kMargin + 1, y + 2, cover::W, cover::H, false);
    uint8_t bits[cover::kBytes];
    if (books[i].id && cover::load(books[i].id, bits)) gfx::blit1bit(kMargin + 1, y + 2, bits, cover::W, cover::H);
    const int textX = kMargin + cover::W + 2 + 14, textW = gfx::W - textX - 34;
    label(textX, y + 8, gfx::ellipsize(books[i].title, textW), sel);
    label(textX, y + 8 + gfx::lineHeight() - 2, gfx::ellipsize(books[i].author, textW), sel);
    if (SdMan.exists(BookOrbit::pathFor(books[i]).c_str())) gfx::fillRect(gfx::W - 24, y + 28, 12, 12, !sel);  // downloaded
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
  if (api.fetchContinueReading(now, nowProgress, found, error) && found) {  // add the book being read to the cached document
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

// The stats panel under the Home menu: y from 146 down to 700.
static void drawStats() {
  if (!stats.any()) {
    gfx::text(kMargin, 172, "Statistics appear after the first sync.");
    return;
  }
  constexpr int kHalf = 220, kRight = kMargin + kHalf + 8, kFull = gfx::W - 2 * kMargin;
  char buf[64];
  int y = 146;
  if (stats.readingNow) {  // the book being read: cover, title, author, progress
    const bool sel = homeSel == 1;  // selected: dark like the selected Library row, with light text
    frame(kMargin, y, kFull, 96, 8, sel);
    label(kMargin + 68, y + 1, "Reading now", sel);
    roundRect(kMargin + 8, y + 11, cover::W + 4, cover::H + 4, 3, false);  // white plate so the cover shows on dark too
    roundRect(kMargin + 8, y + 11, cover::W + 4, cover::H + 4, 3, true);
    gfx::fillRect(kMargin + 10, y + 13, cover::W, cover::H, false);
    uint8_t bits[cover::kBytes];
    if (stats.nowId && cover::load(stats.nowId, bits)) gfx::blit1bit(kMargin + 10, y + 13, bits, cover::W, cover::H);
    label(kMargin + 68, y + 24, gfx::ellipsize(stats.nowTitle, kFull - 80), sel);
    label(kMargin + 68, y + 46, gfx::ellipsize(stats.nowAuthor, kFull - 80), sel);
    bar(kMargin + 68, y + 78, kFull - 68 - 70, 12, stats.nowProgress / 100.0f, sel);
    snprintf(buf, sizeof(buf), "%d%%", (int)stats.nowProgress);
    label(kMargin + kFull - 62, y + 62, buf, sel);
    y += 102;
  }
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
  if (!stats.readingNow && homeSel == 1) homeSel = 0;
  const int y = kListTop;
  const bool sel = homeSel == 0;
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
  drawStats();
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
  const char* names[3] = {"Wi-Fi", "Companion", "Sleep after"};
  String sleepText = String(sleepMinutes) + " min";
  const String values[3] = {wifiValue(), pets::kNames[petIndex], sleepText};
  for (int i = 0; i < 3; i++) {
    const int y = kListTop + i * 72;
    const bool sel = i == settingsSel;
    if (sel) roundRect(kMargin, y, gfx::W - 2 * kMargin, 64, 8, true); else frame(kMargin, y, gfx::W - 2 * kMargin, 64);
    gfx::text(kMargin + 14, y + 17, names[i], !sel);
    gfx::text(gfx::W - kMargin - 14 - gfx::textWidth(values[i].c_str()), y + 17, values[i].c_str(), !sel);
  }
  // About: read-only facts.
  char line[96];
  int y = kListTop + 3 * 72 + 14;
  gfx::text(kMargin, y, "Server");
  gfx::text(kMargin, y + 28, gfx::ellipsize(config.server.c_str(), gfx::W - 2 * kMargin));
  snprintf(line, sizeof(line), "Battery: %d%%", (int)battery.readPercentage());
  gfx::text(kMargin, y + 70, line);
  drawHints(true, true, false);
  gfx::flush(!full ? EInkDisplay::FAST_REFRESH : (firstDraw || halfNext) ? EInkDisplay::HALF_REFRESH : EInkDisplay::FULL_REFRESH);
  firstDraw = false;
  halfNext = false;
}

// A list to choose one value from (companion or sleep timer), opened from Settings.
constexpr int kPickerRows = 9, kPickerRowH = 70;

static int pickerCount() { return pickerKind == 0 ? pets::kCount : (int)(sizeof(kSleepOptions) / sizeof(kSleepOptions[0])); }
static String pickerLabel(int i) { return pickerKind == 0 ? String(pets::kNames[i]) : String(kSleepOptions[i]) + (kSleepOptions[i] == 1 ? " minute" : " minutes"); }
static int pickerCurrent() {
  if (pickerKind == 0) return petIndex;
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
  header(pickerKind == 0 ? "Companion" : "Sleep after");
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

static void drawScreen(bool full) {  // draws whichever screen is current
  // Quick refreshes only: no periodic full-screen cleanup (a full refresh flashes the whole panel).
  if (screen == Screen::Home) drawHome(full);
  else if (screen == Screen::Settings) drawSettings(full);
  else if (screen == Screen::Picker) drawPicker(full);
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
static String cachePath(int p) { return "/BookOrbit/.library/8-" + String(p) + ".json"; }

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
  anyCount = 0;
  do {
    delay(20);
  } while (anyCount == 0 && injected < 0);
  injected = -1;
}

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
  readFileId = b.fileId;
  readServerPercent = -1;
  float serverPct;
  if (net == Net::Online && b.fileId && ensureWifi(error) && api.getProgress(b.fileId, serverPct, error)) readServerPercent = serverPct;
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
  gfx::waitIdle();
  display.deepSleep();
  freeink::PowerManager::deepSleepUntilPowerButton();
}

static void closeBook() {
  sessionTick();
  checkpoint();
  reader::close();
  readFileId = 0;
  ESP.restart();  // back to where the book was opened from (see openBook)
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
  loadStatsCache();
  loadPet();
  screen = openLibraryMagic == kLibraryMagic ? Screen::Library : Screen::Home;
  openLibraryMagic = 0;
  String error;
  if (!config.load(error)) fatal("Setup needed", error + "\nSee config.example.json");

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

// Downloads one missing cover of the visible page per call (so buttons stay responsive), then redraws once when none are left.
static void fetchNextCover() {
  static bool dirty = false;
  static uint32_t retryAfter = 0;
  if (net != Net::Online || millis() < retryAfter) return;
  // Candidates: the book being read now (shown on Home) and, on the library screen, the visible page.
  const int n = bookCount + 1;
  for (int k = 0; k < n; k++) {
    const long id = k == 0 ? (stats.readingNow ? stats.nowId : 0) : (screen == Screen::Library ? books[k - 1].id : 0);
    if (!id || cover::cached(id)) continue;
    String error;
    if (api.fetchThumbnail(id, cover::stagingPath(), error) && cover::convertStaged(id, error)) {
      Serial.printf("cover %ld cached\n", id);
      dirty = true;
    } else {
      Serial.printf("cover %ld failed: %s\n", id, error.c_str());
      if (error.indexOf("404") >= 0) cover::markNone(id);  // the book has no cover
      else retryAfter = millis() + 30000;                   // network trouble: try again later
    }
    return;
  }
  if (dirty) {
    dirty = false;
    showList(false);
  }
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
    } else if (pressed(InputManager::BTN_DOWN) && homeSel == 0 && stats.readingNow) {
      homeSel = 1;
      showList(false);
    } else if (pressed(InputManager::BTN_UP) && homeSel == 1) {
      homeSel = 0;
      showList(false);
    } else if (pressed(InputManager::BTN_CONFIRM)) {
      if (homeSel == 1 && stats.readingNow) {
        openReadingNow();
      } else {
        screen = Screen::Library;
        showList(false);
      }
    } else {
      fetchNextCover();  // the Reading now cover
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
      if (pickerKind == 0) setPet(pickerSel); else setSleep(kSleepOptions[pickerSel]);
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
    } else if (pressed(InputManager::BTN_DOWN) && settingsSel < 2) {
      settingsSel++;
      showList(false);
    } else if (pressed(InputManager::BTN_UP) && settingsSel > 0) {
      settingsSel--;
      showList(false);
    } else if (pressed(InputManager::BTN_CONFIRM)) {
      if (settingsSel == 0) {
        toggleWifi();  // redraws by itself
      } else {
        openPicker(settingsSel == 1 ? 0 : 1);
        showList(false);
      }
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
    screen = Screen::Home;
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
  else fetchNextCover();  // idle: fetch covers one at a time (library screen only)
  delay(10);
}
