#include "cover.h"

#include <SDCardManager.h>
#include <pngle.h>
#include <tjpgd.h>

namespace cover {
namespace {
String binPath(long id) { return "/BookOrbit/.covers/" + String(id) + ".bin"; }
String nonePath(long id) { return "/BookOrbit/.covers/" + String(id) + ".none"; }
String bigPath(long id) { return "/BookOrbit/.covers/" + String(id) + ".lg"; }
String bigTmpPath(long id) { return "/BookOrbit/.covers/" + String(id) + ".lg.tmp"; }

// Box-filter accumulation of source pixels into the W x H target, keeping the cover's aspect ratio (centred, white around).
struct Scaler {
  uint16_t* sums = nullptr;  // gray/4 summed per target pixel
  uint16_t colCnt[W] = {}, rowCnt[H] = {};  // source columns / rows landing on each target column / row
  int srcW = 0, srcH = 0, dstW = 0, dstH = 0, offX = 0, offY = 0;

  // The sum buffer has a fixed size, so it can be taken before a decoder claims its large blocks.
  bool alloc() {
    sums = (uint16_t*)calloc(W * H, sizeof(uint16_t));
    return sums != nullptr;
  }
  // Maps a source of sw x sh pixels onto the target, keeping the aspect ratio (centred, white around).
  bool init(int sw, int sh) {
    srcW = sw;
    srcH = sh;
    if (sw <= 0 || sh <= 0 || !sums) return false;
    if ((long)sw * H > (long)sh * W) { dstW = W; dstH = max(1, sh * W / sw); }
    else { dstH = H; dstW = max(1, sw * H / sh); }
    offX = (W - dstW) / 2;
    offY = (H - dstH) / 2;
    memset(colCnt, 0, sizeof(colCnt));
    memset(rowCnt, 0, sizeof(rowCnt));
    for (int x = 0; x < sw; x++) colCnt[offX + x * dstW / sw]++;
    for (int y = 0; y < sh; y++) rowCnt[offY + y * dstH / sh]++;
    return true;
  }
  void add(int x, int y, uint8_t gray) {
    if (x < 0 || y < 0 || x >= srcW || y >= srcH) return;
    sums[(offY + y * dstH / srcH) * W + offX + x * dstW / srcW] += gray >> 2;
  }
  void free_() {
    free(sums);
    sums = nullptr;
  }
  // Ordered 4x4 Bayer dither to 1 bit; pixels nothing landed on stay white.
  void toBits(uint8_t* bits) const {
    static const uint8_t bayer[4][4] = {{0, 8, 2, 10}, {12, 4, 14, 6}, {3, 11, 1, 9}, {15, 7, 13, 5}};
    memset(bits, 0, kBytes);
    for (int y = 0; y < H; y++)
      for (int x = 0; x < W; x++) {
        const uint32_t count = (uint32_t)colCnt[x] * rowCnt[y];
        const int gray = count ? (int)(sums[y * W + x] / count) * 4 : 255;
        const bool black = gray < (bayer[y & 3][x & 3] * 16 + 8);
        if (black) bits[y * (W / 8) + x / 8] |= 0x80 >> (x & 7);
      }
  }
};

// The large cover is built in bands so it never needs the whole image in RAM: decoders deliver the picture top to bottom, so a
// target row is final a few rows after the decoder has moved past it, and is then error-diffused (Floyd-Steinberg) into the 1-bit result.
struct Band {
  static constexpr int LW = BigW, LH = BigH, kRows = 40, kSafe = 18;  // a decoder band reaches back at most ~17 target rows
  uint16_t* ring = nullptr;                 // kRows x LW sums of gray/4
  FsFile* out = nullptr;                    // the finished 1-bit rows are written here as they are completed
  uint8_t rowBits[LW / 8] = {};
  int16_t* err = nullptr;                   // two error rows (LW + 2 each), in 1/16 grey levels
  uint16_t* colCnt = nullptr;               // LW source columns per target column
  uint16_t* rowCnt = nullptr;               // LH source rows per target row
  int srcW = 0, srcH = 0, dstW = 0, dstH = 0, offX = 0, offY = 0, nextFinal = 0, maxTy = -1;

