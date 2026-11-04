#pragma once
// What waits on the SD card for the server: reading progress and reading sessions. The reader has no network, so both are
// written to files and sent the next time the device is online.
#include <Arduino.h>

class BookOrbit;

namespace outbox {
// Progress: one "<fileId> <percent>" line per book. The furthest position is the source of truth: a book's entry only ever
// moves forward, and the server is only told when it is behind.
void queueProgress(long fileId, float percent);
void flushProgress(BookOrbit& api);
// Sessions: one JSON line per session ({"fileId":..,"session":{..}}); queueing a session again replaces its earlier copy
// (matched by `id`, the session's UUID).
void queueSession(const String& line, const char* id);
void flushSessions(BookOrbit& api);
bool pending();  // anything waiting to be sent
void clear();    // drops everything (switching to another server)
}  // namespace outbox
