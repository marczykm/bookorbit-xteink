#include "gfx.h"

#include <Arduino.h>
#include <math.h>
#include <FreeInkUIFont.h>
#include <esp_partition.h>
#include <render/TtfFont.h>

namespace gfx {
namespace {
EInkDisplay* disp = nullptr;
const freeink::ui::BitmapFont& font = freeink::ui::kNotoSansFont;

// Optional Noto Serif from the flash `fonts` partition; it has the Polish letters the bitmap font lacks.
constexpr uint16_t kTtfSize = 22;
constexpr size_t kTtfArena = 14 * 1024;
freeink::book::TtfFont ttf;
freeink::font::Arena ttfArena;
bool ttfOn = false;
freeink::book::TtfFont* face = &ttf;  // the face text is drawn with: the UI font, or one lent by the reader
bool ttfOnOwn = false;

// Portrait is the panel rotated 90 degrees clockwise.
inline void setPixel(int x, int y, bool black) {
  if (x < 0 || y < 0 || x >= W || y >= H) return;
  const int phyX = y, phyY = disp->getDisplayHeight() - 1 - x;
  uint8_t& b = disp->getFrameBuffer()[phyY * disp->getDisplayWidthBytes() + phyX / 8];
  const uint8_t mask = 0x80 >> (phyX % 8);
  if (black) b &= ~mask; else b |= mask;
}

// Next UTF-8 code point, folded to something the ASCII font has.
uint32_t nextChar(const char*& p) {
  const uint8_t c = *p++;
  if (c < 0x80) return c;
  uint32_t cp = 0;
  int extra = 0;
  if ((c & 0xE0) == 0xC0) { cp = c & 0x1F; extra = 1; }
  else if ((c & 0xF0) == 0xE0) { cp = c & 0x0F; extra = 2; }
  else if ((c & 0xF8) == 0xF0) { cp = c & 0x07; extra = 3; }
  while (extra-- > 0 && (*p & 0xC0) == 0x80) cp = (cp << 6) | (*p++ & 0x3F);
  if (ttfOn) return cp;
  switch (cp) {
    case 0x105: return 'a'; case 0x104: return 'A'; case 0x107: return 'c'; case 0x106: return 'C';
    case 0x119: return 'e'; case 0x118: return 'E'; case 0x142: return 'l'; case 0x141: return 'L';
    case 0x144: return 'n'; case 0x143: return 'N'; case 0xF3: return 'o';  case 0xD3: return 'O';
    case 0x15B: return 's'; case 0x15A: return 'S'; case 0x17A: case 0x17C: return 'z';
    case 0x179: case 0x17B: return 'Z';
    case 0x2013: case 0x2014: return '-'; case 0x2018: case 0x2019: return '\''; case 0x201C: case 0x201D: return '"';
    default: return '?';
  }
}

const freeink::ui::FontGlyph* glyphFor(uint32_t cp) {
  if (cp < font.first || cp > font.last) cp = '?';
  return &font.glyphs[cp - font.first];
}

int advanceOf(uint32_t cp) { return ttfOn ? face->advance(cp, kTtfSize, 0) : glyphFor(cp)->xAdvance; }
}  // namespace

void begin(EInkDisplay* display) { disp = display; }

bool loadUiFont() {
  const esp_partition_t* part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, (esp_partition_subtype_t)0x40, "fonts");
  if (!part) return false;
  const void* mem = nullptr;
  esp_partition_mmap_handle_t handle;
  if (esp_partition_mmap(part, 0, part->size, ESP_PARTITION_MMAP_DATA, &mem, &handle) != ESP_OK) return false;
  const uint8_t* pack = static_cast<const uint8_t*>(mem);
  uint32_t count;
  memcpy(&count, pack + 4, 4);
  if (memcmp(pack, "FPK1", 4) != 0 || count == 0) return false;
  uint32_t style, offset, length;  // the first entry is the regular face
  memcpy(&style, pack + 8, 4);
  memcpy(&offset, pack + 12, 4);
  memcpy(&length, pack + 16, 4);
  uint8_t* buf = style == 0 && offset + length <= part->size ? static_cast<uint8_t*>(malloc(kTtfArena)) : nullptr;
  if (!buf) return false;
  ttfArena.init(buf, kTtfArena);
  if (!ttf.init(pack + offset, length, ttfArena)) return false;
  ttf.setEmSizing(true);
  ttfOn = true;
  ttfOnOwn = true;
  return true;
}

