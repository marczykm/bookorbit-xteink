#pragma once
// Simulator: HTTPClient on libcurl. GET()/POST() run the whole request; the body is then read from the client passed to
// begin() (getStreamPtr()) or with getString().
#include <string>
#include <utility>
#include <vector>

#include "WiFiClientSecure.h"

typedef enum {
  HTTPC_DISABLE_FOLLOW_REDIRECTS,
  HTTPC_STRICT_FOLLOW_REDIRECTS,
  HTTPC_FORCE_FOLLOW_REDIRECTS,
} followRedirects_t;

#define HTTPC_ERROR_CONNECTION_REFUSED (-1)
#define HTTPC_ERROR_SEND_HEADER_FAILED (-2)
#define HTTPC_ERROR_NOT_CONNECTED (-4)
#define HTTPC_ERROR_CONNECTION_LOST (-5)
#define HTTPC_ERROR_READ_TIMEOUT (-11)

class HTTPClient {
 public:
  bool begin(WiFiClient& client, const String& url);
  void end();
  void addHeader(const String& name, const String& value) { headers_.emplace_back(name, value); }
  void setUserAgent(const String& ua) { userAgent_ = ua; }
  void setConnectTimeout(int32_t ms) { connectTimeout_ = ms; }
  void setTimeout(uint16_t ms) { timeout_ = ms; }
  void setFollowRedirects(followRedirects_t f) { follow_ = f; }
  void collectHeaders(const char* names[], size_t n);
  int GET();
  int POST(const String& payload) { return request("POST", payload.c_str(), payload.length()); }
  int POST(const uint8_t* payload, size_t n) { return request("POST", (const char*)payload, n); }
  int PUT(const String& payload) { return request("PUT", payload.c_str(), payload.length()); }
  int sendRequest(const char* method, const String& payload) { return request(method, payload.c_str(), payload.length()); }
  String getString();
  int getSize() const { return size_; }
  WiFiClient* getStreamPtr() { return client_; }
  WiFiClient& getStream() { return *client_; }
  String header(const char* name) const;
  String getLocation() const { return location_; }
  static String errorToString(int error);

 private:
  int request(const char* method, const char* body, size_t len);
  WiFiClient* client_ = nullptr;
  String url_, userAgent_ = "ESP32HTTPClient", location_;
  std::vector<std::pair<String, String>> headers_;
  std::vector<std::pair<String, String>> wanted_;  // collectHeaders() names and their values
  int32_t connectTimeout_ = 5000;
  uint16_t timeout_ = 5000;
  followRedirects_t follow_ = HTTPC_DISABLE_FOLLOW_REDIRECTS;
  int size_ = -1;
};
