#pragma once
// EPUB reader on top of FreeInkBook: lays chapters out into an SD-card page cache and renders one page at a time.
#include <Arduino.h>

namespace reader {
// Opens the EPUB (laying out the first chapter if needed). Takes ~100 KB of heap, so turn Wi-Fi off first.
// `serverPercent` (0-100, or -1 for none) is jumped to when it is ahead of the saved position.
bool open(const char* epubPath, String& error, float serverPercent = -1);
void close();  // saves the position and frees memory
bool isOpen();
void draw(bool full);  // draws the current page and refreshes the panel
bool next(bool full = false);  // false at the end of the book
bool prev();                   // false at the start of the book
void savePosition();
float progressPercent();  // 0-100, estimated from the chapter and page position
}  // namespace reader
