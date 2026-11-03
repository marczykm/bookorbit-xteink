#include "bookorbit.h"

#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <SDCardManager.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <time.h>

extern const uint8_t rootca_crt_bundle_start[] asm("_binary_x509_crt_bundle_start");
extern const uint8_t rootca_crt_bundle_end[] asm("_binary_x509_crt_bundle_end");

namespace {
constexpr int kTimeoutMs = 15000;

// One of the two is used, depending on the server URL's scheme.
struct Conn {
  WiFiClient plain;
  WiFiClientSecure tls;
};
bool insecureTls = false;

// Certificates are only valid within a date range, so HTTPS needs the real time.
bool syncClock() {
  if (time(nullptr) > 1700000000) return true;
  configTime(0, 0, "pool.ntp.org", "time.google.com");
  for (int i = 0; i < 100 && time(nullptr) < 1700000000; i++) delay(100);
  return time(nullptr) > 1700000000;
}

bool begin(HTTPClient& http, Conn& conn, const String& url, const String& token, String& error) {
  const bool https = url.startsWith("https://");
  if (!https && !url.startsWith("http://")) {
    error = "Server must start with http:// or https://";
    return false;
  }
  if (https) {
    if (insecureTls) {
      conn.tls.setInsecure();
    } else {
      if (!syncClock()) {
        error = "Could not get the time (needed for HTTPS)";
        return false;
      }
      conn.tls.setCACertBundle(rootca_crt_bundle_start, rootca_crt_bundle_end - rootca_crt_bundle_start);
    }
  }
  http.setConnectTimeout(kTimeoutMs);
  http.setTimeout(kTimeoutMs);
  static const char* kHeaders[] = {"Transfer-Encoding"};
  http.collectHeaders(kHeaders, 1);  // the response may be chunked: BodyStream needs to know
  if (!(https ? http.begin(conn.tls, url) : http.begin(conn.plain, url))) {
    error = "Bad server address";
    return false;
  }
  if (token.length()) http.addHeader("Authorization", "Bearer " + token);
  return true;
}

void (*activityHook)(bool) = nullptr;
// Marks the duration of a request for the app's low-memory mode.
struct NetGuard {
  NetGuard() { if (activityHook) activityHook(true); }
  ~NetGuard() { if (activityHook) activityHook(false); }
};

// Success is any 2xx: the real server answers POSTs such as sign-in with 201 Created, the mock with 200.
bool isOk(int code) { return code >= 200 && code < 300; }

// A response body as a Stream, whatever its framing: Content-Length, chunked (what the real server sends) or until the
// connection closes. pull() never waits, so a slow transfer cannot freeze the app; the blocking readers sit on top of it.
class BodyStream : public Stream {
 public:
  explicit BodyStream(HTTPClient& http) : c_(http.getStreamPtr()) {
    if (http.header("Transfer-Encoding").indexOf("chunked") >= 0) {
      mode_ = Mode::Chunked;
    } else if (http.getSize() >= 0) {
      mode_ = Mode::Length;
      remaining_ = http.getSize();
    } else {
      mode_ = Mode::Close;
    }
  }

  // Copies what has arrived (at most len bytes) without waiting; 0 when nothing is there yet. Check done().
  size_t pull(uint8_t* buf, size_t len) {
    if (done_ || len == 0) return 0;
    if (hasPeek_) {
      buf[0] = peeked_;
      hasPeek_ = false;
      return 1;
    }
    size_t n = 0;
    switch (mode_) {
      case Mode::Length: {
        n = min<size_t>(min<size_t>(len, remaining_), c_->available());
        if (n) n = c_->read(buf, n);
        remaining_ -= n;
        if (remaining_ == 0) { done_ = true; complete_ = true; }
        break;
      }
      case Mode::Close: {
        n = min<size_t>(len, c_->available());
        if (n) n = c_->read(buf, n);
        if (n == 0 && !c_->connected()) { done_ = true; complete_ = true; }
        break;
      }
      case Mode::Chunked: n = pullChunked(buf, len); break;
    }
    if (n == 0 && !done_ && !c_->connected() && c_->available() == 0) done_ = true;  // closed mid-body: incomplete
    bytes_ += n;
    return n;
  }
  bool done() const { return done_; }
  bool complete() const { return complete_; }  // the whole body arrived (not cut off)
  size_t bytes() const { return bytes_; }

  // Blocking read: waits for data (up to kStallMs without any), returns 0 at the end or after a stall.
  size_t readSome(uint8_t* buf, size_t len) {
    const uint32_t start = millis();
    for (;;) {
      const size_t n = pull(buf, len);
      if (n || done_) return n;
      if (millis() - start > kStallMs) return 0;
      delay(2);
    }
  }

