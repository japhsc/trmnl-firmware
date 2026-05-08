#include "bahnbrett/http_client.h"

#if defined(ARDUINO)

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

}  // namespace

HttpResponse ArduinoHttpClient::Get(const std::string& url) {
  HttpResponse response;

  // Determine if HTTPS is needed
  bool isHttps = (url.find("https://") != std::string::npos);

  // Allocate the appropriate WiFi client on the heap for proper scoping/cleanup
  WiFiClient* client = nullptr;
  if (isHttps) {
    WiFiClientSecure* secureClient = new WiFiClientSecure();
    if (insecure_tls_) {
      secureClient->setInsecure();
    }
    client = secureClient;
  } else {
    client = new WiFiClient();
  }

  if (!client) {
    BAHNBRETT_HTTP_LOG("Failed to allocate WiFiClient");
    return response;
  }

  {
    // Scoped block ensures HTTPClient is destroyed before client cleanup
    HTTPClient http;
    http.setConnectTimeout(10000);
    http.setTimeout(30000);
    http.setReuse(false);

    if (!http.begin(*client, url.c_str())) {
      BAHNBRETT_HTTP_LOG("begin() failed url=%s", url.c_str());
      http.end();
      client->stop();
      delete client;
      return response;
    }

    BAHNBRETT_HTTP_LOG("GET start");
    int code = http.GET();
    response.status = code;
    int content_len = http.getSize();
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
        int written = http.writeToStream(&sink);
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
    http.end();
  }

  client->stop();
  delete client;

  BAHNBRETT_HTTP_LOG("Get() return status=%d body=%u", response.status,
                     (unsigned)response.body.size());
  return response;
}

}  // namespace bahnbrett

#endif
