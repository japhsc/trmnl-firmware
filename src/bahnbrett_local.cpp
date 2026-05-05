#ifdef BAHNBRETT_LOCAL_RENDER

#include "bahnbrett_local.h"

#include <Arduino.h>
#include <WiFi.h>
#include <api-client/display.h>
#include <display.h>
#include <esp_heap_caps.h>
#include <trmnl_log.h>

#include <bahnbrett/bahnbrett.h>
#include <bahnbrett/bmp_builder.h>
#include <bahnbrett/config.h>
#include <bahnbrett/http_client.h>
#include <bahnbrett/renderer.h>
#include <bahnbrett/time_utils.h>
#include <bahnbrett/transport_rest_provider.h>
#include <bahnbrett/trmnl_bmp_backend.h>

#include <vector>

extern ApiDisplayResult apiDisplayResult;

static void bahnbrett_log_heap(const char* tag) {
  size_t free_internal = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
  size_t largest_internal = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
  size_t free_total = ESP.getFreeHeap();
  Log_info("[bahnbrett] heap @ %s: total_free=%u internal_free=%u internal_largest=%u",
           tag, (unsigned)free_total, (unsigned)free_internal, (unsigned)largest_internal);
}

static bool g_show_easter_egg = false;

void bahnbrett_trigger_easter_egg(void) {
  g_show_easter_egg = true;
}

// Single static bitmap buffer reused for every refresh. Lives in BSS so the
// 48 KB chunk is reserved at link time and can never be lost to heap
// fragmentation. Sized for the worst-case TRMNL panel (800x480, 1-bpp) plus
// the 62-byte BMP header slot used by the in-place encoder.
// 4-byte aligned to satisfy any DMA / 32-bit-read assumptions in bbep.
namespace {

constexpr int kBahnbrettMaxWidth = 800;
constexpr int kBahnbrettMaxHeight = 480;
constexpr size_t kBahnbrettStaticBufferSize =
    static_cast<size_t>(bahnbrett::kBmpHeaderSize) +
    (kBahnbrettMaxWidth + 7) / 8 * kBahnbrettMaxHeight;

}  // namespace

alignas(4) static uint8_t g_bahnbrett_bitmap_buffer[kBahnbrettStaticBufferSize];

namespace {

bahnbrett::Bitmap MakeStaticBitmap(const bahnbrett::DashboardConfig& config) {
  size_t need = bahnbrett::Bitmap::RequiredBufferSize(
      config.width, config.height, config.format, config.bitmap_header_pad);
  if (need > kBahnbrettStaticBufferSize) {
    Log_info("[bahnbrett] FATAL: panel %dx%d needs %u bytes, static buffer is %u",
             config.width, config.height, (unsigned)need,
             (unsigned)kBahnbrettStaticBufferSize);
    abort();
  }
  return bahnbrett::Bitmap(config.width, config.height, config.format,
                           config.bitmap_header_pad,
                           g_bahnbrett_bitmap_buffer,
                           kBahnbrettStaticBufferSize);
}

bool PresentBitmap(const bahnbrett::DashboardConfig& config,
                   bahnbrett::Bitmap* bitmap) {
  bahnbrett::DisplayTarget target;
  target.width = config.width;
  target.height = config.height;
  target.format = config.format;
  bahnbrett::TrmnlBmpBackend backend(target);
  Log_info("[bahnbrett] PresentBitmap: w=%d h=%d header_pad=%d buf_size=%u path=%s",
           bitmap->width(), bitmap->height(), bitmap->header_pad(),
           (unsigned)bitmap->buffer_size(),
           bitmap->header_pad() == bahnbrett::kBmpHeaderSize ? "in-place" : "legacy");
  bool ok;
  if (bitmap->header_pad() == bahnbrett::kBmpHeaderSize) {
    ok = backend.PresentInPlace(bitmap);
  } else {
    ok = backend.Present(*bitmap);
  }
  Log_info("[bahnbrett] PresentBitmap result=%d", ok ? 1 : 0);
  return ok;
}

bool RenderAndPresent(const bahnbrett::DashboardConfig& config,
                      std::vector<bahnbrett::WidgetData>* data,
                      const bahnbrett::RenderStatus& status) {
  Log_info("[bahnbrett] RenderAndPresent: widgets=%u error='%s' wifi=%d",
           (unsigned)data->size(), status.error_message.c_str(),
           static_cast<int>(status.wifi_state));
  bahnbrett_log_heap("pre-render");

  bahnbrett::EpochSeconds now_epoch = bahnbrett::NowEpochSeconds();
  bahnbrett::Bitmap bitmap = MakeStaticBitmap(config);
  bahnbrett::RenderDashboardInto(bitmap, config, *data, now_epoch, status);
  Log_info("[bahnbrett] RenderDashboardInto done: bitmap w=%d h=%d stride=%d header_pad=%d size=%u owns=%d",
           bitmap.width(), bitmap.height(), bitmap.row_stride(),
           bitmap.header_pad(), (unsigned)bitmap.buffer_size(),
           bitmap.owns_storage() ? 1 : 0);

  std::vector<bahnbrett::WidgetData>().swap(*data);
  bahnbrett_log_heap("post-data-free");

  bool ok = PresentBitmap(config, &bitmap);
  bahnbrett_log_heap("post-present");
  return ok;
}

}  // namespace