  // Stream interface for ArduinoJson.
  int available() override { return (hasPeek_ ? 1 : 0) + (done_ ? 0 : c_->available()); }
  int read() override {
    uint8_t b;
    return readSome(&b, 1) == 1 ? b : -1;
  }
  int peek() override {
    if (!hasPeek_) {
      const int v = read();
      if (v < 0) return -1;
      peeked_ = (uint8_t)v;
      hasPeek_ = true;
      if (bytes_) bytes_--;  // counted again when re-read
    }
    return peeked_;
  }
  size_t write(uint8_t) override { return 0; }

 private:
  static constexpr uint32_t kStallMs = 45000;  // the real server can pause for many seconds mid-response

  size_t pullChunked(uint8_t* buf, size_t len) {
    for (;;) {
      switch (state_) {
        case State::Size: {  // "<hex>[;ext]\r\n"
          while (c_->available()) {
            const int ch = c_->read();
            if (ch == '\n') {
              line_[lineLen_] = 0;
              lineLen_ = 0;
              chunkLeft_ = strtoul(line_, nullptr, 16);
              if (chunkLeft_ == 0) { done_ = true; complete_ = true; return 0; }  // the final chunk; trailers are ignored
              state_ = State::Data;
              break;
            }
            if (ch != '\r' && lineLen_ < sizeof(line_) - 1) line_[lineLen_++] = (char)ch;
          }
          if (state_ == State::Size) return 0;
          break;
        }
        case State::Data: {
          const size_t n = min<size_t>(min<size_t>(len, chunkLeft_), c_->available());
          if (n == 0) return 0;
          const size_t got = c_->read(buf, n);
          chunkLeft_ -= got;
          if (chunkLeft_ == 0) { state_ = State::DataEnd; crlfLeft_ = 2; }
          return got;
        }
        case State::DataEnd: {
          while (crlfLeft_ && c_->available()) { c_->read(); crlfLeft_--; }
          if (crlfLeft_) return 0;
          state_ = State::Size;
          break;
        }
      }
    }
  }

  enum class Mode { Length, Chunked, Close } mode_ = Mode::Close;
  enum class State { Size, Data, DataEnd } state_ = State::Size;
  WiFiClient* c_;
  bool hasPeek_ = false, done_ = false, complete_ = false;
  long remaining_ = 0;
  size_t chunkLeft_ = 0, bytes_ = 0;
  uint8_t peeked_ = 0, crlfLeft_ = 0;
  char line_[24];
  size_t lineLen_ = 0;
};

String errorFor(int code) {
  if (code == 401) return "Unauthorized (HTTP 401)";
  if (code < 0) return "Cannot reach server (or HTTPS certificate rejected)";
  return "Server returned HTTP " + String(code);
}

// Keep letters, digits and a few safe characters; everything else (including UTF-8) becomes '_'.
String fileSafe(const char* s) {
  String out;
  for (; *s && out.length() < 80; s++) {
    const char c = *s;
    out += (isalnum((uint8_t)c) || c == ' ' || c == '-' || c == '.' || c == '(' || c == ')') ? c : '_';
  }
  out.trim();
  return out.isEmpty() ? String("book") : out;
}
}  // namespace

void BookOrbit::setActivityHook(void (*hook)(bool)) { activityHook = hook; }

void BookOrbit::configure(const String& server, bool insecure) {
  server_ = server;
  insecureTls = insecure;
}

bool BookOrbit::login(const String& user, const String& password, String& error) {
  NetGuard netGuard;
  Conn client;
  HTTPClient http;
  if (!begin(http, client, server_ + "/api/v1/auth/login", "", error)) return false;
  http.addHeader("Content-Type", "application/json");
  JsonDocument body;
  body["username"] = user;
  body["password"] = password;
  body["clientKind"] = "native";
  body["deviceLabel"] = "Xteink X4";
  String payload;
  serializeJson(body, payload);
  const int code = http.POST(payload);
  if (!isOk(code)) {
    error = code == 401 ? "Wrong username or password" : errorFor(code);
    http.end();
    return false;
  }
  JsonDocument doc;
  BodyStream resp(http);
  const auto parseErr = deserializeJson(doc, resp);
  http.end();
  token_ = doc["accessToken"] | "";
  refreshToken_ = doc["refreshToken"] | "";
  if (parseErr || token_.isEmpty()) {
    Serial.printf("login: HTTP %d, parse %s, access token %s\n", code, parseErr.c_str(), token_.isEmpty() ? "missing" : "present");
    error = "Unexpected login response";
    return false;
  }
  return true;
}

