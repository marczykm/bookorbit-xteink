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
  http.useHTTP10(true);  // no chunked bodies, so the stream can be parsed directly
  if (!(https ? http.begin(conn.tls, url) : http.begin(conn.plain, url))) {
    error = "Bad server address";
    return false;
  }
  if (token.length()) http.addHeader("Authorization", "Bearer " + token);
  return true;
}

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

void BookOrbit::configure(const String& server, bool insecure) {
  server_ = server;
  insecureTls = insecure;
}

bool BookOrbit::login(const String& user, const String& password, String& error) {
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
  if (code != 200) {
    error = code == 401 ? "Wrong username or password" : errorFor(code);
    http.end();
    return false;
  }
  JsonDocument doc;
  const bool bad = static_cast<bool>(deserializeJson(doc, http.getStream()));
  http.end();
  token_ = doc["accessToken"] | "";
  refreshToken_ = doc["refreshToken"] | "";
  if (bad || token_.isEmpty()) {
    error = "Unexpected login response";
    return false;
  }
  return true;
}

int BookOrbit::listBooksOnce(int page, int pageSize, Book* out, int max, int& total, String& error) {
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
  if (code != 200) {
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
  const auto err = deserializeJson(doc, http.getStream(), DeserializationOption::Filter(filter));
  http.end();
  if (err) {
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
  if (code != 200) {
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
  const auto err = deserializeJson(doc, http.getStream(), DeserializationOption::Filter(filter));
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

bool BookOrbit::fetchWidgetsOnce(const char* const* types, int count, String& json, String& error) {
  Conn client;
  HTTPClient http;
  if (!begin(http, client, server_ + "/api/v1/dashboard/widgets/batch", token_, error)) return false;
  http.addHeader("Content-Type", "application/json");
  JsonDocument body;
  for (int i = 0; i < count; i++) body["widgets"][i] = types[i];
  String payload;
  serializeJson(body, payload);
  const int code = http.POST(payload);
  if (code != 200) {
    error = errorFor(code);
    http.end();
    return false;
  }
  JsonDocument doc;
  const auto err = deserializeJson(doc, http.getStream());
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

bool BookOrbit::fetchThumbnailOnce(long bookId, const String& path, String& error) {
  Conn client;
  HTTPClient http;
  if (!begin(http, client, server_ + "/api/v1/books/" + String(bookId) + "/thumbnail", token_, error)) return false;
  const int code = http.GET();
  if (code != 200) {
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
  size_t done = 0;
  uint8_t buf[1024];
  Stream& in = http.getStream();
  uint32_t lastData = millis();
  while ((http.connected() || in.available()) && millis() - lastData < kTimeoutMs) {
    const int avail = in.available();
    if (avail > 0) {
      const int n = in.readBytes(buf, min<size_t>(avail, sizeof(buf)));
      if (n > 0 && file.write(buf, n) != (size_t)n) break;
      done += n;
      lastData = millis();
      if (done > 1024 * 1024) break;  // a thumbnail this big is not a thumbnail
    } else {
      delay(5);
    }
  }
  file.close();
  http.end();
  if (done == 0) {
    SdMan.remove(path.c_str());
    error = "Empty thumbnail";
    return false;
  }
  return true;
}

bool BookOrbit::postSessionOnce(long fileId, const String& body, bool& rejected, String& error) {
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
  Conn client;
  HTTPClient http;
  if (!begin(http, client, server_ + "/api/v1/books/files/" + String(fileId) + "/progress", token_, error)) return false;
  const int code = http.GET();
  if (code != 200) {
    error = errorFor(code);
    http.end();
    return false;
  }
  JsonDocument doc;
  const bool bad = static_cast<bool>(deserializeJson(doc, http.getStream()));
  http.end();
  percentage = doc["percentage"] | 0.0f;
  return !bad;
}

String BookOrbit::pathFor(const Book& book) { return "/BookOrbit/" + fileSafe(book.title) + ".epub"; }

bool BookOrbit::downloadOnce(const Book& book, String& path, String& error, void (*progress)(size_t, size_t)) {
  if (!book.fileId) {
    error = "No EPUB file for this book";
    return false;
  }
  Conn client;
  HTTPClient http;
  if (!begin(http, client, server_ + "/api/v1/books/files/" + String(book.fileId) + "/serve", token_, error)) return false;
  const int code = http.GET();
  if (code != 200) {
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
  Stream& in = http.getStream();
  uint32_t lastData = millis();
  while (http.connected() || in.available()) {
    const int n = in.readBytes(buf, in.available() ? min<size_t>(in.available(), sizeof(buf)) : 0);
    if (n > 0) {
      if (file.write(buf, n) != (size_t)n) {
        error = "SD write failed (card full?)";
        file.close();
        SdMan.remove(tmp.c_str());
        http.end();
        return false;
      }
      done += n;
      lastData = millis();
      if (progress) progress(done, total);
    } else if (millis() - lastData > kTimeoutMs) {
      break;
    } else {
      delay(5);
    }
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
  if (refreshToken_.isEmpty()) return false;
  Conn client;
  HTTPClient http;
  String error;
  if (!begin(http, client, server_ + "/api/v1/auth/refresh", "", error)) return false;
  http.addHeader("Content-Type", "application/json");
  const int code = http.POST("{\"refreshToken\":\"" + refreshToken_ + "\"}");
  JsonDocument doc;
  const bool bad = code != 200 || static_cast<bool>(deserializeJson(doc, http.getStream()));
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

bool BookOrbit::fetchWidgets(const char* const* types, int count, String& json, String& error) {
  bool ok = fetchWidgetsOnce(types, count, json, error);
  if (!ok && unauthorized(error) && refresh()) ok = fetchWidgetsOnce(types, count, json, error);
  return ok;
}

bool BookOrbit::fetchThumbnail(long bookId, const String& path, String& error) {
  bool ok = fetchThumbnailOnce(bookId, path, error);
  if (!ok && unauthorized(error) && refresh()) ok = fetchThumbnailOnce(bookId, path, error);
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
