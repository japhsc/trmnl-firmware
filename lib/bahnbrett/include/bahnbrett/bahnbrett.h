#pragma once

// Anchor header to force PlatformIO LDF to link the Bahnbrett library.
// Real headers live in the main repo include path; they are provided via build_flags.

#include <bahnbrett/config.h>
#include <bahnbrett/provider.h>
#include <bahnbrett/renderer.h>
#include <bahnbrett/trmnl_bmp_backend.h>
#include <bahnbrett/vbb_transport_rest_provider.h>