  bool alloc() {
    const size_t sz = (size_t)kRows * LW * 2 + 2 * (LW + 2) * 2 + LW * 2 + LH * 2;
    uint8_t* mem = (uint8_t*)calloc(1, sz);
    if (!mem) return false;
    ring = (uint16_t*)mem;
    err = (int16_t*)(mem + (size_t)kRows * LW * 2);
    colCnt = (uint16_t*)(err + 2 * (LW + 2));
    rowCnt = colCnt + LW;
    return true;
  }
  void free_() {
    free(ring);  // one block holds everything
    ring = nullptr;
  }
  bool init(int sw, int sh) {
    srcW = sw;
    srcH = sh;
    if (sw <= 0 || sh <= 0 || !ring) return false;
    if ((long)sw * LH > (long)sh * LW) { dstW = LW; dstH = max(1, sh * LW / sw); }
    else { dstH = LH; dstW = max(1, sw * LH / sh); }
    offX = (LW - dstW) / 2;
    offY = (LH - dstH) / 2;
    for (int x = 0; x < sw; x++) colCnt[offX + x * dstW / sw]++;
    for (int y = 0; y < sh; y++) rowCnt[offY + y * dstH / sh]++;
    return true;
  }
  void finalizeRow(int ty) {
    uint16_t* row = ring + (ty % kRows) * LW;
    int16_t* cur = err + (ty & 1) * (LW + 2) + 1;     // errors carried into this row
    int16_t* next = err + ((ty + 1) & 1) * (LW + 2) + 1;  // ...and into the next one
    const bool rtl = ty & 1;  // serpentine: odd rows run right to left
    memset(rowBits, 0, sizeof(rowBits));
    for (int i = 0; i < LW; i++) {
      const int x = rtl ? LW - 1 - i : i, dir = rtl ? -1 : 1;
      const uint32_t count = (uint32_t)colCnt[x] * rowCnt[ty];
      int v = count ? (int)(row[x] * 4 / count) : 255;
      v += cur[x] / 16;
      v = constrain(v, 0, 255);
      const bool white = v >= 128;
      const int e = v - (white ? 255 : 0);
      if (!white) rowBits[x / 8] |= 0x80 >> (x & 7);
      if (x + dir >= 0 && x + dir < LW) cur[x + dir] += e * 7;
      if (x - dir >= 0 && x - dir < LW) next[x - dir] += e * 3;
      next[x] += e * 5;
      if (x + dir >= 0 && x + dir < LW) next[x + dir] += e * 1;
    }
    if (out) out->write(rowBits, sizeof(rowBits));
    memset(cur - 1, 0, (LW + 2) * sizeof(int16_t));  // this row's errors are spent
    memset(row, 0, LW * sizeof(uint16_t));
  }
  void add(int x, int y, uint8_t gray) {
    if (x < 0 || y < 0 || x >= srcW || y >= srcH || !ring) return;
    const int ty = offY + y * dstH / srcH, tx = offX + x * dstW / srcW;
    if (ty > maxTy) {
      while (nextFinal <= ty - kSafe && nextFinal < LH) finalizeRow(nextFinal++);
      maxTy = ty;
    }
    if (ty < nextFinal) return;  // too late for this row (cannot happen with a normal decoder)
    ring[(ty % kRows) * LW + tx] += gray >> 2;
  }
  void finish() {
    while (nextFinal < LH) finalizeRow(nextFinal++);
  }
};

// Both cover sizes are built from one decode.
struct Sink {
  Scaler small;
  Band big;
  bool alloc() { return big.alloc() && small.alloc(); }  // the larger block first, before a decoder takes its blocks
  bool init(int sw, int sh) { return small.init(sw, sh) && big.init(sw, sh); }
  void add(int x, int y, uint8_t g) {
    small.add(x, y, g);
    big.add(x, y, g);
  }
  void free_() {
    small.free_();
    big.free_();
  }
};

struct PngCtx {
  Sink* scaler;
  bool bad = false;
};

void pngInit(pngle_t* p, uint32_t w, uint32_t h) {
  PngCtx* c = static_cast<PngCtx*>(pngle_get_user_data(p));
  if (!c->scaler->init(w, h)) c->bad = true;
}

void pngDraw(pngle_t* p, uint32_t x, uint32_t y, uint32_t, uint32_t, const uint8_t rgba[4]) {
  PngCtx* c = static_cast<PngCtx*>(pngle_get_user_data(p));
  if (c->bad) return;
  const int gray = (rgba[0] * 30 + rgba[1] * 59 + rgba[2] * 11) / 100;
  const int a = rgba[3];
  c->scaler->add(x, y, (gray * a + 255 * (255 - a)) / 255);  // transparent areas read as white
}

bool decodePng(FsFile& f, Sink& s) {
  PngCtx ctx{&s};
  if (!s.alloc()) return false;  // before the decoder takes its big blocks
  pngle_t* p = pngle_new();
  if (!p) return false;
  pngle_set_user_data(p, &ctx);
  pngle_set_init_callback(p, pngInit);
  pngle_set_draw_callback(p, pngDraw);
  uint8_t buf[512];
  bool ok = true;
  for (;;) {
    const int n = f.read(buf, sizeof(buf));
    if (n < 0) { ok = false; break; }
    if (n == 0) break;
    if (pngle_feed(p, buf, n) < 0 || ctx.bad) { ok = false; break; }
  }
  pngle_destroy(p);
  if (ok) s.big.finish();
  return ok && s.small.sums;
}

struct JpgCtx {
  FsFile* file;
  Sink* scaler;
  bool bad = false;
};

size_t jpgIn(JDEC* jd, uint8_t* buf, size_t len) {
  JpgCtx* c = static_cast<JpgCtx*>(jd->device);
  if (buf) {
    const int n = c->file->read(buf, len);
    return n > 0 ? n : 0;
  }
  const uint64_t pos = c->file->curPosition();
  return c->file->seekSet(pos + len) ? len : 0;
}

int jpgOut(JDEC* jd, void* bitmap, JRECT* r) {
  JpgCtx* c = static_cast<JpgCtx*>(jd->device);
  const uint8_t* px = static_cast<const uint8_t*>(bitmap);
  for (int y = r->top; y <= r->bottom; y++)
    for (int x = r->left; x <= r->right; x++) c->scaler->add(x, y, *px++);
  return 1;
}

bool decodeJpeg(FsFile& f, Sink& s) {
  static constexpr size_t kPool = 8 * 1024;
  if (!s.alloc()) return false;
  void* pool = malloc(kPool);
  if (!pool) return false;
  JpgCtx ctx{&f, &s};
  JDEC jd;
  bool ok = false;
  if (jd_prepare(&jd, jpgIn, pool, kPool, &ctx) == JDR_OK) {  // progressive files fail here and get no cover
    uint8_t scale = 0;  // shrink by 2/4/8 while the result is still at least as big as the large target
    while (scale < 3 && (jd.width >> (scale + 1)) >= BigW && (jd.height >> (scale + 1)) >= BigH) scale++;
    if (s.init(jd.width >> scale, jd.height >> scale)) ok = jd_decomp(&jd, jpgOut, scale) == JDR_OK;
    if (ok) s.big.finish();
  }
  free(pool);
  return ok;
}
}  // namespace

bool cached(long id) { return (SdMan.exists(binPath(id).c_str()) && SdMan.exists(bigPath(id).c_str())) || SdMan.exists(nonePath(id).c_str()); }

bool load(long id, uint8_t* bits) {
  FsFile f = SdMan.open(binPath(id).c_str(), O_RDONLY);
  if (!f) return false;
  const bool ok = f.read(bits, kBytes) == kBytes;
  f.close();
  return ok;
}

bool loadBig(long id, uint8_t* bits) {
  FsFile f = SdMan.open(bigPath(id).c_str(), O_RDONLY);
  if (!f) return false;
  const bool ok = f.read(bits, kBigBytes) == kBigBytes;
  f.close();
  return ok;
}

bool loadBigRows(long id, int firstRow, int rows, uint8_t* bits) {
  FsFile f = SdMan.open(bigPath(id).c_str(), O_RDONLY);
  if (!f) return false;
  const int stride = BigW / 8;
  bool ok = f.seekSet((uint32_t)firstRow * stride) && f.read(bits, (size_t)rows * stride) == rows * stride;
  f.close();
  return ok;
}

bool hasBig(long id) { return SdMan.exists(bigPath(id).c_str()); }

String stagingPath() { return "/BookOrbit/.covers/staging.img"; }

void markNone(long id) {
  SdMan.ensureDirectoryExists("/BookOrbit/.covers");
  SdMan.writeFile(nonePath(id).c_str(), "1");
}

bool convertStaged(long id, String& error) {
  FsFile f = SdMan.open(stagingPath().c_str(), O_RDONLY);
  if (!f) { error = "no staged cover"; return false; }
  uint8_t magic[4] = {0};
  f.read(magic, 4);
  f.seekSet(0);
  Sink s;
  bool ok = false;
  SdMan.ensureDirectoryExists("/BookOrbit/.covers");
  FsFile bigTmp = SdMan.open(bigTmpPath(id).c_str(), O_WRITE | O_CREAT | O_TRUNC);  // the large image is streamed here row by row
  s.big.out = bigTmp ? &bigTmp : nullptr;
  Serial.printf("cover %ld: decoding, free heap %u, largest block %u\n", id, (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMaxAllocHeap());
  if (magic[0] == 0x89 && magic[1] == 'P') ok = decodePng(f, s);
  else if (magic[0] == 0xFF && magic[1] == 0xD8) ok = decodeJpeg(f, s);
  else error = "unsupported cover format";
  f.close();
  SdMan.remove(stagingPath().c_str());
  if (ok && s.small.sums && s.big.ring) {
    uint8_t bits[kBytes];
    s.small.toBits(bits);
    SdMan.ensureDirectoryExists("/BookOrbit/.covers");
    FsFile out = SdMan.open(binPath(id).c_str(), O_WRITE | O_CREAT | O_TRUNC);
    if (out) { out.write(bits, kBytes); out.close(); } else { ok = false; error = "cannot write cover"; }
    if (!bigTmp) { ok = false; error = "cannot write cover"; }
  } else if (error.isEmpty()) {
    error = "cover decode failed";
  }
  s.free_();
  if (bigTmp) {
    bigTmp.close();
    if (ok) ok = SdMan.replaceFile(bigTmpPath(id).c_str(), bigPath(id).c_str());
    else SdMan.remove(bigTmpPath(id).c_str());
  }
  if (!ok) markNone(id);
  return ok;
}
}  // namespace cover
