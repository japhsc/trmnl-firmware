#include "bahnbrett/http_client.h"
#include "bahnbrett/time_utils.h"

#if defined(ARDUINO)

// The direct includes let the PlatformIO dependency finder link the
// framework libraries to this library. The real http_client.h lives
// outside this tree, so the finder cannot follow it.
#include <HTTPClient.h>
#include <Stream.h>
#include <WiFiClientSecure.h>

#include <algorithm>

#if defined(BAHNBRETT_TRMNL)
#include <Arduino.h>
#define BAHNBRETT_HTTP_LOG(fmt, ...) \
  Serial.printf("[bahnbrett:http] " fmt "\n", ##__VA_ARGS__)
#else
#define BAHNBRETT_HTTP_LOG(fmt, ...) ((void)0)
#endif

namespace bahnbrett {

namespace {

// Stream sink that appends written bytes to a std::string. Used with
// HTTPClient::writeToStream so we get HTTPClient's chunked-transfer
// decoding for free without double-buffering the payload as both an
// Arduino String and a std::string. The cap protects the ESP32 heap.
class StringSink : public Stream {
 public:
  StringSink(std::string* out, size_t cap) : out_(out), cap_(cap) {}

  size_t write(uint8_t c) override {
    if (out_->size() >= cap_) {
      overflow_ = true;
      return 0;
    }
    out_->push_back(static_cast<char>(c));
    return 1;
  }

  size_t write(const uint8_t* data, size_t n) override {
    if (out_->size() + n > cap_) {
      size_t room = (out_->size() < cap_) ? cap_ - out_->size() : 0;
      out_->append(reinterpret_cast<const char*>(data), room);
      overflow_ = true;
      return room;
    }
    out_->append(reinterpret_cast<const char*>(data), n);
    return n;
  }

  // Stream is read/write; we never read from this sink.
  int available() override { return 0; }
  int read() override { return -1; }
  int peek() override { return -1; }
  void flush() override {}

  bool overflow() const { return overflow_; }

 private:
  std::string* out_;
  size_t cap_;
  bool overflow_ = false;
};

// Reads the collected `Date` response header into HttpResponse::date_epoch.
// The render clock comes from this header because the device RTC drifts
// across deep-sleep cycles; see ADR 0037.
void ApplyDateHeader(HTTPClient* http, HttpResponse* response) {
  if (!http->hasHeader("Date")) {
    return;
  }
  String date_header = http->header("Date");
  if (date_header.length() == 0) {
    return;
  }
  EpochSeconds date_epoch = 0;
  if (ParseHttpDate(std::string(date_header.c_str()), &date_epoch)) {
    response->date_epoch = date_epoch;
  }
}

// The response body cap keeps the body, the JSON tree, and the TLS buffers
// inside the device heap; see ADR 0039. The Linux client allows more.
constexpr size_t kMaxBodyBytes = 48 * 1024;

// Reads the response body into `response` through the shared sink. The
// function clears the body and zeroes the status when the body exceeds the
// cap, so the caller reports a transport failure.
bool ReadBody(HTTPClient& http, HttpResponse* response) {
  int content_len = http.getSize();
  if (content_len >= 0 && static_cast<size_t>(content_len) > kMaxBodyBytes) {
    BAHNBRETT_HTTP_LOG("oversize content_len=%d, rejecting", content_len);
    response->status = 0;
    return false;
  }
  if (content_len > 0) {
    response->body.reserve(static_cast<size_t>(content_len));
  }
  StringSink sink(&response->body, kMaxBodyBytes);
  int written = http.writeToStream(&sink);
  BAHNBRETT_HTTP_LOG("writeToStream returned %d body=%u overflow=%d",
                     written, (unsigned)response->body.size(),
                     sink.overflow() ? 1 : 0);
  if (sink.overflow()) {
    response->body.clear();
    response->body.shrink_to_fit();
    response->status = 0;
    return false;
  }
  return true;
}

}  // namespace

ArduinoHttpClient::ArduinoHttpClient(bool insecure_tls) : insecure_tls_(insecure_tls) {
  if (insecure_tls_) {
    client_.setInsecure();
  }
  http_.setUserAgent(kUserAgent);
  http_.setConnectTimeout(10000);
  http_.setTimeout(30000);
  // Keep the TLS session alive across all requests in one render cycle so
  // we pay for the handshake only once. Without this, each Get() allocates
  // a fresh mbedTLS context (~40 KB), which fragments the heap enough that
  // subsequent connections fail with code=-1.
  http_.setReuse(true);
}

ArduinoHttpClient::~ArduinoHttpClient() {
  http_.end();
  client_.stop();
}

HttpResponse ArduinoHttpClient::Get(const std::string& url) {
  HttpResponse response;

  if (!http_.begin(client_, url.c_str())) {
    BAHNBRETT_HTTP_LOG("begin() failed url=%s", url.c_str());
    return response;
  }

  static const char* kCollectedHeaders[] = {"Date"};
  http_.collectHeaders(kCollectedHeaders, 1);

  BAHNBRETT_HTTP_LOG("GET start");
  int code = http_.GET();
  response.status = code;
  BAHNBRETT_HTTP_LOG("GET done code=%d size=%d", code, http_.getSize());

  if (code > 0) {
    ApplyDateHeader(&http_, &response);
    NoteServerEpoch(response.date_epoch);
    ReadBody(http_, &response);
  }

  BAHNBRETT_HTTP_LOG("Get() return status=%d body=%u", response.status,
                     (unsigned)response.body.size());
  return response;
}

HttpResponse ArduinoHttpClient::Post(const std::string& url,
                                     const std::string& body,
                                     const std::string& content_type,
                                     const HttpHeaders& headers) {
  HttpResponse response;

  if (!http_.begin(client_, url.c_str())) {
    BAHNBRETT_HTTP_LOG("begin() failed url=%s", url.c_str());
    return response;
  }

  http_.addHeader("Content-Type", content_type.c_str());
  http_.addHeader("Accept-Encoding", "identity");
  for (const auto& header : headers) {
    http_.addHeader(header.first.c_str(), header.second.c_str());
  }

  static const char* kCollectedHeaders[] = {"Date"};
  http_.collectHeaders(kCollectedHeaders, 1);

  BAHNBRETT_HTTP_LOG("POST start body=%u", (unsigned)body.size());
  int code = http_.POST(body.c_str());
  response.status = code;
  BAHNBRETT_HTTP_LOG("POST done code=%d size=%d", code, http_.getSize());

  if (code > 0) {
    ApplyDateHeader(&http_, &response);
    NoteServerEpoch(response.date_epoch);
    ReadBody(http_, &response);
  }

  BAHNBRETT_HTTP_LOG("Post() return status=%d body=%u", response.status,
                     (unsigned)response.body.size());
  return response;
}

}  // namespace bahnbrett

#endif
