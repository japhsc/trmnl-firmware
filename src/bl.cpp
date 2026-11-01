#include <Arduino.h>
#include <ArduinoLog.h>
#include <app_logger.h>
#include <bl.h>
#include <display.h>
#include <display_session.h>
#include <globals.h>
#include <request_handling.h>
#include <setup_session.h>
#include <sleep_session.h>
#include <status_screens.h>
#include <trmnl_log.h>
#include <wake_init.h>
#include <wifi_session.h>

#ifdef BAHNBRETT_LOCAL_RENDER
#include <bahnbrett_config_web.h>
#include <bahnbrett_local.h>

// Set by the wake button handling (KEY2 or a triple click) when the device
// must enter the config mode. The flag survives deep sleep through RTC
// memory, because the wake handling runs before the display comes up.
RTC_DATA_ATTR bool g_bahnbrett_config_mode = false;
#endif

/**
 * @brief Function to init business logic module
 * @param none
 * @return none
 */
void bl_init(void) {
  uint32_t init_time = bootEarly();
  wifiSessionInit();
  initPeripherals();
  WakeContext wake = readWakeContext();
  openPreferences();
  startBoardHardware(wake, init_time);
  if (wake.reason != ESP_SLEEP_WAKEUP_TIMER) showWakeLogo();
  logVersions();

#ifdef BAHNBRETT_LOCAL_RENDER
  if (g_bahnbrett_config_mode) {
    g_bahnbrett_config_mode = false;
    bahnbrett_render_config_screen();
    bool config_saved = bahnbrett_config_web_start();
    if (config_saved) {
      Log_info("[bahnbrett] Config mode: saved, rebooting to apply");
      display_sleep();
      ESP.restart();
    }
    Log_info("[bahnbrett] Config mode: cancelled or timed out, sleeping");
    display_sleep();
    goToSleep();
  }

  // Easter egg (double-click) renders immediately — no WiFi or API needed.
  if (bahnbrett_render_easter_egg_now()) {
    Log_info("[bahnbrett] Easter egg rendered, sleeping");
    display_sleep();
    goToSleep();
  }
#endif

  wifiSessionConnect(wake.showErrorsNow);
  updateTimeSinceSleep();
#ifdef BAHNBRETT_LOCAL_RENDER
  // The Bahnbrett build renders locally and needs no TRMNL cloud account.
  // The registration call contacts api.usetrmnl.com when no API key is
  // stored; its 404 branch draws the BYOD-license screen and sleeps before
  // bahnbrett_local_render() runs. Skip the registration entirely.
  Log_info("[bahnbrett] The device skips the TRMNL cloud registration.");
#else
  ensureDeviceSetUp(); // may sleep
#endif
  submitStoredLogs();
  log_retry = true;

#ifdef BAHNBRETT_LOCAL_RENDER
  // The Bahnbrett render path fetches the departures, renders the bitmap,
  // presents it, and applies its own failure sleep floor; see ADR 0045.
  // The stock result handling below never runs in this build.
  bahnbrett_local_render();

  // display go to sleep
  display_sleep();
  goToSleep();
#else
  // OTA checking, image checking and drawing
  https_request_err_e request_result = downloadAndShow(battery_voltage);
  Log.info("%s [%d]: request result - %s\r\n", __FILE__, __LINE__, https_request_err_str(request_result));
  showImageTooBigIfNeeded(request_result);
  applyRetryPolicy(request_result, wake.showErrorsNow); // may sleep
  submitStoredLogs();
  showFriendlyIdIfUnregistered(request_result);
  resetIfRequested(request_result);
  updateFirmwareIfDue();
  showRequestResult(request_result);

  // display go to sleep
  Log_info("%s [%d]: BL done, going to sleep...", __FILE__, __LINE__);
  display_sleep();
  goToSleep();
#endif
} /* bl_init() */

/**
 * @brief Function to process business logic module
 * @param none
 * @return none
 */
void bl_process(void) {}
