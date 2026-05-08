#include "bahnbrett/http_client.h"

#if defined(ARDUINO)

#include <Stream.h>

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

}  // namespace

ArduinoHttpClient::ArduinoHttpClient(bool insecure_tls) : insecure_tls_(insecure_tls) {
  if (insecure_tls_) {
    client_.setInsecure();
  }
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

  BAHNBRETT_HTTP_LOG("GET start");
  int code = http_.GET();
  response.status = code;
  int content_len = http_.getSize();
  BAHNBRETT_HTTP_LOG("GET done code=%d size=%d", code, content_len);

  if (code > 0) {
    constexpr size_t kMaxBody = 128 * 1024;
    if (content_len < 0 || static_cast<size_t>(content_len) <= kMaxBody) {
      if (content_len > 0) {
        response.body.reserve(static_cast<size_t>(content_len));
      }
      // writeToStream handles Transfer-Encoding: chunked. Feeding it our
      // sink avoids the double-allocation that getString() forced.
      StringSink sink(&response.body, kMaxBody);
      int written = http_.writeToStream(&sink);
      BAHNBRETT_HTTP_LOG("writeToStream returned %d body=%u overflow=%d",
                         written, (unsigned)response.body.size(),
                         sink.overflow() ? 1 : 0);
      if (sink.overflow()) {
        response.body.clear();
        response.body.shrink_to_fit();
        response.status = 0;
      }
    } else {
      BAHNBRETT_HTTP_LOG("oversize content_len=%d, rejecting", content_len);
      response.status = 0;
    }
  }

  BAHNBRETT_HTTP_LOG("Get() return status=%d body=%u", response.status,
                     (unsigned)response.body.size());
  return response;
}

}  // namespace bahnbrett

#endif
