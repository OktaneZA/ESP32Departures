// Buttons or touch, depending on the board — see input.h.

#include "input.h"
#include "board.h"
#include "display.h"

namespace {

// Bare tactile switches bounce; without requiring the level to have been stable
// a single push registers several times. Kept from the original button code.
constexpr uint32_t BTN_DEBOUNCE_MS = 40;

// Resistive panels report a touch continuously while a finger rests on them, so
// a tap is the *edge*. This also stops a slow press stepping several panels.
constexpr uint32_t TOUCH_DEBOUNCE_MS = 250;

bool edge(bool down, bool& lastStable, uint32_t& changedAt, uint32_t debounceMs) {
    if (down != lastStable) {
        if (millis() - changedAt >= debounceMs) {
            lastStable = down;
            changedAt = millis();
            return down;            // changed, and it settled: a real press
        }
    } else {
        changedAt = millis();
    }
    return false;
}

}  // namespace

namespace input {

void begin() {
    if (board::HAS_BUTTONS) {
        // BUTTON_1 is the BOOT pin and has an external pull-up; BUTTON_2 needs
        // the internal one. Enabling it on both is harmless and says less about
        // the board than two different calls would.
        pinMode((uint8_t)board::PIN_BTN_CLOCK, INPUT_PULLUP);
        pinMode((uint8_t)board::PIN_BTN_NEXT, INPUT_PULLUP);
    }
    // Touch needs no setup of its own: LovyanGFX brings the controller up as
    // part of the panel, which is why this must run after ui::begin().
}

Press poll() {
    Press p;

    if (board::HAS_BUTTONS) {
        static bool clockDown = false, nextDown = false;
        static uint32_t clockAt = 0, nextAt = 0;
        p.clock = edge(digitalRead((uint8_t)board::PIN_BTN_CLOCK) == LOW,
                       clockDown, clockAt, BTN_DEBOUNCE_MS);
        p.next  = edge(digitalRead((uint8_t)board::PIN_BTN_NEXT) == LOW,
                       nextDown, nextAt, BTN_DEBOUNCE_MS);
        return p;
    }

    if (board::HAS_TOUCH) {
        // Split the screen down the middle: left half is the clock, right half
        // steps on. Deliberately invisible and deliberately huge — there is no
        // on-screen target to miss, and half a screen is hard to get wrong in
        // the dark. Which half is decided at touch-down, so a drag cannot
        // change its mind halfway.
        static bool touchDown = false;
        static uint32_t touchAt = 0;
        int x = 0, y = 0;
        bool down = ui::getTouch(x, y);
        static int downX = 0;
        if (down) downX = x;
        if (edge(down, touchDown, touchAt, TOUCH_DEBOUNCE_MS)) {
            if (downX < board::SCREEN_W / 2) p.clock = true;
            else                             p.next = true;
        }
    }
    return p;
}

}  // namespace input