int BookOrbit::listBooksOnce(int page, int pageSize, Book* out, int max, int& total, String& error) {
  NetGuard netGuard;
  Conn client;
  HTTPClient http;
  if (!begin(http, client, server_ + "/api/v1/books/query", token_, error)) return -1;
  http.addHeader("Content-Type", "application/json");
  JsonDocument body;
  body["sort"][0]["field"] = "addedAt";
  body["sort"][0]["dir"] = "desc";
  body["pagination"]["page"] = page;
  body["pagination"]["size"] = pageSize;
  String payload;
  serializeJson(body, payload);
  const int code = http.POST(payload);
  if (!isOk(code)) {
    error = errorFor(code);
    http.end();
    return -1;
  }
  // The real server sends much more per book than we need; keep only these fields.
  JsonDocument filter;
  filter["total"] = true;
  filter["items"][0]["id"] = true;
  filter["items"][0]["title"] = true;
  filter["items"][0]["authors"] = true;
  filter["items"][0]["files"][0]["id"] = true;
  filter["items"][0]["files"][0]["format"] = true;
  JsonDocument doc;
  BodyStream resp(http);
  const uint32_t tList = millis();
  const auto err = deserializeJson(doc, resp, DeserializationOption::Filter(filter));
  http.end();
  if (err) {
    Serial.printf("books/query: %s after %u bytes, %u ms reading\n", err.c_str(), (unsigned)resp.bytes(), (unsigned)(millis() - tList));
    error = String("Bad response: ") + err.c_str();
    return -1;
  }
  total = doc["total"] | 0;
  int n = 0;
  for (JsonObject item : doc["items"].as<JsonArray>()) {
    if (n >= max) break;
    Book& b = out[n++];
    b = Book();
    b.id = item["id"] | 0L;
    strlcpy(b.title, item["title"] | "(untitled)", sizeof(b.title));
    strlcpy(b.author, item["authors"][0] | "", sizeof(b.author));
    for (JsonObject f : item["files"].as<JsonArray>()) {
      const char* fmt = f["format"] | "";
      if (strcasecmp(fmt, "epub") == 0 || strcasecmp(fmt, "kepub") == 0) {
        b.fileId = f["id"] | 0L;
        break;
      }
    }
  }
  return n;
}

bool BookOrbit::fetchContinueReadingOnce(Book& book, float& progress, bool& found, String& error) {
  NetGuard netGuard;
  Conn client;
  HTTPClient http;
  found = false;
  if (!begin(http, client, server_ + "/api/v1/dashboard/scrollers/batch", token_, error)) return false;
  http.addHeader("Content-Type", "application/json");
  JsonDocument body;
  body["items"][0]["id"] = "continue";
  body["items"][0]["type"] = "continue-reading";
  body["items"][0]["limit"] = 1;
  String payload;
  serializeJson(body, payload);
  const int code = http.POST(payload);
  if (!isOk(code)) {
    error = errorFor(code);
    http.end();
    return false;
  }
  JsonDocument filter;
  filter["items"][0]["books"][0]["id"] = true;
  filter["items"][0]["books"][0]["title"] = true;
  filter["items"][0]["books"][0]["authors"] = true;
  filter["items"][0]["books"][0]["readingProgress"] = true;
  filter["items"][0]["books"][0]["files"][0]["id"] = true;
  filter["items"][0]["books"][0]["files"][0]["format"] = true;
  JsonDocument doc;
  BodyStream resp(http);
  const auto err = deserializeJson(doc, resp, DeserializationOption::Filter(filter));
  http.end();
  if (err) {
    error = String("Bad response: ") + err.c_str();
    return false;
  }
  JsonObject b = doc["items"][0]["books"][0];
  if (b.isNull()) return true;  // nothing in progress
  book = Book();
  book.id = b["id"] | 0L;
  strlcpy(book.title, b["title"] | "(untitled)", sizeof(book.title));
  strlcpy(book.author, b["authors"][0] | "", sizeof(book.author));
  for (JsonObject f : b["files"].as<JsonArray>()) {
    const char* fmt = f["format"] | "";
    if (strcasecmp(fmt, "epub") == 0 || strcasecmp(fmt, "kepub") == 0) { book.fileId = f["id"] | 0L; break; }
  }
  progress = b["readingProgress"] | 0.0f;
  found = book.id != 0;
  return true;
}

