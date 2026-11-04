#pragma once
// Simulator: Arduino's String on top of std::string (just what the firmware and ArduinoJson use).
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <stdio.h>

#include <algorithm>
#include <string>

class __FlashStringHelper;

class String {
 public:
  String() = default;
  String(const char* s) : s_(s ? s : "") {}
  String(const char* s, size_t n) : s_(s, n) {}
  String(const std::string& s) : s_(s) {}
  String(char c) : s_(1, c) {}
  String(int v, unsigned char base = 10) : s_(fmtInt(v, base)) {}
  String(unsigned int v, unsigned char base = 10) : s_(fmtUInt(v, base)) {}
  String(long v, unsigned char base = 10) : s_(fmtInt(v, base)) {}
  String(unsigned long v, unsigned char base = 10) : s_(fmtUInt(v, base)) {}
  String(long long v, unsigned char base = 10) : s_(fmtInt(v, base)) {}
  String(unsigned long long v, unsigned char base = 10) : s_(fmtUInt(v, base)) {}
  String(float v, unsigned char decimals = 2) : s_(fmtFloat(v, decimals)) {}
  String(double v, unsigned char decimals = 2) : s_(fmtFloat(v, decimals)) {}

  const char* c_str() const { return s_.c_str(); }
  unsigned int length() const { return (unsigned int)s_.size(); }
  bool isEmpty() const { return s_.empty(); }
  bool reserve(unsigned int n) { s_.reserve(n); return true; }
  explicit operator bool() const { return true; }

  String& operator=(const char* s) { s_ = s ? s : ""; return *this; }
  String& operator+=(const String& o) { s_ += o.s_; return *this; }
  String& operator+=(const char* s) { if (s) s_ += s; return *this; }
  String& operator+=(char c) { s_ += c; return *this; }
  template <typename T> String& operator+=(T v) { return *this += String(v); }
  bool concat(const String& o) { s_ += o.s_; return true; }
  bool concat(const char* s) { if (s) s_ += s; return true; }
  bool concat(const char* s, unsigned int n) { s_.append(s, n); return true; }
  bool concat(char c) { s_ += c; return true; }
  template <typename T> bool concat(T v) { s_ += String(v).s_; return true; }

  char operator[](unsigned int i) const { return i < s_.size() ? s_[i] : 0; }
  char& operator[](unsigned int i) { return s_[i]; }
  char charAt(unsigned int i) const { return (*this)[i]; }
  void setCharAt(unsigned int i, char c) { if (i < s_.size()) s_[i] = c; }

  bool equals(const String& o) const { return s_ == o.s_; }
  bool equals(const char* o) const { return s_ == (o ? o : ""); }
  bool equalsIgnoreCase(const String& o) const { return strcasecmp(c_str(), o.c_str()) == 0; }
  int compareTo(const String& o) const { return s_.compare(o.s_); }
  bool startsWith(const String& p) const { return s_.compare(0, p.s_.size(), p.s_) == 0; }
  bool startsWith(const String& p, unsigned int from) const { return from <= s_.size() && s_.compare(from, p.s_.size(), p.s_) == 0; }
  bool endsWith(const String& p) const { return s_.size() >= p.s_.size() && s_.compare(s_.size() - p.s_.size(), p.s_.size(), p.s_) == 0; }

  int indexOf(char c, unsigned int from = 0) const { return pos(s_.find(c, from)); }
  int indexOf(const String& t, unsigned int from = 0) const { return pos(s_.find(t.s_, from)); }
  int indexOf(const char* t, unsigned int from = 0) const { return pos(s_.find(t, from)); }
  int lastIndexOf(char c) const { return pos(s_.rfind(c)); }
  int lastIndexOf(char c, unsigned int from) const { return pos(s_.rfind(c, from)); }
  int lastIndexOf(const String& t) const { return pos(s_.rfind(t.s_)); }
  int lastIndexOf(const String& t, unsigned int from) const { return pos(s_.rfind(t.s_, from)); }
  String substring(unsigned int from) const { return from < s_.size() ? String(s_.substr(from)) : String(); }
  String substring(unsigned int from, unsigned int to) const {
    if (from > to) std::swap(from, to);
    if (from >= s_.size()) return String();
    return String(s_.substr(from, to - from));
  }

  void trim() {
    size_t a = 0, b = s_.size();
    while (a < b && isspace((unsigned char)s_[a])) a++;
    while (b > a && isspace((unsigned char)s_[b - 1])) b--;
    s_ = s_.substr(a, b - a);
  }
  void toLowerCase() { for (auto& c : s_) c = (char)tolower((unsigned char)c); }
  void toUpperCase() { for (auto& c : s_) c = (char)toupper((unsigned char)c); }
  void replace(char a, char b) { for (auto& c : s_) if (c == a) c = b; }
  void replace(const String& a, const String& b) {
    if (a.s_.empty()) return;
    for (size_t p = 0; (p = s_.find(a.s_, p)) != std::string::npos; p += b.s_.size()) s_.replace(p, a.s_.size(), b.s_);
  }
  void remove(unsigned int from) { if (from < s_.size()) s_.erase(from); }
  void remove(unsigned int from, unsigned int n) { if (from < s_.size()) s_.erase(from, n); }
  long toInt() const { return atol(c_str()); }
  float toFloat() const { return (float)atof(c_str()); }
  double toDouble() const { return atof(c_str()); }
  void getBytes(unsigned char* buf, unsigned int n) const { toCharArray((char*)buf, n); }
  void toCharArray(char* buf, unsigned int n) const {
    if (!n) return;
    const size_t k = std::min<size_t>(n - 1, s_.size());
    memcpy(buf, s_.data(), k);
    buf[k] = 0;
  }

  friend String operator+(const String& a, const String& b) { return String(a.s_ + b.s_); }
  friend String operator+(const String& a, const char* b) { return String(a.s_ + (b ? b : "")); }
  friend String operator+(const char* a, const String& b) { return String(std::string(a ? a : "") + b.s_); }
  friend String operator+(const String& a, char b) { return String(a.s_ + b); }
  template <typename T> friend String operator+(const String& a, T b) { return a + String(b); }
  friend bool operator==(const String& a, const String& b) { return a.s_ == b.s_; }
  friend bool operator==(const String& a, const char* b) { return a.s_ == (b ? b : ""); }
  friend bool operator!=(const String& a, const String& b) { return a.s_ != b.s_; }
  friend bool operator!=(const String& a, const char* b) { return !(a == b); }
  friend bool operator<(const String& a, const String& b) { return a.s_ < b.s_; }

 private:
  static int pos(size_t p) { return p == std::string::npos ? -1 : (int)p; }
  static std::string fmtUInt(unsigned long long v, int base) {
    if (base == 10) return std::to_string(v);
    std::string r;
    do { r.insert(r.begin(), "0123456789abcdefghijklmnopqrstuvwxyz"[v % base]); v /= base; } while (v);
    return r;
  }
  static std::string fmtInt(long long v, int base) {
    if (base == 10) return std::to_string(v);
    return fmtUInt((unsigned long long)v, base);
  }
  static std::string fmtFloat(double v, int decimals) {
    char b[64];
    snprintf(b, sizeof(b), "%.*f", decimals, v);
    return b;
  }
  std::string s_;
};
