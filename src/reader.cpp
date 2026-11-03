#include "reader.h"

#include <BookStorage.h>
#include <EInkDisplay.h>
#include <FreeInkBook.h>
#include <FreeInkUIBookFont.h>
#include <SDCardManager.h>
#include <esp_partition.h>
#include <cache/PageCache.h>
#include <css/Css.h>
#include <layout/ChapterLayout.h>
#include <render/PageRenderer.h>
#include <render/TtfFont.h>

#include "gfx.h"

using namespace freeink::book;

extern EInkDisplay display;

namespace reader {
namespace {

// Heap budgets (no PSRAM on the original X4). Wi-Fi must be off while a book is open.
constexpr size_t kBookArena = 16 * 1024;   // ZIP directory, OPF, TOC
constexpr size_t kScratchArena = 96 * 1024;  // layout + page render working memory
constexpr size_t kIndexArena = 4 * 1024;     // page index of the open chapter
constexpr size_t kSheetArena = 6 * 1024;     // book CSS
constexpr uint32_t kFontFingerprintBitmap = 0xB00C0001;  // built-in ASCII font
constexpr uint32_t kFontFingerprintTtf = 0xB00C0002;     // Noto Serif from the flash `fonts` partition; bump when it changes
constexpr size_t kFaceArena[3] = {14 * 1024, 8 * 1024, 8 * 1024};  // glyph caches: regular, bold, italic

constexpr const char* kFontSizeFile = "/BookOrbit/.fontsize";

class SdBookSource : public BookSource {
 public:
  bool open(const char* path) {
    file_ = SdMan.open(path, O_RDONLY);
    return static_cast<bool>(file_);
  }
  void close() { if (file_) file_.close(); }
  int32_t readAt(uint64_t offset, void* dst, uint32_t len) override {
    if (!file_.seekSet(static_cast<uint32_t>(offset))) return -1;
    return file_.read(dst, len);
  }
  uint64_t size() const override { return const_cast<FsFile&>(file_).fileSize(); }

 private:
  FsFile file_;
};

// Cache files live in one directory per book on the SD card; writes go to a temp name and are renamed on completion.
class SdCacheStorage : public CacheStorage {
 public:
  void setDir(const String& dir) {
    dir_ = dir;
    SdMan.ensureDirectoryExists(dir_.c_str());
    closeRead();
  }
  bool exists(const char* name) override { return SdMan.exists(path(name).c_str()); }
  bool remove(const char* name) override {
    closeRead();
    return SdMan.remove(path(name).c_str());
  }
  int64_t fileSize(const char* name) override {
    FsFile f = SdMan.open(path(name).c_str(), O_RDONLY);
    if (!f) return -1;
    const int64_t n = f.fileSize();
    f.close();
    return n;
  }
  int32_t readAt(const char* name, uint32_t offset, void* dst, uint32_t len) override {
    if (!readFile_ || readName_ != name) {
      closeRead();
      readFile_ = SdMan.open(path(name).c_str(), O_RDONLY);
      if (!readFile_) return -1;
      readName_ = name;
    }
    if (!readFile_.seekSet(offset)) return -1;
    return readFile_.read(dst, len);
  }
  bool beginWrite(const char* name) override {
    closeRead();
    writeName_ = name;
    SdMan.remove(tmpPath().c_str());
    writeFile_ = SdMan.open(tmpPath().c_str(), O_WRITE | O_CREAT | O_TRUNC);
    return static_cast<bool>(writeFile_);
  }
  bool write(const void* data, uint32_t len) override { return writeFile_ && writeFile_.write(data, len) == len; }
  bool endWrite() override {
    if (!writeFile_) return false;
    writeFile_.close();
    SdMan.remove(path(writeName_.c_str()).c_str());
    return SdMan.rename(tmpPath().c_str(), path(writeName_.c_str()).c_str());
  }
  void closeRead() {
    if (readFile_) readFile_.close();
    readName_ = "";
  }

