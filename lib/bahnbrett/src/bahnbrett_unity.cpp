// Unity build for Bahnbrett core (ESP32 firmware integration).
// Only compiled when BAHNBRETT_LOCAL_RENDER is defined; other environments
// include this library via PlatformIO LDF but must not build these sources.
#ifdef BAHNBRETT_LOCAL_RENDER

#include "../../../../../src/base64.cpp"
#include "../../../../../src/bitmap.cpp"
#include "../../../../../src/bmp_builder.cpp"
#include "../../../../../src/config.cpp"
#include "../../../../../src/filters.cpp"
#include "../../../../../src/hmac_sha1.cpp"
#include "../../../../../src/hvv_geofox_provider.cpp"
#include "../../../../../src/json.cpp"
#include "../../../../../src/provider.cpp"
#include "../../../../../src/provider_factory.cpp"
#include "../../../../../src/renderer.cpp"
#include "../../../../../src/time_utils.cpp"
#include "../../../../../src/transitous_provider.cpp"
#include "../../../../../src/trmnl_bmp_backend.cpp"
#include "../../../../../src/vbb_transport_rest_provider.cpp"
#include "http_client_arduino.cpp"

#endif  // BAHNBRETT_LOCAL_RENDER