bool BookOrbit::fetchThumbnailOnce(long bookId, const String& path, String& error) {
  NetGuard netGuard;
  Conn client;
  HTTPClient http;
  if (!begin(http, client, server_ + "/api/v1/books/" + String(bookId) + "/thumbnail", token_, error)) return false;
  const int code = http.GET();
  if (!isOk(code)) {
    error = errorFor(code);
    http.end();
    return false;
  }
  SdMan.ensureDirectoryExists("/BookOrbit");
  SdMan.ensureDirectoryExists("/BookOrbit/.covers");
  SdMan.remove(path.c_str());
  FsFile file = SdMan.open(path.c_str(), O_WRITE | O_CREAT | O_TRUNC);
  if (!file) {
    error = "Cannot write to SD card";
    http.end();
    return false;
  }
  BodyStream in(http);
  uint8_t buf[1024];
  size_t done = 0;
  bool writeFailed = false;
  for (;;) {
    const size_t n = in.readSome(buf, sizeof(buf));
    if (n == 0) break;
    if (file.write(buf, n) != n) { writeFailed = true; break; }
    done += n;
    if (done > 1024 * 1024) break;  // a thumbnail this big is not a thumbnail
  }
  file.close();
  http.end();
  if (writeFailed || done == 0 || !in.complete()) {
    SdMan.remove(path.c_str());
    error = "Thumbnail download incomplete";
    return false;
  }
  return true;
}

bool BookOrbit::fetchWidgetsOnce(const char* const* types, int count, String& json, String& error) {
  NetGuard netGuard;
  Conn client;
  HTTPClient http;
  if (!begin(http, client, server_ + "/api/v1/dashboard/widgets/batch", token_, error)) return false;
  http.addHeader("Content-Type", "application/json");
  JsonDocument body;
  for (int i = 0; i < count; i++) body["widgets"][i] = types[i];
  String payload;
  serializeJson(body, payload);
  const int code = http.POST(payload);
  if (!isOk(code)) {
    error = errorFor(code);
    http.end();
    return false;
  }
  JsonDocument doc;
  BodyStream resp(http);
  const auto err = deserializeJson(doc, resp);
  http.end();
  if (err) {
    error = String("Bad response: ") + err.c_str();
    return false;
  }
  JsonDocument out;  // {"type": data, ...} for the widgets the server had data for
  for (JsonObject item : doc["items"].as<JsonArray>()) {
    const char* type = item["type"] | "";
    if (*type && !item["data"].isNull()) out[type] = item["data"];
  }
  serializeJson(out, json);
  return true;
}


bool BookOrbit::postSessionOnce(long fileId, const String& body, bool& rejected, String& error) {
  NetGuard netGuard;
  Conn client;
  HTTPClient http;
  rejected = false;
  if (!begin(http, client, server_ + "/api/v1/books/files/" + String(fileId) + "/sessions", token_, error)) return false;
  http.addHeader("Content-Type", "application/json");
  const int code = http.POST(body);
  http.end();
  if (code == 200 || code == 201 || code == 204) return true;
  rejected = code >= 400 && code < 500 && code != 401 && code != 408 && code != 429;
  error = errorFor(code);
  return false;
}

bool BookOrbit::postProgressOnce(long fileId, float percentage, String& error) {
  NetGuard netGuard;
  Conn client;
  HTTPClient http;
  if (!begin(http, client, server_ + "/api/v1/books/files/" + String(fileId) + "/progress", token_, error)) return false;
  http.addHeader("Content-Type", "application/json");
  const int code = http.POST("{\"percentage\":" + String(percentage, 1) + "}");
  http.end();
  if (code != 200 && code != 201 && code != 204) {
    error = errorFor(code);
    return false;
  }
  return true;
}

bool BookOrbit::getProgressOnce(long fileId, float& percentage, String& error) {
  NetGuard netGuard;
  Conn client;
  HTTPClient http;
  if (!begin(http, client, server_ + "/api/v1/books/files/" + String(fileId) + "/progress", token_, error)) return false;
  const int code = http.GET();
  if (!isOk(code)) {
    error = errorFor(code);
    http.end();
    return false;
  }
  JsonDocument doc;
  BodyStream resp(http);
  const bool bad = static_cast<bool>(deserializeJson(doc, resp));
  http.end();
  percentage = doc["percentage"] | 0.0f;
  return !bad;
}

String BookOrbit::pathFor(const Book& book) { return "/BookOrbit/" + fileSafe(book.title) + ".epub"; }

