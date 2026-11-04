#pragma once
// The Home statistics: the same dashboard widgets as the Android app, plus the book being read, cached on the SD card.
#include <Arduino.h>

class BookOrbit;

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
};
extern Stats stats;

void loadStats();                     // from the SD card cache
bool refreshStats(BookOrbit& api);    // fetches fresh numbers (online only) and caches them; true when they changed
void clearStats();                    // forgets them (switching to another server)
