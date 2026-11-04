#pragma once
// The Reading now book's cover: downloaded once and cached on the SD card as a small 48x72 1-bit image (the Home card) and a
// large 336x504 one, error-diffused for quality (the sleep screen).
#include <Arduino.h>

namespace cover {
constexpr int W = 48, H = 72;
constexpr int kBytes = (W / 8) * H;  // 1 bit per pixel, MSB first, 1 = black
constexpr int BigW = 336, BigH = 504;
constexpr int kBigBytes = (BigW / 8) * BigH;

// True when a cover (or a "no usable cover" marker) is already cached, so nothing needs fetching.
bool cached(long bookId);
// Reads the cached 1-bit image into `bits` (kBytes). False when the book has none.
bool load(long bookId, uint8_t* bits);
// The same for the large image (kBigBytes).
bool loadBig(long bookId, uint8_t* bits);
// Reads `rows` rows of the large image starting at `firstRow` into `bits`, so it can be drawn in strips without holding it all in RAM.
bool loadBigRows(long bookId, int firstRow, int rows, uint8_t* bits);
// True when the large image is cached.
bool hasBig(long bookId);
// Where the downloaded thumbnail is staged before conversion.
String stagingPath();
// Converts the staged PNG or baseline JPEG to the cached 1-bit image; on a file it cannot decode, stores a marker so it is not retried.
bool convertStaged(long bookId, String& error);
// Marks the book as having no cover (download failed for good).
void markNone(long bookId);
}  // namespace cover
