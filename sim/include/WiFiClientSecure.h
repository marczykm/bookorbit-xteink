#pragma once
// Simulator: a "connection" holds the whole response body that HTTPClient fetched with curl; the firmware reads it as a
// stream. TLS is curl's, with the host's certificates.
#include <string>

#include "Arduino.h"

class WiFiClient : public Stream {
 public:
  int available() override { return (int)(body_.size() - pos_); }
  int read() override { return pos_ < body_.size() ? (uint8_t)body_[pos_++] : -1; }
  int read(uint8_t* buf, size_t n) {
    n = std::min(n, body_.size() - pos_);
    memcpy(buf, body_.data() + pos_, n);
    pos_ += n;
    return (int)n;
  }
  int peek() override { return pos_ < body_.size() ? (uint8_t)body_[pos_] : -1; }
  size_t write(uint8_t) override { return 1; }
  using Print::write;
  uint8_t connected() { return pos_ < body_.size(); }
  void stop() { body_.clear(); pos_ = 0; }
  void setTimeout(unsigned long) {}

 private:
  friend class HTTPClient;
  std::string body_;
  size_t pos_ = 0;
};

class WiFiClientSecure : public WiFiClient {
 public:
  void setInsecure() { insecure_ = true; }
  void setCACertBundle(const uint8_t*, size_t = 0) {}

 private:
  friend class HTTPClient;
  bool insecure_ = false;
};
