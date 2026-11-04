// Simulator, firmware side: the Arduino core, panel, buttons, battery, SD card and Wi-Fi the firmware sees, backed by the
// host and by the memory shared with the simulator window (sim_link.h).
#include <Arduino.h>
#include <BatteryMonitor.h>
#include <EInkDisplay.h>
#include <InputManager.h>
#include <PowerManager.h>
#include <SDCardManager.h>
#include <Update.h>
#include <WiFi.h>
#include <XteinkDetect.h>
#include <dirent.h>
#include <errno.h>
#include <poll.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <deque>
#include <mutex>
#include <random>
#include <thread>

#include "sim_link.h"

#ifdef __APPLE__
#include <mach-o/getsect.h>
#include <mach-o/ldsyms.h>
#endif

// The Noto Serif font pack (the device embeds it with board_build.embed_files) and the TLS bundle the firmware names.
#ifdef __APPLE__
#define SIM_CONST_SECTION ".section __TEXT,__const\n"
#else
#define SIM_CONST_SECTION ".section .rodata\n"
#endif
__asm__(SIM_CONST_SECTION
        ".globl _binary_build_fontpack_bin_start\n"
        ".p2align 4\n"
        "_binary_build_fontpack_bin_start:\n"
        ".incbin \"" SIM_FONTPACK_PATH "\"\n"
        ".globl _binary_build_fontpack_bin_end\n"
        "_binary_build_fontpack_bin_end:\n"
        ".byte 0\n"
        ".globl _binary_x509_crt_bundle_start\n"
        "_binary_x509_crt_bundle_start:\n"
        ".globl _binary_x509_crt_bundle_end\n"
        "_binary_x509_crt_bundle_end:\n"
        ".byte 0\n"
        ".text\n");

void setup();
void loop();

namespace sim {
namespace {
Shared* gShared = nullptr;
int gPtyFd = -1;  // the pseudo-terminal tools/drive.py talks to (owned by the host process)
const auto gStart = std::chrono::steady_clock::now();

std::mutex gInMutex;
std::deque<char> gInput;  // serial input from the terminal and the pseudo-terminal

void readInto(int fd) {
  char buf[256];
  for (;;) {
    const ssize_t n = ::read(fd, buf, sizeof(buf));
    if (n > 0) {
      std::lock_guard<std::mutex> lock(gInMutex);
      gInput.insert(gInput.end(), buf, buf + n);
    } else if (n == 0 && fd == STDIN_FILENO) {
      return;  // stdin closed
    } else {
      std::this_thread::sleep_for(std::chrono::milliseconds(50));  // pty without a reader yet
    }
  }
}

// RTC_NOINIT_ATTR variables: one section, copied to the shared memory on restart and back at start.
uint8_t* rtcSection(size_t& size) {
#ifdef __APPLE__
  unsigned long n = 0;
  uint8_t* p = getsectiondata(&_mh_execute_header, "__DATA", "__rtc_noinit", &n);
  size = p ? n : 0;
  return p;
#else
  extern uint8_t __start_rtc_noinit[], __stop_rtc_noinit[];
  size = __stop_rtc_noinit - __start_rtc_noinit;
  return __start_rtc_noinit;
#endif
}

[[noreturn]] void leave(int code) {
  size_t size = 0;
  uint8_t* rtc = rtcSection(size);
  if (rtc && size <= sizeof(gShared->rtc)) {
    memcpy(gShared->rtc, rtc, size);
    gShared->rtcSize = (int32_t)size;
  }
  fflush(stdout);
  _exit(code);
}
}  // namespace

Shared& shared() { return *gShared; }

// Called by the host's main() in the firmware process.
int runFirmware(int shmFd, int ptyFd) {
  gShared = (Shared*)mmap(nullptr, sizeof(Shared), PROT_READ | PROT_WRITE, MAP_SHARED, shmFd, 0);
  if (gShared == MAP_FAILED) {
    perror("mmap");
    return 1;
  }
  gPtyFd = ptyFd;
  size_t size = 0;
  uint8_t* rtc = rtcSection(size);
  if (gShared->resetReason != ESP_RST_POWERON && rtc && gShared->rtcSize == (int32_t)size) memcpy(rtc, gShared->rtc, size);
  std::thread(readInto, STDIN_FILENO).detach();
  if (gPtyFd >= 0) std::thread(readInto, gPtyFd).detach();
  setup();
  for (;;) loop();
}
}  // namespace sim

// ---- Arduino core

HardwareSerial Serial;
EspClass ESP;
UpdateClass Update;

