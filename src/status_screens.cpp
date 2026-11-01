#include <ArduinoLog.h>
#include <WifiCaptive.h>
#include <config.h>
#include <globals.h>
#include <message_screens.h>
#include <status_screens.h>
#include <trmnl_log.h>

#ifdef BAHNBRETT_LOCAL_RENDER
#include <bahnbrett_local.h>
#endif

#include "displayed_image.h"
#include "esp_flash.h"
#include "loading.h"
#include "logo_medium.h"
#include "logo_small.h"

#ifdef BOARD_TRMNL_X
#include <touchbar_actions.h>
#include <touchbar_gesture.h>
#endif

void showMessageWithLogo(MSG message_type, String friendly_id, bool id, const char *fw_version, String message) {
  display_show_msg(storedLogoOrDefault(0), message_type, friendly_id, id, fw_version, message);
  need_to_refresh_display = 1;
  preferences.putBool(PREFERENCES_DEVICE_REGISTERED_KEY, false);
}

void showMessageWithLogo(MSG message_type) { display_show_msg(storedLogoOrDefault(0), message_type); }

void showMessageWithLogo(MSG message_type, const ApiSetupResponse &apiResponse) {
  display_show_msg(storedLogoOrDefault(0), message_type, "", false, "", apiResponse.message);
  need_to_refresh_display = 1;
  preferences.putBool(PREFERENCES_DEVICE_REGISTERED_KEY, false);
}

uint8_t *storedLogoOrDefault(int iType) {
  //
  // See if there are custom art assets in FLASH memory.
  // The top 4K of FLASH would be reserved for this data.
  // The images are stored as: logo_medium, loading
  //
  uint32_t u32Size;
  // esp_flash_t chip;
  uint8_t *s;
  uint16_t u16Size;
  BRAND *pBrand;

  u32Size = ESP.getFlashChipSize();
  Log_info("%s [%d]: esp flash size: %" PRIu32 "\r\n", __FILE__, __LINE__, u32Size);
  if (u32Size != 0) {
    pBrand = (BRAND *)malloc(sizeof(BRAND)); // DEBUG - we can leak this memory for now
    esp_flash_init(NULL);
    esp_flash_read(NULL, (void *)pBrand, u32Size - sizeof(BRAND), sizeof(BRAND));
    if (*(uint16_t *)&pBrand->u8Images[0] == 0xBBBF /*BB_BITMAP_MARKER*/) {
      // Group5 compressed images are present, use them
      if (iType == 0) {
        return &pBrand->u8Images[0]; // the first image is the medium sized logo
      } else { // the second image is the loading screen with small logo
        // get the pointer to the loading image
        s = &pBrand->u8Images[0];
        u16Size = *(uint16_t *)&s[6]; // compressed image size
        s += u16Size + 8; // skip to loading image
        return s;
      }
    }
  }
#ifdef PARALLEL_EPD
  return const_cast<uint8_t *>(logo_medium);
#else
  if (iType == 0) {
    return const_cast<uint8_t *>(logo_small);
  } else {
    // Force the loading screen to always use the slower update method because
    // we don't know (yet) if the panel can handle the faster update modes
    apiDisplayResult.response.maximum_compatibility = true;
    return const_cast<uint8_t *>(loading);
  }
#endif
}

void showWakeLogo(void) {
  Log.info("%s [%d]: Display TRMNL logo start\r\n", __FILE__, __LINE__);

#ifdef BOARD_TRMNL_X

  if (!otg_message && WifiCaptivePortal.isSaved()) {
    display_show_image(storedLogoOrDefault(1), DEFAULT_IMAGE_SIZE, false, true);
    touchbar_redraw_pending_indicator();
  } else if (!WifiCaptivePortal.isSaved()) {
    showMessageWithLogo(NONE);
  }
#else
#ifdef BAHNBRETT_LOCAL_RENDER
  // The Bahnbrett build replaces the stock logo flash with its own boot
  // screen. The screen carries the departure-data attribution; the
  // dashboard shows no footer; see ADR 0038. The screen shows on the
  // first boot only: PREFERENCES_DEVICE_REGISTERED_KEY exists after the
  // first boot, because the tail below writes it. The present step takes
  // a full refresh, because the screen replaces the whole panel.
  if (!preferences.isKey(PREFERENCES_DEVICE_REGISTERED_KEY)) {
    bahnbrett_render_boot_screen();
  }
#else
  display_show_image(storedLogoOrDefault(1), DEFAULT_IMAGE_SIZE, false, true);
#endif
#endif // BOARD_TRMNL_X
  // Force the display to show the current playlist image after the loading screen
  // (even if it hasn't changed)
  DisplayedImage::cover();

  need_to_refresh_display = 1;
  preferences.putBool(PREFERENCES_DEVICE_REGISTERED_KEY, false);
  Log.info("%s [%d]: Display TRMNL logo end\r\n", __FILE__, __LINE__);
  preferences.putString(PREFERENCES_FILENAME_KEY, "");
}