bool BookOrbit::downloadOnce(const Book& book, String& path, String& error, void (*progress)(size_t, size_t)) {
  NetGuard netGuard;
  if (!book.fileId) {
    error = "No EPUB file for this book";
    return false;
  }
  Conn client;
  HTTPClient http;
  if (!begin(http, client, server_ + "/api/v1/books/files/" + String(book.fileId) + "/serve", token_, error)) return false;
  const int code = http.GET();
  if (!isOk(code)) {
    error = errorFor(code);
    http.end();
    return false;
  }
  SdMan.ensureDirectoryExists("/BookOrbit");
  path = pathFor(book);
  const String tmp = path + ".part";
  FsFile file = SdMan.open(tmp.c_str(), O_WRITE | O_CREAT | O_TRUNC);
  if (!file) {
    error = "Cannot write to SD card";
    http.end();
    return false;
  }
  const size_t total = http.getSize() > 0 ? http.getSize() : 0;
  size_t done = 0;
  uint8_t buf[1024];
  BodyStream in(http);
  for (;;) {
    const size_t n = in.readSome(buf, sizeof(buf));
    if (n == 0) break;
    if (file.write(buf, n) != n) {
      error = "SD write failed (card full?)";
      file.close();
      SdMan.remove(tmp.c_str());
      http.end();
      return false;
    }
    done += n;
    if (progress) progress(done, total);
  }
  file.close();
  http.end();
  if (done == 0 || (total && done != total)) {
    error = "Download incomplete";
    SdMan.remove(tmp.c_str());
    return false;
  }
  SdMan.remove(path.c_str());
  SdMan.rename(tmp.c_str(), path.c_str());
  return true;
}

// The access token can expire while the library is open; a 401 triggers one refresh and one retry.
bool BookOrbit::refresh() {
  NetGuard netGuard;
  if (refreshToken_.isEmpty()) return false;
  Conn client;
  HTTPClient http;
  String error;
  if (!begin(http, client, server_ + "/api/v1/auth/refresh", "", error)) return false;
  http.addHeader("Content-Type", "application/json");
  const int code = http.POST("{\"refreshToken\":\"" + refreshToken_ + "\"}");
  JsonDocument doc;
  BodyStream resp(http);
  const bool bad = !isOk(code) || static_cast<bool>(deserializeJson(doc, resp));
  http.end();
  refreshToken_ = "";  // refresh tokens are single-use
  if (bad || String(doc["accessToken"] | "").isEmpty()) return false;
  token_ = doc["accessToken"].as<String>();
  refreshToken_ = doc["refreshToken"] | "";
  Serial.println("access token refreshed");
  return true;
}

static bool unauthorized(const String& error) { return error.startsWith("Unauthorized"); }

int BookOrbit::listBooks(int page, int pageSize, Book* out, int max, int& total, String& error) {
  int n = listBooksOnce(page, pageSize, out, max, total, error);
  if (n < 0 && unauthorized(error) && refresh()) n = listBooksOnce(page, pageSize, out, max, total, error);
  return n;
}

bool BookOrbit::fetchContinueReading(Book& book, float& progress, bool& found, String& error) {
  bool ok = fetchContinueReadingOnce(book, progress, found, error);
  if (!ok && unauthorized(error) && refresh()) ok = fetchContinueReadingOnce(book, progress, found, error);
  return ok;
}

bool BookOrbit::fetchThumbnail(long bookId, const String& path, String& error) {
  bool ok = fetchThumbnailOnce(bookId, path, error);
  if (!ok && unauthorized(error) && refresh()) ok = fetchThumbnailOnce(bookId, path, error);
  return ok;
}

bool BookOrbit::fetchWidgets(const char* const* types, int count, String& json, String& error) {
  bool ok = fetchWidgetsOnce(types, count, json, error);
  if (!ok && unauthorized(error) && refresh()) ok = fetchWidgetsOnce(types, count, json, error);
  return ok;
}


bool BookOrbit::postSession(long fileId, const String& body, bool& rejected, String& error) {
  bool ok = postSessionOnce(fileId, body, rejected, error);
  if (!ok && unauthorized(error) && refresh()) ok = postSessionOnce(fileId, body, rejected, error);
  return ok;
}

bool BookOrbit::postProgress(long fileId, float percentage, String& error) {
  bool ok = postProgressOnce(fileId, percentage, error);
  if (!ok && unauthorized(error) && refresh()) ok = postProgressOnce(fileId, percentage, error);
  return ok;
}

bool BookOrbit::getProgress(long fileId, float& percentage, String& error) {
  bool ok = getProgressOnce(fileId, percentage, error);
  if (!ok && unauthorized(error) && refresh()) ok = getProgressOnce(fileId, percentage, error);
  return ok;
}

bool BookOrbit::download(const Book& book, String& path, String& error, void (*progress)(size_t, size_t)) {
  bool ok = downloadOnce(book, path, error, progress);
  if (!ok && unauthorized(error) && refresh()) ok = downloadOnce(book, path, error, progress);
  return ok;
}
