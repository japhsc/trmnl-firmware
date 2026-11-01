// Wrapper: compile the shared http_client_arduino.cpp from the core repo.
// The core copy in firmware/src/ stays authoritative; see ADR 0048. The
// direct framework includes let the PlatformIO dependency finder link the
// framework libraries to this library, because the real http_client.h
// lives outside this tree and the finder cannot follow it.
// The BAHNBRETT_TRMNL half of the guard matches the core's device files:
// the stock environments reach this library through the LDF but carry no
// -I../../include, so the include below cannot resolve there.
#if defined(ARDUINO) && defined(BAHNBRETT_TRMNL)

#include <HTTPClient.h>
#include <Stream.h>
#include <WiFiClientSecure.h>

#include "../../../../../src/http_client_arduino.cpp"

#endif  // ARDUINO && BAHNBRETT_TRMNL