 private:
  String path(const char* name) const { return dir_ + "/" + name; }
  String tmpPath() const { return dir_ + "/write.tmp"; }
  String dir_, readName_, writeName_;
  FsFile readFile_, writeFile_;
};

// All state of the open book.
struct State {
  uint8_t *bookBuf = nullptr, *scratchBuf = nullptr, *indexBuf = nullptr, *sheetBuf = nullptr;
  Arena bookArena, scratch, indexArena, sheetArena;
  SdBookSource source;
  SdCacheStorage cache;
  Book book;
  CssStylesheet sheet;
  freeink::ui::BitmapBookFont bitmapFont;
  uint8_t* faceBuf[3] = {nullptr, nullptr, nullptr};
  Arena faceArena[3];
  TtfFont faces[3];
  esp_partition_mmap_handle_t mapHandle = 0;
  bool mapped = false, ttf = false;
  FontChain fonts;
  LayoutParams params;
  uint32_t generation = 0;
  PageCacheReader pages;
  bool pagesOpen = false;
  int spine = 0;
  uint32_t page = 0;
  int turnsSinceSave = 0;
  bool open = false;
};
State* st = nullptr;

void freeBuffers() {
  if (!st) return;
  free(st->bookBuf);
  free(st->scratchBuf);
  free(st->indexBuf);
  free(st->sheetBuf);
  for (uint8_t* b : st->faceBuf) free(b);
  if (st->mapped) esp_partition_munmap(st->mapHandle);
  delete st;
  st = nullptr;
}

String cacheDirFor(const char* epubPath) {
  String name = epubPath;
  name = name.substring(name.lastIndexOf('/') + 1);
  name.replace(".epub", "");
  return "/BookOrbit/.cache/" + name;
}

// Lays out spine item `index` into the cache unless a current cache exists, then opens it.
void showBusy(const char* what);

// Set while opening a book so the "Preparing" notice only flashes up when a chapter actually has to be laid out.
const char* busyOnLayout = nullptr;

bool openChapter(int index, String& error) {
  State& s = *st;
  char name[64];
  pageCacheName(index, s.generation, name, sizeof(name));
  s.indexArena.reset();
  s.pagesOpen = false;
  if (s.pages.open(s.cache, name, s.generation, s.indexArena) != BookStatus::Ok) {
    const ManifestItem* item = s.book.spineItem(index);
    const ZipEntry* entry = item ? s.book.zip().find(item->href) : nullptr;
    if (!entry) {
      error = "Chapter missing from book";
      return false;
    }
    if (busyOnLayout) { showBusy(busyOnLayout); busyOnLayout = nullptr; }
    s.scratch.reset();
    Serial.printf("layout chapter %d ...\n", index);
    const uint32_t t0 = millis();
    PageCacheWriter writer;
    uint32_t totalChars = 0;
    if (!writer.begin(s.cache, name, s.generation, s.scratch)) {
      error = "Cannot start page cache";
      return false;
    }
    const BookStatus ls = ChapterLayout::layout(s.source, s.book.zip(), *entry, item->href, s.params, s.scratch,
                                                writer, nullptr, &totalChars);
    s.scratch.reset();
    Serial.printf("layout chapter %d: %s, %u pages, %u ms, scratch peak %u\n", index, bookStatusName(ls), (unsigned)writer.pageCount(), (unsigned)(millis() - t0), (unsigned)s.scratch.highWater());
    if (ls != BookStatus::Ok || writer.failed()) {
      error = String("Layout failed: ") + bookStatusName(ls);
      return false;
    }
    writer.setTotalChars(totalChars);
    if (!writer.finish()) {
      error = "Cannot write page cache";
      return false;
    }
    s.indexArena.reset();
    const BookStatus rs = s.pages.open(s.cache, name, s.generation, s.indexArena);
    if (rs != BookStatus::Ok) {
      error = String("Cache open failed: ") + bookStatusName(rs);
      return false;
    }
  }
  s.pagesOpen = true;
  s.spine = index;
  return true;
}

// Maps the font pack from the `fonts` flash partition and registers its faces; false leaves the chain empty.
bool loadFonts(State& s) {
  const esp_partition_t* part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, (esp_partition_subtype_t)0x40, "fonts");
  if (!part) { Serial.println("fonts: no `fonts` partition"); return false; }
  const void* mem = nullptr;
  if (esp_partition_mmap(part, 0, part->size, ESP_PARTITION_MMAP_DATA, &mem, &s.mapHandle) != ESP_OK) { Serial.println("fonts: mmap failed"); return false; }
  s.mapped = true;
  const uint8_t* pack = static_cast<const uint8_t*>(mem);
  if (memcmp(pack, "FPK1", 4) != 0) { Serial.println("fonts: bad pack header"); return false; }
  uint32_t count;
  memcpy(&count, pack + 4, 4);
  if (count > 4) return false;
  int added = 0;
  for (uint32_t i = 0; i < count; i++) {
    uint32_t style, offset, length;
    memcpy(&style, pack + 8 + 12 * i, 4);
    memcpy(&offset, pack + 12 + 12 * i, 4);
    memcpy(&length, pack + 16 + 12 * i, 4);
    if (style > 2 || offset + length > part->size) continue;  // bold-italic has no cache budget; the chain falls back
    if (!s.faceBuf[style]) { Serial.printf("fonts: no heap for face %u\n", (unsigned)style); return false; }
    s.faceArena[style].init(s.faceBuf[style], kFaceArena[style]);
    if (!s.faces[style].init(pack + offset, length, s.faceArena[style])) { Serial.printf("fonts: face %u init failed\n", (unsigned)style); return false; }
    s.faces[style].setEmSizing(true);
    if (!s.fonts.add(&s.faces[style], static_cast<uint8_t>(style))) return false;
    added++;
  }
  return added > 0;
}

void showBusy(const char* what) {
  gfx::fillRect(0, gfx::H / 2 - 40, gfx::W, 80, true);
  const int w = gfx::textWidth(what);
  gfx::text((gfx::W - w) / 2, gfx::H / 2 - gfx::lineHeight() / 2, what, false);
  gfx::flush(EInkDisplay::FAST_REFRESH);
}

}  // namespace

