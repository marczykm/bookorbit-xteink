#pragma once
// Simulator: the slice of the Arduino-ESP32 core the firmware uses, on the host.
#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <algorithm>
#include <cmath>

#include "WString.h"

using std::max;
using std::min;
using std::abs;

#ifndef constrain
#define constrain(x, lo, hi) ((x) < (lo) ? (lo) : ((x) > (hi) ? (hi) : (x)))
#endif
#define PI 3.1415926535897932384626433832795
#define TWO_PI 6.283185307179586476925286766559
#define DEG_TO_RAD 0.017453292519943295769236907684886
#define PROGMEM
#define F(s) (s)

unsigned long millis();
unsigned long micros();
void delay(unsigned long ms);
void yield();
long random(long max);
long random(long min, long max);
void randomSeed(unsigned long);

// Kept across ESP.restart() (the simulator saves this section and restores it in the next run), like the chip's RTC RAM.
#define RTC_NOINIT_ATTR __attribute__((section("__DATA,__rtc_noinit")))

class Print {
 public:
  virtual ~Print() = default;
  virtual size_t write(uint8_t c) = 0;
  virtual size_t write(const uint8_t* buf, size_t n) {
    size_t k = 0;
    while (k < n && write(buf[k])) k++;
    return k;
  }
  size_t write(const char* s) { return s ? write((const uint8_t*)s, strlen(s)) : 0; }
  size_t print(const String& s) { return write((const uint8_t*)s.c_str(), s.length()); }
  size_t print(const char* s) { return write(s); }
  size_t print(char c) { return write((uint8_t)c); }
  template <typename T> size_t print(T v) { return print(String(v)); }
  size_t println() { return write("\n"); }
  template <typename T> size_t println(T v) { return print(v) + println(); }
  size_t printf(const char* fmt, ...) __attribute__((format(printf, 2, 3))) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n < 0) return 0;
    if ((size_t)n >= sizeof(buf)) {
      std::string big(n + 1, '\0');
      va_start(ap, fmt);
      vsnprintf(&big[0], big.size(), fmt, ap);
      va_end(ap);
      return write((const uint8_t*)big.data(), n);
    }
    return write((const uint8_t*)buf, n);
  }
  virtual void flush() {}
};

class Printable {
 public:
  virtual ~Printable() = default;
  virtual size_t printTo(Print& p) const = 0;
};

class Stream : public Print {
 public:
  virtual int available() = 0;
  virtual int read() = 0;
  virtual int peek() = 0;
  void setTimeout(unsigned long ms) { timeout_ = ms; }
  size_t readBytes(char* buf, size_t n) { return readBytes((uint8_t*)buf, n); }
  size_t readBytes(uint8_t* buf, size_t n) {
    size_t k = 0;
    const unsigned long start = millis();
    while (k < n && millis() - start < timeout_) {
      const int c = read();
      if (c < 0) {
        if (!available()) delay(1);
        continue;
      }
      buf[k++] = (uint8_t)c;
    }
    return k;
  }
  String readString() {
    String s;
    for (int c; (c = read()) >= 0;) s += (char)c;
    return s;
  }

 protected:
  unsigned long timeout_ = 1000;
};

// The serial port: output goes to the terminal (and the simulator's pseudo-terminal, for tools/drive.py), input comes from both.
class HardwareSerial : public Stream {
 public:
  void begin(unsigned long) {}
  size_t write(uint8_t c) override { return write(&c, 1); }
  size_t write(const uint8_t* buf, size_t n) override;
  int available() override;
  int read() override;
  int peek() override;
  using Print::write;
  explicit operator bool() const { return true; }
};
extern HardwareSerial Serial;

class EspClass {
 public:
  [[noreturn]] void restart();
  uint32_t getFreeHeap();
  uint32_t getMaxAllocHeap();
  uint32_t getMinFreeHeap() { return getFreeHeap(); }
  uint32_t getHeapSize() { return 320 * 1024; }
  uint32_t getPsramSize() { return 0; }
};
extern EspClass ESP;

typedef enum {
  ESP_RST_UNKNOWN = 0,
  ESP_RST_POWERON = 1,
  ESP_RST_EXT = 2,
  ESP_RST_SW = 3,
  ESP_RST_PANIC = 4,
  ESP_RST_DEEPSLEEP = 8,
} esp_reset_reason_t;
esp_reset_reason_t esp_reset_reason();
void esp_fill_random(void* buf, size_t len);

// FreeRTOS: tasks are threads.
typedef void* TaskHandle_t;
typedef uint32_t TickType_t;
typedef int BaseType_t;
#define pdPASS 1
#define pdMS_TO_TICKS(ms) ((TickType_t)(ms))
#define portMAX_DELAY 0xffffffffu
BaseType_t xTaskCreate(void (*fn)(void*), const char* name, uint32_t stack, void* arg, int prio, TaskHandle_t* handle);
void vTaskDelay(TickType_t ticks);

// configTime(): the host clock is already right.
inline void configTime(long, int, const char*, const char* = nullptr, const char* = nullptr) {}
