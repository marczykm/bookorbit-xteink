#pragma once
// Book cover thumbnails for the library list: downloaded once, shrunk to a 48x72 1-bit image and cached on the SD card.
#include <Arduino.h>

namespace cover {
constexpr int W = 48, H = 72;
constexpr int kBytes = (W / 8) * H;  // 1 bit per pixel, MSB first, 1 = black

// True when a cover (or a "no usable cover" marker) is already cached, so nothing needs fetching.
bool cached(long bookId);
// Reads the cached 1-bit image into `bits` (kBytes). False when the book has none.
bool load(long bookId, uint8_t* bits);
// Where the downloaded thumbnail is staged before conversion.
String stagingPath();
// Converts the staged PNG or baseline JPEG to the cached 1-bit image; on a file it cannot decode, stores a marker so it is not retried.
bool convertStaged(long bookId, String& error);
// Marks the book as having no cover (download failed for good).
void markNone(long bookId);
}  // namespace cover