bool isOpen() { return st && st->open; }

void savePosition() {
  if (!isOpen() || !st->pagesOpen) return;
  const uint32_t chars = st->pages.charStart(st->page);
  char line[56];
  snprintf(line, sizeof(line), "%d %u %.1f", st->spine, (unsigned)chars, progressPercent());
  st->cache.beginWrite("pos.txt");
  st->cache.write(line, strlen(line));
  st->cache.endWrite();
  st->turnsSinceSave = 0;
}

// Size of one spine file (uncompressed), the best cheap stand-in for how much text it holds.
static uint32_t spineBytes(int index) {
  const ManifestItem* item = st->book.spineItem(index);
  const ZipEntry* entry = item ? st->book.zip().find(item->href) : nullptr;
  return entry ? max<uint32_t>(1, entry->uncompressedSize) : 1;
}

// Share of the book's text (0..1) before spine file `index` plus `within` (0..1) of that file.
static float bookFraction(int index, float within) {
  const int n = (int)st->book.spineCount();
  double before = 0, total = 0;
  for (int i = 0; i < n; i++) {
    const uint32_t b = spineBytes(i);
    if (i < index) before += b;
    total += b;
  }
  if (total <= 0) return 0;
  return (float)((before + within * spineBytes(index)) / total);
}

float progressPercent() {
  if (!isOpen() || !st->pagesOpen) return 0;
  const float pages = max<uint32_t>(1, st->pages.pageCount());
  return min(100.0f, 100.0f * bookFraction(st->spine, st->page / pages));
}

