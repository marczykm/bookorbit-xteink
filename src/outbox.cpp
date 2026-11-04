#include "outbox.h"

#include <ArduinoJson.h>
#include <SDCardManager.h>

#include "bookorbit.h"

namespace outbox {
namespace {
constexpr const char* kProgressFile = "/BookOrbit/.progress";
constexpr const char* kSessionFile = "/BookOrbit/.sessions";

String readAll(const char* path) { return SdMan.exists(path) ? SdMan.readFile(path) : String(); }

// Calls f(line) for each non-empty line of `text`.
template <typename F>
void forEachLine(const String& text, F f) {
  for (int start = 0; start < (int)text.length();) {
    int nl = text.indexOf('\n', start);
    if (nl < 0) nl = text.length();
    const String line = text.substring(start, nl);
    start = nl + 1;
    if (line.length()) f(line);
  }
}

// Writes what is still waiting, or removes the file when nothing is.
void store(const char* path, const String& lines) {
  if (lines.isEmpty()) {
    SdMan.remove(path);
    return;
  }
  SdMan.ensureDirectoryExists("/BookOrbit");
  SdMan.writeFile(path, lines);
}
}  // namespace

void queueProgress(long fileId, float percent) {
  if (!fileId) return;
  String out;
  forEachLine(readAll(kProgressFile), [&](const String& line) {
    if (atol(line.c_str()) != fileId) {  // keep the other books' entries
      out += line + "\n";
      return;
    }
    const int sp = line.indexOf(' ');  // this book was already waiting: never move it backwards
    if (sp > 0) percent = max(percent, line.substring(sp + 1).toFloat());
  });
  out += String(fileId) + " " + String(percent, 1) + "\n";
  store(kProgressFile, out);
}

void flushProgress(BookOrbit& api) {
  if (!SdMan.exists(kProgressFile)) return;
  String left;
  forEachLine(readAll(kProgressFile), [&](const String& line) {
    const long id = atol(line.c_str());
    const int sp = line.indexOf(' ');
    if (!id || sp < 0) return;
    String error;
    const float local = line.substring(sp + 1).toFloat();
    float server = 0;
    if (api.getProgress(id, server, error) && server >= local) {  // the server is already as far or further: keep it
      Serial.printf("progress kept: file %ld is at %.1f%% on the server (device %.1f%%)\n", id, server, local);
    } else if (api.postProgress(id, local, error)) {
      Serial.printf("progress synced: file %ld at %s%%\n", id, line.substring(sp + 1).c_str());
    } else {
      Serial.printf("progress sync failed: %s\n", error.c_str());
      left += line + "\n";
    }
  });
  store(kProgressFile, left);
}

void queueSession(const String& line, const char* id) {
  String out;
  forEachLine(readAll(kSessionFile), [&](const String& l) {
    if (l.indexOf(id) < 0) out += l + "\n";  // drop the earlier copy of this session
  });
  out += line + "\n";
  store(kSessionFile, out);
}

void flushSessions(BookOrbit& api) {
  if (!SdMan.exists(kSessionFile)) return;
  String left;
  forEachLine(readAll(kSessionFile), [&](const String& line) {
    JsonDocument doc;
    if (deserializeJson(doc, line)) return;  // unreadable: drop it
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
  });
  store(kSessionFile, left);
}

bool pending() { return SdMan.exists(kProgressFile) || SdMan.exists(kSessionFile); }

void clear() {
  SdMan.remove(kProgressFile);
  SdMan.remove(kSessionFile);
}
}  // namespace outbox
