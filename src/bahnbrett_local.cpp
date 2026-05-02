#ifdef BAHNBRETT_LOCAL_RENDER

#include "bahnbrett_local.h"

#include <Arduino.h>
#include <WiFi.h>
#include <display.h>

#include <bahnbrett/bahnbrett.h>
#include <bahnbrett/config.h>
#include <bahnbrett/http_client.h>
#include <bahnbrett/renderer.h>
#include <bahnbrett/time_utils.h>
#include <bahnbrett/transport_rest_provider.h>
#include <bahnbrett/trmnl_bmp_backend.h>

#include <vector>

namespace {

bool RenderAndPresent(const bahnbrett::DashboardConfig& config,
                      const std::vector<bahnbrett::WidgetData>& data,
                      const bahnbrett::RenderStatus& status) {
  bahnbrett::EpochSeconds now_epoch = bahnbrett::NowEpochSeconds();
  bahnbrett::Bitmap bitmap =
      bahnbrett::RenderDashboard(config, data, now_epoch, status);

  bahnbrett::DisplayTarget target;
  target.width = config.width;
  target.height = config.height;
  target.format = config.format;
  bahnbrett::TrmnlBmpBackend backend(target);
  return backend.Present(bitmap);
}

}  // namespace

https_request_err_e bahnbrett_local_render(void)
{
  bahnbrett::DashboardConfig config = bahnbrett::DefaultDashboardConfig();
  config.width = static_cast<int>(display_width());
  config.height = static_cast<int>(display_height());
  config.format = bahnbrett::PixelFormat::kMono1;

  bahnbrett::RenderStatus status;
  std::vector<bahnbrett::WidgetData> data;

  if (WiFi.status() != WL_CONNECTED) {
    status.wifi_state = bahnbrett::RenderStatus::WifiState::kNotConnected;
    status.wifi_rssi_dbm = -100;
    status.error_message = "WiFi unavailable";
    RenderAndPresent(config, data, status);
    return HTTPS_UNABLE_TO_CONNECT;
  }

  status.wifi_state = bahnbrett::RenderStatus::WifiState::kConnected;
  status.wifi_rssi_dbm = WiFi.RSSI();

  bahnbrett::ArduinoHttpClient http_client(true);
  bahnbrett::TransportRestProvider provider("https://v6.vbb.transport.rest",
                                            http_client);
  data = provider.GetDashboard(config);

  bool has_station = false;
  for (const auto& widget : data) {
    if (!widget.station.id.empty()) {
      has_station = true;
      break;
    }
  }
  if (!has_station) {
    status.error_message = "Departures unavailable";
    RenderAndPresent(config, data, status);
    return HTTPS_REQUEST_FAILED;
  }

  if (!RenderAndPresent(config, data, status)) {
    return HTTPS_REQUEST_FAILED;
  }

  return HTTPS_SUCCESS;
}

#endif  // BAHNBRETT_LOCAL_RENDER