bool open(const char* epubPath, String& error, float serverPercent) {
  close();
  st = new State();
  State& s = *st;
  s.scratchBuf = (uint8_t*)malloc(kScratchArena);  // biggest first, while the heap is least fragmented
  s.bookBuf = (uint8_t*)malloc(kBookArena);  // fits the 18 KB left beside the scratch block
  for (int i = 0; i < 3; i++) s.faceBuf[i] = (uint8_t*)malloc(kFaceArena[i]);  // before the small ones fragment the heap
  s.indexBuf = (uint8_t*)malloc(kIndexArena);
  s.sheetBuf = (uint8_t*)malloc(kSheetArena);
  if (!s.bookBuf || !s.scratchBuf || !s.indexBuf || !s.sheetBuf) {
    error = "Not enough memory (" + String(ESP.getMaxAllocHeap() / 1024) + " KB free block)";
    freeBuffers();
    return false;
  }
  s.bookArena.init(s.bookBuf, kBookArena);
  s.scratch.init(s.scratchBuf, kScratchArena);
  s.indexArena.init(s.indexBuf, kIndexArena);
  s.sheetArena.init(s.sheetBuf, kSheetArena);

  if (!s.source.open(epubPath)) {
    error = "Cannot open the book file";
    freeBuffers();
    return false;
  }
  const BookStatus bs = s.book.open(s.source, s.bookArena, s.scratch);
  s.scratch.reset();
  if (bs != BookStatus::Ok) {
    error = bs == BookStatus::Encrypted ? "This book is DRM protected" : String("Not a readable EPUB: ") + bookStatusName(bs);
    s.source.close();
    freeBuffers();
    return false;
  }

  // Book stylesheet.
  CssStylesheetBuilder builder;
  builder.begin(s.sheetArena);
  for (size_t m = 0; m < s.book.manifestCount(); ++m) {
    const ManifestItem* item = s.book.manifestItem(m);
    if (strcmp(item->mediaType, "text/css") != 0) continue;
    const ZipEntry* entry = s.book.zip().find(item->href);
    if (entry) builder.addSheet(s.source, *entry, s.scratch);
    s.scratch.reset();
  }
  s.sheet = builder.finish();

  s.ttf = loadFonts(s);
  if (!s.ttf) {  // no usable font pack: built-in ASCII font
    Serial.println("font pack missing, using the built-in font");
    s.fonts = FontChain();
    s.fonts.add(&s.bitmapFont, StyleNone);
  }
  s.params.pageWidth = gfx::W;
  s.params.pageHeight = gfx::H;
  s.params.marginLeft = 20;
  s.params.marginRight = 20;
  s.params.marginTop = 24;
  s.params.marginBottom = 56;  // room for the page footer
  s.params.baseSizePx = s.ttf ? 22 : 24;
  if (s.ttf && SdMan.exists(kFontSizeFile)) {
    const int px = atoi(SdMan.readFile(kFontSizeFile).c_str());
    for (int opt : kFontSizes) if (opt == px) s.params.baseSizePx = px;
  }
  s.params.font = &s.fonts;
  s.params.stylesheet = &s.sheet;
  s.params.defaultAlign = TextAlign::Justify;
  s.params.language = s.book.metadata().language[0] ? s.book.metadata().language : "en";
  s.generation = layoutGenerationHash(s.params, s.ttf ? kFontFingerprintTtf : kFontFingerprintBitmap);
  s.cache.setDir(cacheDirFor(epubPath));

  // Resume where the book was left.
  int spine = 0;
  unsigned chars = 0;
  float localPercent = 0, seekFraction = -1;
  if (s.cache.exists("pos.txt")) {
    char line[56] = {0};
    s.cache.readAt("pos.txt", 0, line, sizeof(line) - 1);
    sscanf(line, "%d %u %f", &spine, &chars, &localPercent);
    if (spine < 0 || spine >= (int)s.book.spineCount()) { spine = 0; chars = 0; }
  }
  if (serverPercent > localPercent + 1.0f) {  // read further on another device: jump there
    const double target = min(serverPercent, 99.9f) / 100.0;
    double total = 0;
    for (int i = 0; i < (int)s.book.spineCount(); i++) total += spineBytes(i);
    double acc = 0;
    spine = 0;
    seekFraction = 0;
    for (int i = 0; i < (int)s.book.spineCount(); i++) {
      const double b = spineBytes(i);
      if (acc + b > target * total || i == (int)s.book.spineCount() - 1) {
        spine = i;
        seekFraction = (float)min(1.0, max(0.0, (target * total - acc) / b));
        break;
      }
      acc += b;
    }
    chars = 0;
    Serial.printf("resuming from server position %.1f%% (local %.1f%%)\n", serverPercent, localPercent);
  }
  busyOnLayout = "Preparing book...";
  const bool opened = openChapter(spine, error);
  busyOnLayout = nullptr;
  if (!opened) {
    close();
    return false;
  }
  s.page = chars ? s.pages.pageForChar(chars) : seekFraction > 0 ? (uint32_t)(seekFraction * s.pages.pageCount()) : 0;
  s.open = true;
  Serial.printf("fonts: %s; arena peaks: scratch %u, book %u, index %u, sheet %u\n", s.ttf ? "Noto Serif (flash)" : "built-in", (unsigned)s.scratch.highWater(), (unsigned)s.bookArena.highWater(), (unsigned)s.indexArena.highWater(), (unsigned)s.sheetArena.highWater());
  Serial.printf("book open: \"%s\", %u spine items, chapter %d has %u pages; free heap %u\n", s.book.metadata().title,
                (unsigned)s.book.spineCount(), spine, (unsigned)s.pages.pageCount(), (unsigned)ESP.getFreeHeap());
  return true;
}

