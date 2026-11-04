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

void loadStats() {
  if (SdMan.exists(kStatsFile)) parseStats(SdMan.readFile(kStatsFile));
}

bool refreshStats(BookOrbit& api) {
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

void clearStats() {
  SdMan.remove(kStatsFile);
  stats = Stats();
}