https_request_err_e bahnbrett_local_render(void)
{
  Log_info("[bahnbrett] bahnbrett_local_render entry");
  bahnbrett_log_heap("entry");

  bahnbrett::DashboardConfig config = bahnbrett::DefaultDashboardConfig();
  config.width = static_cast<int>(display_width());
  config.height = static_cast<int>(display_height());
  config.format = bahnbrett::PixelFormat::kMono1;
  config.bitmap_header_pad = bahnbrett::kBmpHeaderSize;
  Log_info("[bahnbrett] config: w=%d h=%d format=mono1 header_pad=%d",
           config.width, config.height, config.bitmap_header_pad);

  if (g_show_easter_egg) {
    g_show_easter_egg = false;
    Log_info("[bahnbrett] easter egg path");
    bahnbrett::Bitmap bitmap = MakeStaticBitmap(config);
    bahnbrett::RenderEasterEggInto(bitmap, config);
    PresentBitmap(config, &bitmap);
    return HTTPS_SUCCESS;
  }

  bahnbrett::RenderStatus status;
  std::vector<bahnbrett::WidgetData> data;

  if (WiFi.status() != WL_CONNECTED) {
    Log_info("[bahnbrett] path=wifi-down");
    status.wifi_state = bahnbrett::RenderStatus::WifiState::kNotConnected;
    status.wifi_rssi_dbm = -100;
    status.error_message = "WiFi unavailable";
    RenderAndPresent(config, &data, status);
    return HTTPS_UNABLE_TO_CONNECT;
  }

  status.wifi_state = bahnbrett::RenderStatus::WifiState::kConnected;
  status.wifi_rssi_dbm = WiFi.RSSI();
  Log_info("[bahnbrett] path=wifi-ok rssi=%d, fetching dashboard", status.wifi_rssi_dbm);

  {
    bahnbrett::ArduinoHttpClient http_client(true);
    bahnbrett::TransportRestProvider provider("https://v6.vbb.transport.rest",
                                              http_client);
    data = provider.GetDashboard(config);
  }

  Log_info("[bahnbrett] GetDashboard returned widgets=%u", (unsigned)data.size());
  for (size_t i = 0; i < data.size(); ++i) {
    Log_info("[bahnbrett]   widget[%u]: station_id='%s' name='%s' departures=%u",
             (unsigned)i, data[i].station.id.c_str(),
             data[i].station.name.c_str(), (unsigned)data[i].departures.size());
  }

  // WiFi off as soon as fetch is done. Two reasons:
  //   1. Frees ~50 KB of WiFi/TLS heap and de-fragments enough for the
  //      48 KB Bitmap allocation that follows. Without this, total free
  //      can be 240+ KB but the largest contiguous block stays around
  //      40 KB and operator new throws bad_alloc.
  //   2. Avoids 2.4 GHz EMI and supply droop during the e-ink full
  //      refresh (which draws large bursts and is sensitive to both).
  // Mirrors the cloud path's disconnect at bl.cpp:904.
  bahnbrett_log_heap("pre-wifi-off");
  WiFi.disconnect(true);
  bahnbrett_log_heap("post-wifi-off");

  bool has_station = false;
  for (const auto& widget : data) {
    if (!widget.station.id.empty()) {
      has_station = true;
      break;
    }
  }
  Log_info("[bahnbrett] has_station=%d", has_station ? 1 : 0);

  if (!has_station) {
    status.error_message = "Departures unavailable";
    RenderAndPresent(config, &data, status);
    return HTTPS_REQUEST_FAILED;
  }

  if (!RenderAndPresent(config, &data, status)) {
    Log_info("[bahnbrett] RenderAndPresent reported failure");
    return HTTPS_REQUEST_FAILED;
  }

  Log_info("[bahnbrett] bahnbrett_local_render success");
  return HTTPS_SUCCESS;
}

#endif  // BAHNBRETT_LOCAL_RENDER