void close() {
  gfx::lendFace(nullptr);
  if (st) {
    savePosition();
    st->source.close();
    st->cache.closeRead();
  }
  freeBuffers();
}

void draw(bool full) {
  if (!isOpen()) return;
  State& s = *st;
  Serial.printf("draw chapter %d page %u/%u\n", s.spine, (unsigned)s.page, (unsigned)s.pages.pageCount());
  const uint32_t drawStart = millis();
  const size_t mark = s.scratch.mark();
  Page page{};
  const BookStatus rs = s.pages.readPage(s.page, s.scratch, &page);
  gfx::clear();
  if (rs == BookStatus::Ok) {
    FrameTarget frame{display.getFrameBuffer(), (int16_t)display.getDisplayWidth(), (int16_t)display.getDisplayHeight(),
                      (int16_t)display.getDisplayWidthBytes(), FrameFormat::Mono1Dithered, FrameRotation::Portrait};
    PageRenderer::render(page, s.fonts, s.source, s.book.zip(), s.scratch, frame);
  } else {
    gfx::text(20, 100, "Cannot read page");
  }
  s.scratch.release(mark);
  char footer[48];
  snprintf(footer, sizeof(footer), "%d/%u   page %u/%u", s.spine + 1, (unsigned)s.book.spineCount(), (unsigned)s.page + 1,
           (unsigned)s.pages.pageCount());
  gfx::text(20, gfx::H - 40, footer);
  Serial.printf("rendered in %u ms\n", (unsigned)(millis() - drawStart));
  // "full" is the periodic ghosting cleanup. A half refresh does it with far less flashing than a full one (1.7 s vs 3.8 s).
  gfx::flush(full ? EInkDisplay::HALF_REFRESH : EInkDisplay::FAST_REFRESH);
}

void uiFont(bool on) {
  gfx::lendFace(on && isOpen() && st->ttf ? &st->faces[0] : nullptr);
}

int tocCount() { return isOpen() ? (int)st->book.tocCount() : 0; }
const char* tocTitle(int i) {
  const TocEntry* e = isOpen() ? st->book.tocEntry(i) : nullptr;
  return e ? e->title : "";
}
int tocDepth(int i) {
  const TocEntry* e = isOpen() ? st->book.tocEntry(i) : nullptr;
  return e ? e->depth : 0;
}