void lendFace(freeink::font::TtfFont* lent) {
  face = lent ? lent : &ttf;
  ttfOn = lent ? true : ttfOnOwn;
}

namespace {
bool washed = false;  // the screen behind a modal is already faded
}

void clear(bool black) {
  memset(disp->getFrameBuffer(), black ? 0x00 : 0xFF, disp->getBufferSize());
  washed = false;
}

void blit1bit(int x, int y, const uint8_t* bits, int w, int h) {
  const int stride = (w + 7) / 8;
  for (int j = 0; j < h; j++)
    for (int i = 0; i < w; i++) setPixel(x + i, y + j, bits[j * stride + i / 8] & (0x80 >> (i & 7)));
}

void fillRect(int x, int y, int w, int h, bool black) {
  for (int j = 0; j < h; j++)
    for (int i = 0; i < w; i++) setPixel(x + i, y + j, black);
}

int lineHeight() { return ttfOn ? face->lineHeight(kTtfSize) : font.yAdvance; }

int textWidth(const char* s) {
  int w = 0;
  while (*s) w += advanceOf(nextChar(s));
  return w;
}

void text(int x, int y, const char* s, bool black) {
  const int baseline = y + (ttfOn ? face->ascent(kTtfSize) : font.ascent);
  while (*s) {
    if (ttfOn) {
      const uint32_t cp = nextChar(s);
      if (const auto* g = face->rasterize(cp, kTtfSize)) {
        for (int row = 0; row < g->height; row++)
          for (int col = 0; col < g->width; col++)
            if (g->pixels[row * g->width + col] > 110) setPixel(x + g->xoff + col, baseline + g->yoff + row, black);
      }
      x += face->advance(cp, kTtfSize, 0);
      continue;
    }
    const auto* g = glyphFor(nextChar(s));
    uint32_t bit = 0;  // glyph bitmaps are one continuous MSB-first bit stream
    for (int row = 0; row < g->height; row++) {
      for (int col = 0; col < g->width; col++, bit++) {
        if (font.bitmap[g->bitmapOffset + (bit >> 3)] & (0x80 >> (bit & 7)))
          setPixel(x + g->xOffset + col, baseline + g->yOffset + row, black);
      }
    }
    x += g->xAdvance;
  }
}

int bigTextWidth(const char* s, int sizePx) {
  if (!ttfOn) return textWidth(s) * max(1, sizePx / font.yAdvance);
  int w = 0;
  for (const char* p = s; *p;) w += face->advance(nextChar(p), sizePx, 0);
  return w;
}

void bigText(int x, int y, const char* s, int sizePx, bool black) {
  if (ttfOn) {
    const int baseline = y + face->ascent(sizePx);
    for (const char* p = s; *p;) {
      const uint32_t cp = nextChar(p);
      if (const auto* g = face->rasterize(cp, sizePx)) {
        for (int row = 0; row < g->height; row++)
          for (int col = 0; col < g->width; col++)
            if (g->pixels[row * g->width + col] > 110) setPixel(x + g->xoff + col, baseline + g->yoff + row, black);
      }
      x += face->advance(cp, sizePx, 0);
    }
    return;
  }
  const int scale = max(1, sizePx / font.yAdvance);  // no TTF: blow the bitmap font up
  for (const char* p = s; *p;) {
    const auto* g = glyphFor(nextChar(p));
    uint32_t bit = 0;
    for (int row = 0; row < g->height; row++)
      for (int col = 0; col < g->width; col++, bit++)
        if (font.bitmap[g->bitmapOffset + (bit >> 3)] & (0x80 >> (bit & 7)))
          fillRect(x + (g->xOffset + col) * scale, y + (font.ascent + g->yOffset + row) * scale, scale, scale, black);
    x += g->xAdvance * scale;
  }
}

void bigTextCentered(int y, const char* s, int sizePx, bool black) { bigText((W - bigTextWidth(s, sizePx)) / 2, y, s, sizePx, black); }

