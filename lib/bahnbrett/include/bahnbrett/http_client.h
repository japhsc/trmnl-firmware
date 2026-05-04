#pragma once

#include <string>

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
  explicit ArduinoHttpClient(bool insecure_tls) : insecure_tls_(insecure_tls) {}
  HttpResponse Get(const std::string& url) override;

 private:
  bool insecure_tls_ = true;
};
#endif

}  // namespace bahnbrett
