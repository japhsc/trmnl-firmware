#ifdef BAHNBRETT_LOCAL_RENDER

#include "bahnbrett_local.h"

#include <Arduino.h>
#include <WiFi.h>
#include <cstdlib>
#include <api-client/display.h>
#include <display.h>
#include <esp_heap_caps.h>
#include <trmnl_log.h>

#include <qrcode.h>

#include <bahnbrett/bahnbrett.h>
#include <bahnbrett/bmp_builder.h>
#include <bahnbrett/config.h>
#include <bahnbrett/config_store.h>
#include <bahnbrett/font_5x7.h>
#include <bahnbrett/http_client.h>
#include <bahnbrett/json.h>
#include <bahnbrett/renderer.h>
#include <bahnbrett/time_utils.h>
#include <bahnbrett/transport_rest_provider.h>
#include <bahnbrett/trmnl_bmp_backend.h>

#include <Preferences.h>

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

bool bahnbrett_render_easter_egg_now(void) {
  if (!g_show_easter_egg) return false;
  g_show_easter_egg = false;

  bahnbrett::DashboardConfig config = bahnbrett::DefaultDashboardConfig();
  config.width = static_cast<int>(display_width());
  config.height = static_cast<int>(display_height());
  config.format = bahnbrett::PixelFormat::kMono1;
  config.bitmap_header_pad = bahnbrett::kBmpHeaderSize;

  bahnbrett::Bitmap bitmap = MakeStaticBitmap(config);
  bahnbrett::RenderEasterEggInto(bitmap, config);
  return PresentBitmap(config, &bitmap);
}

namespace {

// Draw a QR code onto the bitmap at (ox, oy) with each module scaled to
// `scale`×`scale` pixels. Returns the total pixel height used (size * scale).
int DrawQRCode(bahnbrett::Bitmap& bitmap, int ox, int oy, int scale,
               const char* text, uint8_t fg) {
  // Version 4: 33×33 modules, Low ECC = ~62 bytes in byte mode.
  // WiFi QR strings contain ';' which forces byte mode (not alphanumeric),
  // so Version 2 (32 byte limit) silently overflows.
  constexpr int kVersion = 4;
  uint8_t buf[qrcode_getBufferSize(kVersion)];
  QRCode qr;
  qrcode_initText(&qr, buf, kVersion, ECC_LOW, text);

  for (int y = 0; y < qr.size; y++) {
    for (int x = 0; x < qr.size; x++) {
      if (qrcode_getModule(&qr, x, y)) {
        bitmap.FillRect(ox + x * scale, oy + y * scale, scale, scale, fg);
      }
    }
  }
  return qr.size * scale;
}

}  // namespace

void bahnbrett_render_config_screen(void) {
  bahnbrett::DashboardConfig config = bahnbrett::DefaultDashboardConfig();
  config.width = static_cast<int>(display_width());
  config.height = static_cast<int>(display_height());
  config.format = bahnbrett::PixelFormat::kMono1;
  config.bitmap_header_pad = bahnbrett::kBmpHeaderSize;

  bahnbrett::Bitmap bitmap = MakeStaticBitmap(config);
  int w = bitmap.width();
  int h = bitmap.height();

  constexpr uint8_t kWhite = 255;
  constexpr uint8_t kBlack = 0;
  bitmap.Clear(kWhite);

  // Top bar (black strip).
  int bar_h = 60;
  bitmap.FillRect(0, 0, w, bar_h, kBlack);

  // Title in top bar (white on black).
  bitmap.DrawTextScaled(20, 12, "Bahnbrett", kWhite, 3);
  bitmap.DrawTextScaled(20, 34, "Configuration", kWhite, 2);

  // ── Left column: text instructions ──
  int y = bar_h + 30;
  int left = 30;
  bitmap.DrawTextScaledF(left, y, "Connect to WiFi:", kBlack, 1.6f);
  y += 35;
  bitmap.DrawTextScaledF(left + 10, y, "Bahnbrett-Config", kBlack, 2.0f);

  y += 45;
  bitmap.DrawTextScaledF(left, y, "Open in browser:", kBlack, 1.6f);
  y += 35;
  bitmap.DrawTextScaledF(left + 10, y, "http://10.254.0.1", kBlack, 1.8f);

  y += 55;
  bitmap.DrawTextScaledF(left, y, "Timeout: 5 minutes", kBlack, 1.4f);

  // ── Right column: QR codes ──
  // QR version 4: 17 + 4*4 = 33 modules. At 4px scale = 132×132 px.
  // Version 2 (25×25) was too small — the ';' chars force byte mode (32 byte
  // limit at Low ECC), and the WiFi string overflows silently.
  constexpr int kQrScale = 4;
  constexpr int kQrModules = 33;  // version 4
  constexpr int qr_w = kQrModules * kQrScale;
  int qr_right = w - 40;

  auto drawQrLabel = [&](int qx, int qy_below, int qr_w, const std::string& label) {
    int tw = static_cast<int>(label.size() * (bahnbrett::kFont5x7Width + 1) * 1.4f);
    bitmap.DrawTextScaledF(qx + (qr_w - tw) / 2, qy_below, label, kBlack, 1.4f);
  };

  // WiFi QR code
  int wifi_qr_x = qr_right - qr_w;
  int wifi_qr_y = bar_h + 30;
  int wifi_h = DrawQRCode(bitmap, wifi_qr_x, wifi_qr_y, kQrScale,
                          "WIFI:T:nopass;S:Bahnbrett-Config;P:;;", kBlack);
  drawQrLabel(wifi_qr_x, wifi_qr_y + wifi_h + 6, qr_w, "WiFi");

  // URL QR code
  int url_qr_y = wifi_qr_y + wifi_h + 50;
  int url_h = DrawQRCode(bitmap, wifi_qr_x, url_qr_y, kQrScale,
                         "http://10.254.0.1/", kBlack);
  drawQrLabel(wifi_qr_x, url_qr_y + url_h + 6, qr_w, "Config page");

  // Thin vertical separator between text and QR columns.
  bitmap.FillRect(qr_right + 10, bar_h + 20, 2, h - bar_h - 80, kBlack);

  // Footer hint.
  int footer_y = h - 40;
  bitmap.DrawTextScaledF(left, footer_y, "Triple-click the button to enter this mode", kBlack, 1.2f);
  bitmap.FillRect(0, footer_y - 15, w, 2, kBlack);

  PresentBitmap(config, &bitmap);
}