void batteryIcon(int x, int y, int percent, bool black) {
  const int bodyW = 34, h = 18;
  fillRect(x, y, bodyW, h, black);                        // outline: fill, then hollow out
  fillRect(x + 2, y + 2, bodyW - 4, h - 4, !black);
  fillRect(x + bodyW, y + 5, 3, h - 10, black);           // terminal nub
  const int inner = bodyW - 8;
  fillRect(x + 4, y + 4, inner * constrain(percent, 0, 100) / 100, h - 8, black);
}

void wifiIcon(int x, int y, int arcs, bool slash, bool black, bool grey) {
  const int cx = x + 13, cy = y + 19;
  for (int dy = -21; dy <= 0; dy++) {
    for (int dx = -13; dx <= 13; dx++) {
      const float d = sqrtf((float)(dx * dx + dy * dy));
      bool on = d <= 3.0f;  // the dot
      for (int a = 0; a < arcs && !on; a++) {
        const float r = 9.0f + 6.0f * a;
        on = d >= r - 1.7f && d <= r + 1.7f && abs(dx) <= -dy;  // quarter-circle fan, 45 degrees either side
      }
      if (on && (!grey || ((cx + dx + cy + dy) & 1) == 0)) setPixel(cx + dx, cy + dy, black);  // grey = checkerboard
    }
  }
  if (slash) {
    for (int i = 0; i < 24; i++) {
      for (int t = 0; t < 3; t++) setPixel(x + 1 + i + t, y + 21 - i * 22 / 24, black);
    }
  }
}

const char* ellipsize(const char* s, int maxWidth) {
  static char out[160];
  if (textWidth(s) <= maxWidth) {
    strlcpy(out, s, sizeof(out));
    return out;
  }
  const int dots = textWidth("...");
  size_t n = 0;
  int w = 0;
  const char* p = s;
  while (*p && n < sizeof(out) - 4) {
    const char* start = p;
    const int adv = advanceOf(nextChar(p));
    if (w + adv + dots > maxWidth) break;
    memcpy(out + n, start, p - start);
    n += p - start;
    w += adv;
  }
  strlcpy(out + n, "...", sizeof(out) - n);
  return out;
}

void flush(EInkDisplay::RefreshMode mode) { disp->displayBufferAsync(mode); }  // falls back to blocking if RAM is short
bool busy() { return disp->refreshBusy(); }
void waitIdle() { disp->waitRefreshComplete(); }
void setLowMemory(bool on) { disp->setAsyncShadowAllowed(!on); }

void dumpFramebuffer() {
  Serial.println("FB-BEGIN");
  const uint8_t* fb = disp->getFrameBuffer();
  for (uint32_t i = 0; i < disp->getBufferSize(); i++) Serial.printf("%02x", fb[i]);
  Serial.println("\nFB-END");
}
}  // namespace gfx

namespace gfx {
namespace {
void roundRect(int x, int y, int w, int h, int r, bool black) {
  for (int j = 0; j < h; j++) {
    const int fromEdge = j < r ? r - j : (j >= h - r ? j - (h - r) + 1 : 0);
    int inset = 0;
    if (fromEdge > 0) inset = r - (int)sqrtf((float)(r * r - (fromEdge - 0.5f) * (fromEdge - 0.5f)));
    fillRect(x + inset, y + j, w - 2 * inset, 1, black);
  }
}
}  // namespace

void fadeBackground() {
  if (washed) return;  // fade what is behind a box: whiten every other pixel in a checkerboard, so black becomes grey
  uint8_t* fb = disp->getFrameBuffer();
  const int stride = disp->getDisplayWidthBytes(), rows = disp->getDisplayHeight();
  for (int r = 0; r < rows; r++) {
    const uint8_t mask = (r & 1) ? 0xAA : 0x55;
    for (int b = 0; b < stride; b++) fb[r * stride + b] |= mask;
  }
  washed = true;
}

void modal(const char* title, const char* body) {
  waitIdle();
  fadeBackground();
  const int w = 300, h = 80, x = (W - w) / 2, y = 330, r = 10;
  roundRect(x, y, w, h, r, true);
  roundRect(x + 2, y + 2, w - 4, h - 4, r - 2, false);
  bigTextCentered(y + 10, title, 22);
  bigTextCentered(y + 44, ellipsize(body, w - 24), 17);
  flush(EInkDisplay::FAST_REFRESH);
}
}  // namespace gfx
