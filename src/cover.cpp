#include "cover.h"

#include <SDCardManager.h>
#include <pngle.h>
#include <tjpgd.h>

namespace cover {
namespace {
String binPath(long id) { return "/BookOrbit/.covers/" + String(id) + ".bin"; }
String nonePath(long id) { return "/BookOrbit/.covers/" + String(id) + ".none"; }

// Box-filter accumulation of source pixels into the W x H target, keeping the cover's aspect ratio (centred, white around).
struct Scaler {
  uint32_t* sums = nullptr;
  uint16_t* counts = nullptr;
  int srcW = 0, srcH = 0, dstW = 0, dstH = 0, offX = 0, offY = 0;

  bool init(int sw, int sh) {
    srcW = sw;
    srcH = sh;
    if (sw <= 0 || sh <= 0) return false;
    if ((long)sw * H > (long)sh * W) { dstW = W; dstH = max(1, sh * W / sw); }
    else { dstH = H; dstW = max(1, sw * H / sh); }
    offX = (W - dstW) / 2;
    offY = (H - dstH) / 2;
    sums = (uint32_t*)calloc(W * H, sizeof(uint32_t));
    counts = (uint16_t*)calloc(W * H, sizeof(uint16_t));
    return sums && counts;
  }
  void add(int x, int y, uint8_t gray) {
    if (x < 0 || y < 0 || x >= srcW || y >= srcH) return;
    const int i = (offY + y * dstH / srcH) * W + offX + x * dstW / srcW;
    sums[i] += gray;
    counts[i]++;
  }
  void free_() {
    free(sums);
    free(counts);
    sums = nullptr;
    counts = nullptr;
  }
  // Ordered 4x4 Bayer dither to 1 bit; pixels nothing landed on stay white.
  void toBits(uint8_t* bits) const {
    static const uint8_t bayer[4][4] = {{0, 8, 2, 10}, {12, 4, 14, 6}, {3, 11, 1, 9}, {15, 7, 13, 5}};
    memset(bits, 0, kBytes);
    for (int y = 0; y < H; y++)
      for (int x = 0; x < W; x++) {
        const int i = y * W + x;
        const int gray = counts[i] ? sums[i] / counts[i] : 255;
        const bool black = gray < (bayer[y & 3][x & 3] * 16 + 8);
        if (black) bits[y * (W / 8) + x / 8] |= 0x80 >> (x & 7);
      }
  }
};

struct PngCtx {
  Scaler* scaler;
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

bool decodePng(FsFile& f, Scaler& s) {
  PngCtx ctx{&s};
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
  return ok && s.sums;
}

struct JpgCtx {
  FsFile* file;
  Scaler* scaler;
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

bool decodeJpeg(FsFile& f, Scaler& s) {
  static constexpr size_t kPool = 8 * 1024;
  void* pool = malloc(kPool);
  if (!pool) return false;
  JpgCtx ctx{&f, &s};
  JDEC jd;
  bool ok = false;
  if (jd_prepare(&jd, jpgIn, pool, kPool, &ctx) == JDR_OK) {  // progressive files fail here and get no cover
    uint8_t scale = 0;  // shrink by 2/4/8 while the result is still at least as big as the target
    while (scale < 3 && (jd.width >> (scale + 1)) >= W && (jd.height >> (scale + 1)) >= H) scale++;
    if (s.init(jd.width >> scale, jd.height >> scale)) ok = jd_decomp(&jd, jpgOut, scale) == JDR_OK;
  }
  free(pool);
  return ok;
}
}  // namespace

bool cached(long id) { return SdMan.exists(binPath(id).c_str()) || SdMan.exists(nonePath(id).c_str()); }

bool load(long id, uint8_t* bits) {
  FsFile f = SdMan.open(binPath(id).c_str(), O_RDONLY);
  if (!f) return false;
  const bool ok = f.read(bits, kBytes) == kBytes;
  f.close();
  return ok;
}

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
  Scaler s;
  bool ok = false;
  Serial.printf("cover %ld: decoding, free heap %u, largest block %u\n", id, (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMaxAllocHeap());
  if (magic[0] == 0x89 && magic[1] == 'P') ok = decodePng(f, s);
  else if (magic[0] == 0xFF && magic[1] == 0xD8) ok = decodeJpeg(f, s);
  else error = "unsupported cover format";
  f.close();
  SdMan.remove(stagingPath().c_str());
  if (ok && s.sums) {
    uint8_t bits[kBytes];
    s.toBits(bits);
    SdMan.ensureDirectoryExists("/BookOrbit/.covers");
    FsFile out = SdMan.open(binPath(id).c_str(), O_WRITE | O_CREAT | O_TRUNC);
    if (out) { out.write(bits, kBytes); out.close(); } else { ok = false; error = "cannot write cover"; }
  } else if (error.isEmpty()) {
    error = "cover decode failed";
  }
  s.free_();
  if (!ok) markNone(id);
  return ok;
}
}  // namespace cover
