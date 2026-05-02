// Unity build for Bahnbrett core (ESP32 firmware integration).
// Only compiled when BAHNBRETT_LOCAL_RENDER is defined; other environments
// include this library via PlatformIO LDF but must not build these sources.
#ifdef BAHNBRETT_LOCAL_RENDER

#include "../../../../../src/bitmap.cpp"
#include "../../../../../src/bmp_builder.cpp"
#include "../../../../../src/config.cpp"
#include "../../../../../src/filters.cpp"
#include "../../../../../src/json.cpp"
#include "../../../../../src/renderer.cpp"
#include "../../../../../src/time_utils.cpp"
#include "../../../../../src/transport_rest_provider.cpp"
#include "../../../../../src/http_client_arduino.cpp"
#include "../../../../../src/trmnl_bmp_backend.cpp"

#endif  // BAHNBRETT_LOCAL_RENDER
