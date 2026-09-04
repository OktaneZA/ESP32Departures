#pragma once
// Which board this firmware is for.
//
// Everything that differs between the supported boards — panel driver and pin
// map, screen geometry, how many rows fit, whether there is PSRAM to put the
// back buffer in, and whether input arrives from buttons or a touchscreen —
// lives behind this header. Nothing above it should need to know.
//
// That containment is the whole point. Two bus providers did not leak into the
// renderer because both fill one BusArrival; two boards must not leak into the
// feeds or the layout for the same reason.
//
// Selected by a build flag in platformio.ini:
//
//   (default)    LilyGo T-Display-S3
//   -DBOARD_CYD  ESP32 Cheap Yellow Display
//
// On the back buffer: the S3 keeps a full frame in PSRAM for nothing. The CYD
// has no PSRAM, so its 153,600-byte frame competes with WiFi and mbedTLS in
// DRAM. Whether that fits is a measurement, not a guess — ui::begin() logs the
// free heap after allocating, and boards/cyd.h carries a BAND_H for rendering
// in horizontal strips if the measurement says it does not.

#if defined(BOARD_CYD)
  #include "boards/cyd.h"
#else
  #include "boards/tdisplay_s3.h"
#endif
