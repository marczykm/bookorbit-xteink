// Simulator: HTTPClient on libcurl (the host's network and TLS).
#include <HTTPClient.h>
#include <WiFi.h>
#include <curl/curl.h>

namespace {
size_t onBody(char* data, size_t size, size_t n, void* user) {
  static_cast<std::string*>(user)->append(data, size * n);
  return size * n;
}

struct HeaderSink {
  std::vector<std::pair<String, String>>* wanted;
  String* location;
};

size_t onHeader(char* data, size_t size, size_t n, void* user) {
  auto* sink = static_cast<HeaderSink*>(user);
  String line(data, size * n);
  line.trim();
  const int colon = line.indexOf(':');
  if (colon > 0) {
    const String name = line.substring(0, colon);
    String value = line.substring(colon + 1);
    value.trim();
    if (name.equalsIgnoreCase("Location")) *sink->location = value;
    if (name.equalsIgnoreCase("Transfer-Encoding")) return size * n;  // curl hands over the body already de-chunked
    for (auto& w : *sink->wanted)
      if (name.equalsIgnoreCase(w.first)) w.second = value;
  }
  return size * n;
}
}  // namespace

bool HTTPClient::begin(WiFiClient& client, const String& url) {
  if (!url.startsWith("http://") && !url.startsWith("https://")) return false;
  client_ = &client;
  client_->stop();
  url_ = url;
  headers_.clear();
  size_ = -1;
  location_ = "";
  return true;
}

void HTTPClient::end() {
  headers_.clear();
  url_ = "";
}

void HTTPClient::collectHeaders(const char* names[], size_t n) {
  wanted_.clear();
  for (size_t i = 0; i < n; i++) wanted_.emplace_back(names[i], "");
}

int HTTPClient::GET() { return request("GET", nullptr, 0); }

int HTTPClient::request(const char* method, const char* body, size_t len) {
  if (!client_) return HTTPC_ERROR_NOT_CONNECTED;
  if (WiFi.status() != WL_CONNECTED) return HTTPC_ERROR_CONNECTION_REFUSED;
  static bool inited = (curl_global_init(CURL_GLOBAL_DEFAULT), true);
  (void)inited;
  CURL* curl = curl_easy_init();
  if (!curl) return HTTPC_ERROR_CONNECTION_REFUSED;
  client_->stop();
  for (auto& w : wanted_) w.second = "";
  location_ = "";
  HeaderSink sink{&wanted_, &location_};
  curl_slist* list = nullptr;
  for (const auto& h : headers_) list = curl_slist_append(list, (h.first + ": " + h.second).c_str());
  curl_easy_setopt(curl, CURLOPT_URL, url_.c_str());
  curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, method);
  if (body) {
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)len);
  }
  curl_easy_setopt(curl, CURLOPT_HTTPHEADER, list);
  curl_easy_setopt(curl, CURLOPT_USERAGENT, userAgent_.c_str());
  curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, (long)connectTimeout_);
  curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1L);  // like the device's read timeout: give up when data stops
  curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, (long)std::max(1, timeout_ / 1000));
  curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, follow_ == HTTPC_DISABLE_FOLLOW_REDIRECTS ? 0L : 1L);
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, onBody);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &client_->body_);
  curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, onHeader);
  curl_easy_setopt(curl, CURLOPT_HEADERDATA, &sink);
  curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "identity");
  if (auto* tls = dynamic_cast<WiFiClientSecure*>(client_); tls && tls->insecure_) {
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
  }
  const CURLcode rc = curl_easy_perform(curl);
  long code = 0;
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
  curl_slist_free_all(list);
  curl_easy_cleanup(curl);
  if (rc != CURLE_OK) {
    Serial.printf("[sim] %s %s: %s\n", method, url_.c_str(), curl_easy_strerror(rc));
    client_->stop();
    return rc == CURLE_OPERATION_TIMEDOUT ? HTTPC_ERROR_READ_TIMEOUT : HTTPC_ERROR_CONNECTION_REFUSED;
  }
  size_ = (int)client_->body_.size();
  return (int)code;
}

String HTTPClient::getString() {
  if (!client_) return String();
  String s(client_->body_.data() + client_->pos_, client_->body_.size() - client_->pos_);
  client_->pos_ = client_->body_.size();
  return s;
}

String HTTPClient::header(const char* name) const {
  for (const auto& w : wanted_)
    if (w.first.equalsIgnoreCase(name)) return w.second;
  return String();
}

String HTTPClient::errorToString(int error) {
  switch (error) {
    case HTTPC_ERROR_CONNECTION_REFUSED: return "connection refused";
    case HTTPC_ERROR_SEND_HEADER_FAILED: return "send header failed";
    case HTTPC_ERROR_NOT_CONNECTED: return "not connected";
    case HTTPC_ERROR_CONNECTION_LOST: return "connection lost";
    case HTTPC_ERROR_READ_TIMEOUT: return "read timeout";
    default: return String();
  }
}
