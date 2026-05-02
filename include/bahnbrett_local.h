#pragma once

#ifdef BAHNBRETT_LOCAL_RENDER

#include "types.h"

#ifdef __cplusplus
extern "C" {
#endif

https_request_err_e bahnbrett_local_render(void);
void bahnbrett_trigger_easter_egg(void);

#ifdef __cplusplus
}
#endif

#endif  // BAHNBRETT_LOCAL_RENDER
