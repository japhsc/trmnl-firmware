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
#include <bahnbrett/provider.h>
#include <bahnbrett/provider_factory.h>
#include <bahnbrett/renderer.h>
#include <bahnbrett/time_utils.h>
#include <bahnbrett/trmnl_bmp_backend.h>

#include <Preferences.h>

#include <config.h>

#include <unordered_map>
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
// fragmentation. Sized for the worst-case TRMNL panel (800x480, 1-bpp) at
// the padded 4-byte BMP row stride, plus the 62-byte BMP header slot used by
// the in-place encoder; see ADR 0051.
// 4-byte aligned to satisfy any DMA / 32-bit-read assumptions in bbep.
namespace {

constexpr int kBahnbrettMaxWidth = 800;
constexpr int kBahnbrettMaxHeight = 480;
constexpr size_t kBahnbrettStaticBufferSize =
    static_cast<size_t>(bahnbrett::kBmpHeaderSize) +
    static_cast<size_t>(((kBahnbrettMaxWidth + 31) / 32) * 4) *
        static_cast<size_t>(kBahnbrettMaxHeight);

}  // namespace

alignas(4) static uint8_t g_bahnbrett_bitmap_buffer[kBahnbrettStaticBufferSize];

namespace {

// The in-place encoder writes rows at the padded 4-byte BMP stride, so the
// per-panel buffer size must cover that stride, not the compact one; see
// ADR 0051.
size_t RequiredPanelBufferSize(const bahnbrett::DashboardConfig& config) {
  const size_t compact = bahnbrett::Bitmap::RequiredBufferSize(
      config.width, config.height, config.format, config.bitmap_header_pad);
  if (config.format != bahnbrett::PixelFormat::kMono1) {
    return compact;
  }
  const int bmp_row_stride = ((config.width + 31) / 32) * 4;
  const size_t padded =
      static_cast<size_t>(bahnbrett::kBmpHeaderSize) +
      static_cast<size_t>(bmp_row_stride) * static_cast<size_t>(config.height);
  return padded > compact ? padded : compact;
}

bahnbrett::Bitmap MakeStaticBitmap(const bahnbrett::DashboardConfig& config) {
  size_t need = RequiredPanelBufferSize(config);
  if (need > kBahnbrettStaticBufferSize) {
    Log_info("[bahnbrett] FATAL: panel %dx%d needs %u bytes, static buffer is %u",
             config.width, config.height, (unsigned)need,
             (unsigned)kBahnbrettStaticBufferSize);
    abort();
  }
  return bahnbrett::Bitmap(config.width, config.height, config.format,
                           config.bitmap_header_pad,
                           g_bahnbrett_bitmap_buffer,
                           need);
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
                      const bahnbrett::RenderStatus& status,
                      bahnbrett::EpochSeconds server_epoch) {
  Log_info("[bahnbrett] RenderAndPresent: widgets=%u error='%s' wifi=%d",
           (unsigned)data->size(), status.error_message.c_str(),
           static_cast<int>(status.wifi_state));
  bahnbrett_log_heap("pre-render");

  // Prefer the API's Date header over the local clock: on the device the
  // system clock drifts freely across deep-sleep cycles between NTP syncs
  // (see ADR 0037).
  bahnbrett::EpochSeconds now_epoch = server_epoch > 0
                                          ? server_epoch
                                          : bahnbrett::NowEpochSeconds();
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

// The boot flash that replaces the stock TRMNL logo at startup. The flash
// is the only screen that shows the departure-data attribution; the
// dashboard and the config screen carry no note; see ADR 0038.
void bahnbrett_render_boot_screen(void) {
  bahnbrett::DashboardConfig config = bahnbrett::LoadEffectiveConfig();
  config.width = static_cast<int>(display_width());
  config.height = static_cast<int>(display_height());
  config.format = bahnbrett::PixelFormat::kMono1;
  config.bitmap_header_pad = bahnbrett::kBmpHeaderSize;

  bahnbrett::Bitmap bitmap = MakeStaticBitmap(config);

  // The factory honors the HVV credential fallback, so the note names the
  // provider that the device actually uses.
  bahnbrett::ArduinoHttpClient http_client(true);
  bahnbrett::ProviderSelection selection =
      bahnbrett::CreateProvider(config, http_client);

  bahnbrett::RenderBootScreenInto(bitmap, config,
                                  selection.provider->Attribution());
  PresentBitmap(config, &bitmap);
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

  // Footer hint. The entry action differs per board: the three-key
  // boards enter the config mode with KEY2; the one-button boards need
  // a triple-click. See the button handling in bl.cpp.
  int footer_y = h - 40;
#if defined(BOARD_XIAO_EPAPER_DISPLAY) || defined(BOARD_XIAO_EPAPER_DISPLAY_3CLR)
  bitmap.DrawTextScaledF(left, footer_y, "Press KEY2 to enter this mode", kBlack, 1.2f);
#else
  bitmap.DrawTextScaledF(left, footer_y, "Triple-click the button to enter this mode", kBlack, 1.2f);
#endif
  bitmap.FillRect(0, footer_y - 15, w, 2, kBlack);

  PresentBitmap(config, &bitmap);
}

namespace {

// A cached station entry stays valid for seven days. After that the device
// resolves the configured names again so it follows API-side changes.
constexpr int32_t kStationCacheMaxAgeSeconds = 7 * 24 * 60 * 60;

std::string JsonStringOf(const bahnbrett::JsonValue& object, const char* key) {
  const bahnbrett::JsonValue* value = object.Find(key);
  if (!value || value->type != bahnbrett::JsonValue::Type::kString) {
    return {};
  }
  return value->string_value;
}

// Loads the persisted station cache from NVS. The function returns an empty
// map when NVS holds no cache, the cache is older than the max age, or the
// cache belongs to a different provider. Stop ids differ across providers,
// so a provider switch invalidates the cache; see ADR 0034.
std::unordered_map<std::string, bahnbrett::Station> LoadStationCache(
    const std::string& provider_name) {
  std::unordered_map<std::string, bahnbrett::Station> cache;
  Preferences prefs;
  if (!prefs.begin("bahnbrett", true)) {
    return cache;
  }
  int32_t saved_at = prefs.getInt("station_ts", 0);
  String raw = prefs.getString("station_map", "");
  String cached_provider = prefs.getString("station_prov", "");
  prefs.end();

  if (cached_provider.length() > 0 && cached_provider != provider_name.c_str()) {
    Log_info("[bahnbrett] The provider changed from %s to %s. The device drops the station cache.",
             cached_provider.c_str(), provider_name.c_str());
    return cache;
  }

  bahnbrett::EpochSeconds now = bahnbrett::NowEpochSeconds();
  if (saved_at <= 0 || now <= 0 ||
      now - saved_at > kStationCacheMaxAgeSeconds) {
    return cache;
  }

  bahnbrett::JsonValue root;
  if (!bahnbrett::ParseJson(std::string(raw.c_str()), &root) ||
      root.type != bahnbrett::JsonValue::Type::kObject) {
    return cache;
  }
  for (const auto& entry : root.object) {
    if (entry.second.type != bahnbrett::JsonValue::Type::kObject) {
      continue;
    }
    bahnbrett::Station station;
    station.id = JsonStringOf(entry.second, "id");
    station.name = JsonStringOf(entry.second, "name");
    station.parent_station = JsonStringOf(entry.second, "parent");
    if (station.id.empty()) {
      continue;
    }
    cache[entry.first] = std::move(station);
  }
  return cache;
}

// Persists the station cache to NVS. Entries without an id are skipped so
// that a failed lookup cannot overwrite a good entry from an earlier run.
// The provider name travels with the cache so a later wakeup can detect a
// provider switch.
void SaveStationCache(const std::unordered_map<std::string, bahnbrett::Station>& cache,
                      const std::string& provider_name) {
  if (cache.empty()) {
    return;
  }
  bahnbrett::JsonBuilder jb;
  jb.StartObject();
  size_t written = 0;
  for (const auto& entry : cache) {
    if (entry.second.id.empty()) {
      continue;
    }
    jb.Key(entry.first);
    jb.StartObject();
    jb.KeyString("id", entry.second.id);
    jb.KeyString("name", entry.second.name);
    jb.KeyString("parent", entry.second.parent_station);
    jb.EndObject();
    jb.Comma();
    written++;
  }
  jb.EndObject();
  if (written == 0) {
    return;
  }

  bahnbrett::EpochSeconds now = bahnbrett::NowEpochSeconds();
  if (now <= 0) {
    // The clock is not synchronised. Do not store a bogus timestamp.
    return;
  }
  Preferences prefs;
  if (prefs.begin("bahnbrett", false)) {
    prefs.putString("station_map", jb.str().c_str());
    prefs.putInt("station_ts", static_cast<int32_t>(now));
    prefs.putString("station_prov", provider_name.c_str());
    prefs.end();
    Log_info("[bahnbrett] The device stored %u resolved stations in NVS.",
             (unsigned)written);
  }
}

// Writes the configured refresh interval to the sleep-timer key that
// bl.cpp reads before deep sleep. Values below 30 seconds come from the
// old 10-second default and mean "not configured"; the device then keeps
// the 15-minute default so the panel and the battery stay healthy.
void ApplyRefreshInterval(const bahnbrett::DashboardConfig& config) {
  constexpr int kMinRefreshSeconds = 30;
  constexpr int kMaxRefreshSeconds = 24 * 60 * 60;
  constexpr int kDefaultRefreshSeconds = 900;
  int refresh = config.refresh_seconds;
  if (refresh < kMinRefreshSeconds) {
    refresh = kDefaultRefreshSeconds;
  }
  if (refresh > kMaxRefreshSeconds) {
    refresh = kMaxRefreshSeconds;
  }
  Preferences prefs;
  if (prefs.begin("data", false)) {
    prefs.putUInt(PREFERENCES_SLEEP_TIME_KEY, static_cast<uint32_t>(refresh));
    prefs.end();
  }
  Log_info("[bahnbrett] The device applies a refresh interval of %d seconds.",
           refresh);
}

// The departure fetch needs room for the response body, the JSON tree, and
// the TLS buffers. The crash log from 2026-10 shows 206 KB free at the
// render entry with WiFi up, so a 160 KB floor keeps room for the 48 KB
// body cap and its parse tree while it refuses a degraded heap; see
// ADR 0039.
constexpr size_t kMinFreeHeapForFetchBytes = 160 * 1024;

// Returns true when the heap holds enough room for a fetch. The function
// logs the numbers when it refuses, so the serial log shows the cause.
bool HeapHasRoomForFetch(const char* what) {
  size_t free_heap = ESP.getFreeHeap();
  if (free_heap >= kMinFreeHeapForFetchBytes) {
    return true;
  }
  Log_info("[bahnbrett] The device skips the %s fetch because the free heap is %u bytes and the floor is %u bytes. Restart the device to restore the heap.",
           what, (unsigned)free_heap,
           (unsigned)kMinFreeHeapForFetchBytes);
  return false;
}

}  // namespace

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
  Log_info("[bahnbrett] config: provider=%s refresh_seconds=%d",
           config.provider.c_str(), config.refresh_seconds);

  // Easter egg is now handled earlier in bl_init(), before WiFi.
  // If we reach here, g_show_easter_egg is already false.

  bahnbrett::RenderStatus status;
  std::vector<bahnbrett::WidgetData> data;
  bahnbrett::EpochSeconds server_epoch = 0;

  if (WiFi.status() != WL_CONNECTED) {
    Log_info("[bahnbrett] path=wifi-down");
    status.wifi_state = bahnbrett::RenderStatus::WifiState::kNotConnected;
    status.wifi_rssi_dbm = -100;
    status.error_message = "WiFi unavailable";
    RenderAndPresent(config, &data, status, server_epoch);
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

        if (should_fetch && !HeapHasRoomForFetch("remote config")) {
          // The heap is too low for the fetch. The else path keeps the
          // wakeup counter, so the next wake retries the remote config.
          should_fetch = false;
        }

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

  // The response cap of 48 KB holds about 20 stoptimes at the 2.2 KB that
  // ADR 0034 measures per entry, so the device clamps the configured count.
  // A busy stop returns fewer rows, and the dashboard still renders; see
  // ADR 0039.
  constexpr int kMaxDeparturesPerWidgetOnDevice = 20;
  if (config.max_departures_to_fetch_per_widget >
      kMaxDeparturesPerWidgetOnDevice) {
    Log_info("[bahnbrett] The device clamps the fetch count from %d to %d departures per widget.",
             config.max_departures_to_fetch_per_widget,
             kMaxDeparturesPerWidgetOnDevice);
    config.max_departures_to_fetch_per_widget = kMaxDeparturesPerWidgetOnDevice;
  }

  // Write the configured refresh interval to the sleep timer so bl.cpp
  // applies it when the device enters deep sleep. The remote config runs
  // first, so a remote refresh_seconds change applies at this wake; see
  // ADR 0042.
  ApplyRefreshInterval(config);

  // Prime the provider with stations that earlier wakeups resolved. The
  // device then skips one station lookup per configured name and pays for
  // the departures request only.
  const std::string provider_name =
      bahnbrett::ProviderKindName(bahnbrett::ParseProviderKind(config.provider));
  std::unordered_map<std::string, bahnbrett::Station> station_cache =
      LoadStationCache(provider_name);

  if (!HeapHasRoomForFetch("departures")) {
    status.error_message =
        "Memory is low. Restart the device and remove stations.";
    Log_info("[bahnbrett] The device shows the failure message: %s",
             status.error_message.c_str());
    RenderAndPresent(config, &data, status, server_epoch);
    return HTTPS_REQUEST_FAILED;
  }

  bahnbrett::FetchReport report;
  {
    bahnbrett::ArduinoHttpClient http_client(true);
    bahnbrett::ProviderSelection selection =
        bahnbrett::CreateProvider(config, http_client);
    if (selection.fell_back_to_default) {
      Log_info("[bahnbrett] The HVV provider has no credentials. The device uses Transitous instead.");
    }
    Log_info("[bahnbrett] The device uses the %s provider.", provider_name.c_str());
    // The fetcher resolves the configured names, skips the resolution for
    // names that the cache holds, and writes every resolved station back
    // into the cache. A failed lookup yields an empty id, so it cannot
    // poison the persisted cache.
    bahnbrett::DashboardFetcher fetcher(*selection.provider,
                                        config.min_poll_seconds);
    data = fetcher.Fetch(config, &station_cache);
    // The fetch responses carry the freshest server clock in their Date
    // headers. Read it before the block ends because the client frees its
    // TLS session here; see ADR 0037.
    server_epoch = http_client.server_epoch();
    report = selection.provider->fetch_report();
  }

  if (report.station_lookups > 0) {
    // Keep only the stations that the config lists. This prunes entries for
    // stations that the user removed.
    std::unordered_map<std::string, bahnbrett::Station> current;
    for (const auto& name : config.station_names) {
      auto entry = station_cache.find(name);
      if (entry != station_cache.end()) {
        current[name] = entry->second;
      }
    }
    SaveStationCache(current, provider_name);
  }

  Log_info("[bahnbrett] The provider made %d station lookups with %d failures and %d departure lookups with %d failures. The last failure kind is %d with status %d.",
           report.station_lookups, report.station_lookup_failures,
           report.departure_lookups, report.departure_lookup_failures,
           static_cast<int>(report.last_failure_kind), report.last_failure_status);
  Log_info("[bahnbrett] The fetcher returned widgets=%u", (unsigned)data.size());
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

  // All departure fetches failed while the station cache supplied the
  // station data. The grid would render empty, so show the failure screen
  // with the cause instead.
  bool all_departures_failed =
      report.departure_lookups > 0 &&
      report.departure_lookup_failures == report.departure_lookups;

  if (!has_station || all_departures_failed) {
    status.error_message = bahnbrett::DescribeFetchFailure(report);
    Log_info("[bahnbrett] The device shows the failure message: %s",
             status.error_message.c_str());
    RenderAndPresent(config, &data, status, server_epoch);
    return HTTPS_REQUEST_FAILED;
  }

  if (!RenderAndPresent(config, &data, status, server_epoch)) {
    Log_info("[bahnbrett] RenderAndPresent reported failure");
    return HTTPS_REQUEST_FAILED;
  }

  Log_info("[bahnbrett] bahnbrett_local_render success");
  return HTTPS_SUCCESS;
}

#endif  // BAHNBRETT_LOCAL_RENDER