https_request_err_e bahnbrett_local_render(void)
{
  Log_info("[bahnbrett] bahnbrett_local_render entry");
  bahnbrett_log_heap("entry");

  // Load config from NVS (merged onto defaults).
  bahnbrett::DashboardConfig config = bahnbrett::LoadEffectiveConfig();

  // configTime(0,0,...) in bl.cpp leaves TZ as UTC; apply the POSIX TZ string
  // from config so localtime_r returns the correct local time (incl. DST).
  setenv("TZ", config.timezone.c_str(), 1);
  tzset();
  config.width = static_cast<int>(display_width());
  config.height = static_cast<int>(display_height());
  config.format = bahnbrett::PixelFormat::kMono1;
  config.bitmap_header_pad = bahnbrett::kBmpHeaderSize;
  Log_info("[bahnbrett] config: w=%d h=%d format=mono1 header_pad=%d",
           config.width, config.height, config.bitmap_header_pad);

  // Easter egg is now handled earlier in bl_init(), before WiFi.
  // If we reach here, g_show_easter_egg is already false.

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

  // Optional: fetch remote config JSON if a URL is configured.
  {
    Preferences prefs;
    if (prefs.begin("bahnbrett", true)) {
      String remote_url = prefs.getString("remote_url", "");
      int remote_interval = prefs.getInt("remote_intv", 1);
      int wakeup_count = prefs.getInt("remote_wakeup", 0);
      prefs.end();

      if (!remote_url.isEmpty() && remote_interval > 0) {
        wakeup_count++;
        bool should_fetch = (wakeup_count >= remote_interval);
        Log_info("[bahnbrett] remote config: url=%s interval=%d count=%d fetch=%d",
                 remote_url.c_str(), remote_interval, wakeup_count, should_fetch ? 1 : 0);

        if (should_fetch) {
          // Reset counter
          {
            Preferences p;
            if (p.begin("bahnbrett", false)) {
              p.putInt("remote_wakeup", 0);
              p.end();
            }
          }

          bahnbrett::ArduinoHttpClient http_client(true);
          bahnbrett::HttpResponse resp = http_client.Get(remote_url.c_str());
          if (resp.status == 200 && !resp.body.empty()) {
            bahnbrett::JsonValue root;
            if (bahnbrett::ParseJson(resp.body, &root)) {
              bahnbrett::MergeJsonIntoConfig(&config, root);
              Log_info("[bahnbrett] remote config applied: stations=%d grid=%dx%d",
                       (int)config.station_names.size(), config.grid_cols, config.grid_rows);
            } else {
              Log_info("[bahnbrett] remote config: JSON parse failed");
            }
          } else {
            Log_info("[bahnbrett] remote config: fetch failed (HTTP %d)", resp.status);
          }
        } else {
          // Persist incremented counter
          Preferences p;
          if (p.begin("bahnbrett", false)) {
            p.putInt("remote_wakeup", wakeup_count);
            p.end();
          }
        }
      }
    }
  }

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
