#pragma once

#ifdef BAHNBRETT_LOCAL_RENDER

#include "types.h"

#ifdef __cplusplus
extern "C" {
#endif

https_request_err_e bahnbrett_local_render(void);
void bahnbrett_trigger_easter_egg(void);
bool bahnbrett_render_easter_egg_now(void);
void bahnbrett_render_boot_screen(void);
void bahnbrett_render_config_screen(void);

// Set by the wake button handling (KEY2 or a triple click) when the device
// must enter the config mode. bl.cpp defines the flag; it survives deep
// sleep through RTC memory.
extern bool g_bahnbrett_config_mode;

#ifdef __cplusplus
}
#endif

#endif  // BAHNBRETT_LOCAL_RENDER
