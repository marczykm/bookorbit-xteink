#include "updater.h"

#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <Update.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>

#ifndef FW_VERSION
#define FW_VERSION "dev"
#endif
#ifndef UPDATE_API
#define UPDATE_API "https://api.github.com/repos/marczykm/bookorbit-xteink/releases/latest"
#endif

extern const uint8_t rootca_crt_bundle_start[] asm("_binary_x509_crt_bundle_start");
extern const uint8_t rootca_crt_bundle_end[] asm("_binary_x509_crt_bundle_end");

namespace updater {
namespace {
struct Conn {
  WiFiClient plain;
  WiFiClientSecure tls;
};

bool open(HTTPClient& http, Conn& conn, const String& url, String& error) {
  const bool https = url.startsWith("https://");
  if (https) {
    if (time(nullptr) < 1700000000) {  // certificates are only valid within a date range
      configTime(0, 0, "pool.ntp.org", "time.google.com");
      for (int i = 0; i < 100 && time(nullptr) < 1700000000; i++) delay(100);
      if (time(nullptr) < 1700000000) {
        error = "Could not get the time";
        return false;
      }
    }
    conn.tls.setCACertBundle(rootca_crt_bundle_start, rootca_crt_bundle_end - rootca_crt_bundle_start);
  }
  http.setConnectTimeout(15000);
  http.setTimeout(15000);
  http.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);  // install() turns this off and follows by hand
  http.setUserAgent("bookorbit-xteink");
  if (!(https ? http.begin(conn.tls, url) : http.begin(conn.plain, url))) {
    error = "Bad update address";
    return false;
  }
  return true;
}

// "v1.2.3" -> 10203 (missing parts count as 0).
long versionNumber(const char* v) {
  if (*v == 'v' || *v == 'V') v++;
  long parts[3] = {0, 0, 0};
  for (int i = 0; i < 3 && *v; i++) {
    parts[i] = atol(v);
    while (*v && *v != '.') v++;
    if (*v == '.') v++;
  }
  return parts[0] * 10000 + parts[1] * 100 + parts[2];
}
}  // namespace

const char* currentVersion() { return FW_VERSION; }

namespace {
String assetPrefix = "bookorbit-x4-";  // the asset name starts with this and ends with "-ota.bin"
}
void setDevice(const char* name) { assetPrefix = String("bookorbit-") + name + "-"; }

int check(Info& info, String& error) {
  HTTPClient http;
  Conn conn;
  if (!open(http, conn, UPDATE_API, error)) return -1;
  http.addHeader("Accept", "application/vnd.github+json");
  const int code = http.GET();
  if (code != 200) {
    error = code > 0 ? "GitHub answered " + String(code) : "Could not reach GitHub";
    http.end();
    return -1;
  }
  const String body = http.getString();
  http.end();
  JsonDocument filter;
  filter["tag_name"] = true;
  filter["assets"][0]["name"] = true;
  filter["assets"][0]["browser_download_url"] = true;
  filter["assets"][0]["size"] = true;
  JsonDocument doc;
  if (deserializeJson(doc, body, DeserializationOption::Filter(filter))) {
    error = "Unreadable answer from GitHub";
    return -1;
  }
  const char* tag = doc["tag_name"] | "";
  for (JsonObject a : doc["assets"].as<JsonArray>()) {
    String name = a["name"] | "";
    if (!name.startsWith(assetPrefix) || !name.endsWith("-ota.bin")) continue;
    const char* v = tag[0] == 'v' ? tag + 1 : tag;
    strlcpy(info.version, v, sizeof(info.version));
    strlcpy(info.url, a["browser_download_url"] | "", sizeof(info.url));
    info.size = a["size"] | 0;
    if (!info.url[0] || !info.size) break;
    return versionNumber(tag) > versionNumber(FW_VERSION) ? 1 : 0;
  }
  error = "No update image found";
  return -1;
}

namespace {
struct Hop {
  HTTPClient http;
  Conn conn;
};
}  // namespace

bool install(const Info& info, void (*progress)(int), String& error) {
  // Redirects are followed by hand with a fresh connection each (the TLS client loses its CA bundle when reused for another host).
  Hop* hop = nullptr;
  String url = info.url;
  int code = 0;
  for (int i = 0; i < 5; i++) {
    delete hop;
    hop = new Hop;
    hop->http.setFollowRedirects(HTTPC_DISABLE_FOLLOW_REDIRECTS);
    if (!open(hop->http, hop->conn, url, error)) {
      delete hop;
      return false;
    }
    hop->http.setFollowRedirects(HTTPC_DISABLE_FOLLOW_REDIRECTS);
    code = hop->http.GET();
    if (code != 301 && code != 302 && code != 303 && code != 307 && code != 308) break;
    url = hop->http.getLocation();
    hop->http.end();
    if (!url.length()) break;
  }
  HTTPClient& http = hop->http;
  struct Free {
    Hop* h;
    ~Free() { delete h; }
  } freeHop{hop};
  if (code != 200) {
    error = code > 0 ? "Download failed (" + String(code) + ")" : "Download failed: " + HTTPClient::errorToString(code);
    http.end();
    return false;
  }
  const int len = http.getSize();
  if (len > 0 && (size_t)len != info.size) {
    error = "Unexpected image size";
    http.end();
    return false;
  }
  if (!Update.begin(info.size)) {
    error = String("No room for the image: ") + Update.errorString();
    http.end();
    return false;
  }
  WiFiClient* stream = http.getStreamPtr();
  uint8_t buf[1024];
  size_t done = 0;
  int shown = -1;
  uint32_t lastData = millis();
  while (done < info.size) {
    const int avail = stream->available();
    if (avail <= 0) {
      if (!stream->connected() || millis() - lastData > 20000) break;
      delay(5);
      continue;
    }
    const int n = stream->readBytes(buf, min((size_t)avail, min(sizeof(buf), info.size - done)));
    if (n <= 0) continue;
    lastData = millis();
    if (Update.write(buf, n) != (size_t)n) {
      error = String("Write failed: ") + Update.errorString();
      Update.abort();
      http.end();
      return false;
    }
    done += n;
    const int pct = (int)(done * 100 / info.size);
    if (progress && pct / 10 != shown / 10) {
      shown = pct;
      progress(pct);
    }
  }
  http.end();
  if (done != info.size) {
    error = "Download interrupted";
    Update.abort();
    return false;
  }
  if (!Update.end(true)) {  // checks the image and switches the next boot to it
    error = String("Image rejected: ") + Update.errorString();
    return false;
  }
  return true;
}
}  // namespace updater