unsigned long millis() {
  return (unsigned long)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - sim::gStart).count();
}
unsigned long micros() {
  return (unsigned long)std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - sim::gStart).count();
}
void delay(unsigned long ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }
void yield() { std::this_thread::yield(); }
static std::mt19937 gRandom{std::random_device{}()};
long random(long max) { return max > 0 ? (long)(gRandom() % (unsigned long)max) : 0; }
long random(long min, long max) { return max > min ? min + random(max - min) : min; }
void randomSeed(unsigned long seed) { gRandom.seed(seed); }

size_t HardwareSerial::write(const uint8_t* buf, size_t n) {
  fwrite(buf, 1, n, stdout);
  if (memchr(buf, '\n', n)) fflush(stdout);
  if (sim::gPtyFd >= 0) {
    for (size_t done = 0; done < n;) {  // a reader that is too slow loses output rather than stalling the firmware
      const ssize_t k = ::write(sim::gPtyFd, buf + done, n - done);
      if (k <= 0) break;
      done += k;
    }
  }
  return n;
}
int HardwareSerial::available() {
  std::lock_guard<std::mutex> lock(sim::gInMutex);
  return (int)sim::gInput.size();
}
int HardwareSerial::read() {
  std::lock_guard<std::mutex> lock(sim::gInMutex);
  if (sim::gInput.empty()) return -1;
  const char c = sim::gInput.front();
  sim::gInput.pop_front();
  return (uint8_t)c;
}
int HardwareSerial::peek() {
  std::lock_guard<std::mutex> lock(sim::gInMutex);
  return sim::gInput.empty() ? -1 : (uint8_t)sim::gInput.front();
}

void EspClass::restart() {
  sim::shared().resetReason = ESP_RST_SW;
  sim::leave(sim::kExitRestart);
}
// Roughly what the X4 reports in library mode; the firmware only uses these for logging and to size optional caches.
uint32_t EspClass::getFreeHeap() { return 150 * 1024; }
uint32_t EspClass::getMaxAllocHeap() { return 110 * 1024; }

esp_reset_reason_t esp_reset_reason() { return (esp_reset_reason_t)sim::shared().resetReason; }
void esp_fill_random(void* buf, size_t len) {
  for (size_t i = 0; i < len; i++) ((uint8_t*)buf)[i] = (uint8_t)gRandom();
}

BaseType_t xTaskCreate(void (*fn)(void*), const char*, uint32_t, void* arg, int, TaskHandle_t* handle) {
  std::thread(fn, arg).detach();
  if (handle) *handle = nullptr;
  return pdPASS;
}
void vTaskDelay(TickType_t ticks) { delay(ticks); }

// ---- Device

bool freeink::selectXteinkDevice() { return sim::shared().x3 != 0; }

void freeink::PowerManager::deepSleepUntilPowerButton() {
  sim::shared().resetReason = ESP_RST_DEEPSLEEP;
  sim::leave(sim::kExitSleep);
}

uint16_t BatteryMonitor::readPercentage() const { return (uint16_t)sim::shared().battery; }

void freeink::FreeInkDisplay::setDisplayX3() {
  x3_ = true;
  width_ = 792;
  height_ = 528;
}

void freeink::FreeInkDisplay::begin() {
  memset(fb_, 0xFF, sizeof(fb_));
  sim::Shared& s = sim::shared();
  s.fbWidth = width_;
  s.fbHeight = height_;
  s.fbStride = getDisplayWidthBytes();
}

// Panel refresh times (ms) when --slow is on: X4 fast/half/full, and the X3 (always blocking: 379 ms waveform + 93 ms transfer).
static unsigned long refreshMs(bool x3, EInkDisplay::RefreshMode mode) {
  if (x3) return mode == EInkDisplay::FAST_REFRESH ? 472 : 1500;
  return mode == EInkDisplay::FAST_REFRESH ? 420 : mode == EInkDisplay::HALF_REFRESH ? 1700 : 3800;
}

void freeink::FreeInkDisplay::displayBufferAsync(RefreshMode mode) {
  waitRefreshComplete();
  sim::Shared& s = sim::shared();
  memcpy(s.fb, fb_, getBufferSize());
  __atomic_add_fetch(&s.frameSeq, 1, __ATOMIC_RELEASE);
  if (s.slow) busyUntil_ = millis() + refreshMs(x3_, mode);
  if (x3_) waitRefreshComplete();  // no shadow buffer on the X3: refreshes block
}

