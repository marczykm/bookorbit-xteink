#include "stats.h"

#include <ArduinoJson.h>
#include <SDCardManager.h>

#include "bookorbit.h"

namespace {
constexpr const char* kStatsFile = "/BookOrbit/.stats.json";
constexpr const char* kStatTypes[] = {"reading-streak", "reading-goal", "reading-rhythm", "year-projection", "monthly-challenge", "library-overview"};
}  // namespace

Stats stats;

static void parseStats(const String& json) {
  stats = Stats();
  JsonDocument doc;
  if (deserializeJson(doc, json)) return;
  const auto addNow = [](JsonObject d) {
    if (d.isNull() || stats.nowCount == Stats::kMaxNow) return;
    NowBook& b = stats.now[stats.nowCount++];
    b.id = d["id"] | 0L;
    b.fileId = d["fileId"] | 0L;
    strlcpy(b.title, d["title"] | "", sizeof(b.title));
    strlcpy(b.author, d["author"] | "", sizeof(b.author));
    b.progress = d["progress"] | 0.0f;
  };
  if (JsonArray books = doc["reading-now"]; !books.isNull()) {
    for (JsonObject d : books) addNow(d);
  } else {
    addNow(doc["reading-now"]);  // a cache from 0.0.9 or older: one book
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

void loadStats() {
  if (SdMan.exists(kStatsFile)) parseStats(SdMan.readFile(kStatsFile));
}

bool refreshStats(BookOrbit& api) {
  String json, error;
  if (!api.fetchWidgets(kStatTypes, sizeof(kStatTypes) / sizeof(kStatTypes[0]), json, error)) {
    Serial.printf("stats: %s\n", error.c_str());
    return false;
  }
  Book now[Stats::kMaxNow];
  float progress[Stats::kMaxNow];
  const int n = api.fetchContinueReading(now, progress, Stats::kMaxNow, error);
  if (n < 0) Serial.printf("continue reading: request failed: %s\n", error.c_str());
  else Serial.printf("continue reading: %d in progress%s%s\n", n, n ? ", first: " : "", n ? now[0].title : "");
  if (n > 0) {  // add the books being read to the cached document
    JsonDocument doc;
    deserializeJson(doc, json);
    JsonArray books = doc["reading-now"].to<JsonArray>();
    for (int i = 0; i < n; i++) {
      JsonObject b = books.add<JsonObject>();
      b["id"] = now[i].id;
      b["fileId"] = now[i].fileId;
      b["title"] = now[i].title;
      b["author"] = now[i].author;
      b["progress"] = progress[i];
    }
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

void clearStats() {
  SdMan.remove(kStatsFile);
  stats = Stats();
}
