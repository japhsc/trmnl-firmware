#pragma once

#include <string>

#ifdef ARDUINO
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#endif

namespace bahnbrett {

struct HttpResponse {
  int status = 0;
  std::string body;
};

class HttpClient {
 public:
  virtual ~HttpClient() = default;
  virtual HttpResponse Get(const std::string& url) = 0;
};

#ifdef ARDUINO
class ArduinoHttpClient : public HttpClient {
 public:
  explicit ArduinoHttpClient(bool insecure_tls);
  ~ArduinoHttpClient() override;
  HttpResponse Get(const std::string& url) override;

 private:
  bool insecure_tls_;
  WiFiClientSecure client_;
  HTTPClient http_;
};
#endif

}  // namespace bahnbrett