void freeink::FreeInkDisplay::displayBuffer(RefreshMode mode) {
  displayBufferAsync(mode);
  waitRefreshComplete();
}

bool freeink::FreeInkDisplay::refreshBusy() const { return millis() < busyUntil_; }

void freeink::FreeInkDisplay::waitRefreshComplete() {
  while (refreshBusy()) delay(5);
}

void InputManager::update() {
  const uint32_t now = sim::shared().buttons;
  pressed_ = now & ~current_;
  released_ = current_ & ~now;
  if (pressed_) pressStart_ = millis();
  current_ = now;
}
bool InputManager::isPowerButtonPhysicallyPressed() const { return sim::shared().buttons & (1u << BTN_POWER); }
unsigned long InputManager::getHeldTime() const { return current_ ? millis() - pressStart_ : 0; }

// ---- Wi-Fi

WiFiClass WiFi;

bool WiFiClass::mode(wifi_mode_t m) {
  mode_ = m;
  if (m == WIFI_OFF) begun_ = false;
  return true;
}

wl_status_t WiFiClass::begin(const char* ssid, const char*) {
  if (mode_ == WIFI_OFF) mode_ = WIFI_STA;
  ssid_ = ssid;
  begun_ = true;
  connectAt_ = millis() + 700;  // about how long joining a network takes
  return WL_DISCONNECTED;
}

bool WiFiClass::disconnect(bool wifiOff, bool) {
  begun_ = false;
  if (wifiOff) mode_ = WIFI_OFF;
  return true;
}

wl_status_t WiFiClass::status() {
  if (mode_ == WIFI_OFF || !begun_) return WL_DISCONNECTED;
  if (sim::shared().wifiDown) return WL_NO_SSID_AVAIL;
  return millis() >= connectAt_ ? WL_CONNECTED : WL_DISCONNECTED;
}

// The configured network first (so it shows as in use), then a few made-up ones.
static std::vector<String> gScan;
int16_t WiFiClass::scanNetworks(bool, bool) {
  gScan.clear();
  if (sim::shared().wifiDown) return 0;
  delay(1500);
  const String cfg = SdMan.exists("/bookorbit.json") ? SdMan.readFile("/bookorbit.json") : String();
  const int k = cfg.indexOf("\"ssid\"");
  if (k >= 0) {
    const int a = cfg.indexOf('"', cfg.indexOf(':', k) + 1), b = a >= 0 ? cfg.indexOf('"', a + 1) : -1;
    if (b > a) gScan.push_back(cfg.substring(a + 1, b));
  }
  for (const char* name : {"Simulator Wi-Fi", "Neighbours", "Cafe guest"})
    if (gScan.empty() || gScan[0] != name) gScan.push_back(name);
  return (int16_t)gScan.size();
}
String WiFiClass::SSID(uint8_t i) { return i < gScan.size() ? gScan[i] : String(); }
int32_t WiFiClass::RSSI(uint8_t i) { return -45 - 12 * i; }
wifi_auth_mode_t WiFiClass::encryptionType(uint8_t i) { return i < gScan.size() && gScan[i] == "Cafe guest" ? WIFI_AUTH_OPEN : WIFI_AUTH_WPA2_PSK; }

// ---- SD card

SDCardManager SDCardManager::instance;

const char* SDCardManager::root() {
  static String r = getenv("SIM_SD") ? getenv("SIM_SD") : SIM_DIR "/sdcard";
  return r.c_str();
}

static std::string hostPath(const char* path) {
  std::string p = SDCardManager::root();
  if (!path || *path != '/') p += '/';
  if (path) p += path;
  while (p.size() > 1 && p.back() == '/') p.pop_back();
  return p;
}

bool SDCardManager::begin() {
  ::mkdir(root(), 0755);
  struct stat st;
  return ::stat(root(), &st) == 0 && S_ISDIR(st.st_mode);
}

std::vector<String> SDCardManager::listFiles(const char* path, int maxFiles) {
  std::vector<String> out;
  DIR* d = opendir(hostPath(path).c_str());
  if (!d) return out;
  while (dirent* e = readdir(d)) {
    if ((int)out.size() >= maxFiles) break;
    if (e->d_name[0] == '.' && (!e->d_name[1] || (e->d_name[1] == '.' && !e->d_name[2]))) continue;
    struct stat st;
    if (::stat((hostPath(path) + "/" + e->d_name).c_str(), &st) == 0 && S_ISDIR(st.st_mode)) continue;
    out.emplace_back(e->d_name);
  }
  closedir(d);
  return out;
}

