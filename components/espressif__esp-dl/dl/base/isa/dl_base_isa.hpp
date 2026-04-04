#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#if CONFIG_XTENSA_BOOST || CONFIG_TIE728_BOOST
#include "dl_base_xtensa.h"
#endif

#if CONFIG_TIE728_BOOST
#include "dl_base_tie728.h"
#endif

#if CONFIG_IDF_TARGET_ESP32P4
#include "dl_base_esp32p4.h"
#endif // CONFIG_IDF_TARGET_ESP32P4

#ifdef __cplusplus
}
#endif
