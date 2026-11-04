#pragma once
// Simulator: Wi-Fi is the host's network. A scan lists the configured network plus a few made-up ones; W in the simulator
// window takes the network away (to test the offline paths).
#include "Arduino.h"

typedef enum { WIFI_OFF = 0, WIFI_STA = 1, WIFI_AP = 2, WIFI_AP_STA = 3 } wifi_mode_t;
typedef enum {
  WL_IDLE_STATUS = 0,
  WL_NO_SSID_AVAIL = 1,
  WL_SCAN_COMPLETED = 2,
  WL_CONNECTED = 3,
  WL_CONNECT_FAILED = 4,
  WL_CONNECTION_LOST = 5,
  WL_DISCONNECTED = 6,
} wl_status_t;
typedef enum { WIFI_AUTH_OPEN = 0, WIFI_AUTH_WPA2_PSK = 3 } wifi_auth_mode_t;
typedef enum { WIFI_PS_NONE, WIFI_PS_MIN_MODEM, WIFI_PS_MAX_MODEM } wifi_ps_type_t;
#define WIFI_SCAN_RUNNING (-1)
#define WIFI_SCAN_FAILED (-2)

class IPAddress {
 public:
  IPAddress(uint8_t a = 0, uint8_t b = 0, uint8_t c = 0, uint8_t d = 0) : b_{a, b, c, d} {}
  String toString() const { return String(b_[0]) + "." + String(b_[1]) + "." + String(b_[2]) + "." + String(b_[3]); }
  uint8_t operator[](int i) const { return b_[i]; }

 private:
  uint8_t b_[4];
};

class WiFiClass {
 public:
  bool mode(wifi_mode_t m);
  wifi_mode_t getMode() const { return mode_; }
  wl_status_t begin(const char* ssid, const char* pass = nullptr);
  bool disconnect(bool wifiOff = false, bool eraseAp = false);
  wl_status_t status();
  bool setSleep(bool) { return true; }
  bool setSleep(wifi_ps_type_t) { return true; }
  IPAddress localIP() const { return IPAddress(127, 0, 0, 1); }
  String SSID() const { return ssid_; }
  int16_t scanNetworks(bool async = false, bool showHidden = false);
  String SSID(uint8_t i);
  int32_t RSSI(uint8_t i);
  wifi_auth_mode_t encryptionType(uint8_t i);
  void scanDelete() {}

 private:
  wifi_mode_t mode_ = WIFI_OFF;
  bool begun_ = false;
  unsigned long connectAt_ = 0;
  String ssid_;
};
extern WiFiClass WiFi;
