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
// Table of contents. Titles are drawn with the book's own face once uiFont(true) is called (call uiFont(false) when done).
void uiFont(bool on);
int tocCount();
const char* tocTitle(int i);
int tocDepth(int i);
int tocCurrent();      // the entry the reader is in, -1 when unknown
bool openToc(int i);   // jumps to the start of the entry's chapter and redraws; false if it can't be found
int fontSize();               // base text size in px of the open book
bool setFontSize(int px);     // re-lays out and redraws at the same place in the text; saved for all books
inline constexpr int kFontSizes[] = {18, 20, 22, 26, 30};  // px, smallest to largest
float progressPercent();  // 0-100, estimated from the chapter and page position
}  // namespace reader