String SDCardManager::readFile(const char* path) {
  FILE* f = fopen(hostPath(path).c_str(), "rb");
  if (!f) return String();
  std::string s;
  char buf[4096];
  for (size_t n; (n = fread(buf, 1, sizeof(buf), f)) > 0;) s.append(buf, n);
  fclose(f);
  return String(s);
}

bool SDCardManager::writeFile(const char* path, const String& content) {
  const std::string p = hostPath(path), tmp = p + ".tmp";
  FILE* f = fopen(tmp.c_str(), "wb");
  if (!f) return false;
  const bool ok = fwrite(content.c_str(), 1, content.length(), f) == content.length();
  if (fclose(f) != 0 || !ok) return false;
  return ::rename(tmp.c_str(), p.c_str()) == 0;
}

bool SDCardManager::ensureDirectoryExists(const char* path) { return mkdir(path, true); }

bool SDCardManager::mkdir(const char* path, bool pFlag) {
  const std::string p = hostPath(path);
  if (pFlag) {
    for (size_t i = strlen(root()) + 1; (i = p.find('/', i)) != std::string::npos; i++) ::mkdir(p.substr(0, i).c_str(), 0755);
  }
  struct stat st;
  return ::mkdir(p.c_str(), 0755) == 0 || (::stat(p.c_str(), &st) == 0 && S_ISDIR(st.st_mode));
}

bool SDCardManager::exists(const char* path) {
  struct stat st;
  return ::stat(hostPath(path).c_str(), &st) == 0;
}

bool SDCardManager::remove(const char* path) { return ::unlink(hostPath(path).c_str()) == 0; }
bool SDCardManager::rmdir(const char* path) { return ::rmdir(hostPath(path).c_str()) == 0; }

bool SDCardManager::rename(const char* path, const char* newPath) {
  if (exists(newPath)) return false;  // FAT does not replace on rename (replaceFile() removes the target first)
  return ::rename(hostPath(path).c_str(), hostPath(newPath).c_str()) == 0;
}

static bool removeTree(const std::string& p) {
  DIR* d = opendir(p.c_str());
  if (!d) return ::unlink(p.c_str()) == 0;
  while (dirent* e = readdir(d)) {
    if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
    removeTree(p + "/" + e->d_name);
  }
  closedir(d);
  return ::rmdir(p.c_str()) == 0;
}
bool SDCardManager::removeDir(const char* path) { return removeTree(hostPath(path)); }

FsFile SDCardManager::open(const char* path, oflag_t oflag) {
  FsFile f;
  const int fd = ::open(hostPath(path).c_str(), oflag, 0644);
  if (fd >= 0) {
    struct stat st;
    if (fstat(fd, &st) == 0 && S_ISDIR(st.st_mode) && (oflag & O_ACCMODE) != O_RDONLY) {
      ::close(fd);
      return f;
    }
    f.fd_ = std::shared_ptr<int>(new int(fd), [](int* p) {
      if (*p >= 0) ::close(*p);
      delete p;
    });
  }
  return f;
}

int FsFile::read(void* buf, size_t n) {
  if (!*this) return -1;
  const ssize_t k = ::read(*fd_, buf, n);
  return k < 0 ? -1 : (int)k;
}
int FsFile::read() {
  uint8_t c;
  return read(&c, 1) == 1 ? c : -1;
}
int FsFile::peek() {
  const uint64_t pos = curPosition();
  const int c = read();
  seekSet(pos);
  return c;
}
int FsFile::available() {
  if (!*this) return 0;
  const uint64_t left = fileSize() - curPosition();
  return left > INT32_MAX ? INT32_MAX : (int)left;
}
size_t FsFile::write(const uint8_t* buf, size_t n) {
  if (!*this) return 0;
  const ssize_t k = ::write(*fd_, buf, n);
  return k < 0 ? 0 : (size_t)k;
}
bool FsFile::seekSet(uint64_t pos) { return *this && lseek(*fd_, (off_t)pos, SEEK_SET) == (off_t)pos; }
uint64_t FsFile::curPosition() const {
  if (!*this) return 0;
  const off_t p = lseek(*fd_, 0, SEEK_CUR);
  return p < 0 ? 0 : (uint64_t)p;
}
uint64_t FsFile::fileSize() const {
  struct stat st;
  return *this && fstat(*fd_, &st) == 0 ? (uint64_t)st.st_size : 0;
}
bool FsFile::close() {
  if (!fd_) return false;
  if (*fd_ >= 0) ::close(*fd_);
  *fd_ = -1;
  fd_.reset();
  return true;
}
