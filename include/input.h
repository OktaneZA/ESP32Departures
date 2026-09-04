#pragma once
#include <Arduino.h>

// The two things a user can ask for, however the board lets them ask.
//
// The S3 has two tactile buttons. The CYD has a touchscreen and no usable
// button — BOOT is a strapping pin on the PCB edge, not a front-bezel control.
// Both produce exactly these two events, so main.cpp's rotation logic never
// learns which board it is running on.
namespace input {

struct Press {
    bool clock = false;   // hold the big clock on screen / release it again
    bool next  = false;   // step to the next panel
};

// Configure whatever the board actually has. Call once, after ui::begin() —
// on a touch board the panel has to exist before its touch controller does.
void begin();

// Poll once per frame. Each field is true only on the frame the press lands,
// never for as long as it is held.
Press poll();

}  // namespace input