// The spine file an entry points to, -1 if none matches.
static int tocSpine(int i) {
  const TocEntry* e = st->book.tocEntry(i);
  if (!e || !e->href) return -1;
  for (int j = 0; j < (int)st->book.spineCount(); j++) {
    const ManifestItem* item = st->book.spineItem(j);
    if (item && item->href && strcmp(item->href, e->href) == 0) return j;
  }
  return -1;
}

int tocCurrent() {
  if (!isOpen()) return -1;
  int best = -1, bestSpine = -1;
  for (int i = 0; i < (int)st->book.tocCount(); i++) {
    const int sp = tocSpine(i);
    if (sp >= 0 && sp <= st->spine && sp >= bestSpine) { best = i; bestSpine = sp; }
  }
  return best;
}

bool openToc(int i) {
  if (!isOpen() || !st->pagesOpen) return false;
  State& s = *st;
  const int target = tocSpine(i);
  if (target < 0) return false;
  String error;
  busyOnLayout = "Opening chapter...";
  const int old = s.spine;
  if (!openChapter(target, error) || s.pages.pageCount() == 0) {
    busyOnLayout = nullptr;
    openChapter(old, error);
    return false;
  }
  busyOnLayout = nullptr;
  s.page = 0;
  savePosition();
  draw(true);
  return true;
}

int fontSize() { return isOpen() ? st->params.baseSizePx : 0; }

bool setFontSize(int px) {
  if (!isOpen() || !st->pagesOpen || !st->ttf || px == st->params.baseSizePx) return false;
  State& s = *st;
  const int oldSize = s.params.baseSizePx;
  const uint32_t chars = s.pages.charStart(s.page);  // stay at the same place in the text
  s.params.baseSizePx = px;
  s.generation = layoutGenerationHash(s.params, kFontFingerprintTtf);
  String error;
  busyOnLayout = "Reflowing...";
  Serial.printf("font size %d -> %d px\n", oldSize, px);
  if (!openChapter(s.spine, error)) {
    Serial.printf("font size change failed: %s\n", error.c_str());
    busyOnLayout = nullptr;
    s.params.baseSizePx = oldSize;  // keep reading at the old size
    s.generation = layoutGenerationHash(s.params, kFontFingerprintTtf);
    openChapter(s.spine, error);
    return false;
  }
  busyOnLayout = nullptr;
  s.page = chars ? s.pages.pageForChar(chars) : 0;
  SdMan.ensureDirectoryExists("/BookOrbit");
  SdMan.writeFile(kFontSizeFile, String(px));
  savePosition();
  draw(true);
  return true;
}

bool next(bool full) {
  if (!isOpen()) return false;
  State& s = *st;
  String error;
  if (s.page + 1 < s.pages.pageCount()) {
    s.page++;
  } else {
    int target = s.spine + 1;
    for (; target < (int)s.book.spineCount(); target++) {
      showBusy("Preparing chapter...");
      if (!openChapter(target, error)) { Serial.println(error); continue; }
      if (s.pages.pageCount() > 0) break;
    }
    if (target >= (int)s.book.spineCount()) {
      openChapter(s.spine, error);  // stay on the last chapter
      return false;
    }
    s.page = 0;
    full = true;
  }
  if (++s.turnsSinceSave >= 5) savePosition();
  draw(full);
  return true;
}

bool prev() {
  if (!isOpen()) return false;
  State& s = *st;
  String error;
  bool full = false;
  if (s.page > 0) {
    s.page--;
  } else {
    int target = s.spine - 1;
    for (; target >= 0; target--) {
      showBusy("Preparing chapter...");
      if (!openChapter(target, error)) { Serial.println(error); continue; }
      if (s.pages.pageCount() > 0) break;
    }
    if (target < 0) {
      openChapter(s.spine, error);
      return false;
    }
    s.page = s.pages.pageCount() - 1;
    full = true;
  }
  if (++s.turnsSinceSave >= 5) savePosition();
  draw(full);
  return true;
}

}  // namespace reader
