// =============================================================================
// MatshitaPico -- Solid-state CD-ROM drive replacement for the Commodore CDTV
// Copyright (c) 2026 Nicola Avanzi (na103)
// Licensed under Creative Commons Attribution 4.0 International (CC BY 4.0).
// See LICENSE.
// =============================================================================
// ui_logic.h -- Pure UI logic (no Pico SDK): button FSM + LED patterns.
// Split out of ui.cpp (which does the GPIO glue) so it is unit-testable on the PC.
// =============================================================================
#pragma once
#include <cstdint>

namespace ui {

enum class Led      { Ready, Access, DiscChange, Ejected, Error };
enum class BtnEvent { None, Short, Long };

constexpr uint32_t DEBOUNCE_MS = 25;     // debounce
constexpr uint32_t LONG_MS     = 1500;   // long-press threshold (eject)

// NEXT button FSM (active low -> 'raw_pressed' = contact closed).
// Call update() every service() with the current time in ms.
class ButtonFsm {
public:
    void reset(uint32_t now_ms) {
        stable_pressed = last_raw = false;
        last_change = press_start = now_ms;
        long_fired = false;
    }
    BtnEvent update(uint32_t now_ms, bool raw_pressed) {
        BtnEvent ev = BtnEvent::None;
        if (raw_pressed != last_raw) { last_raw = raw_pressed; last_change = now_ms; }
        // adopt the stable level after the debounce
        if (raw_pressed != stable_pressed && (now_ms - last_change) >= DEBOUNCE_MS) {
            stable_pressed = raw_pressed;
            if (stable_pressed) { press_start = now_ms; long_fired = false; }   // pressed
            else if (!long_fired) ev = BtnEvent::Short;                          // short release
        }
        // long press: fires when the threshold elapses while held
        if (stable_pressed && !long_fired && (now_ms - press_start) >= LONG_MS) {
            long_fired = true;
            ev = BtnEvent::Long;
        }
        return ev;
    }
private:
    bool     stable_pressed = false;
    bool     last_raw       = false;
    uint32_t last_change    = 0;
    uint32_t press_start    = 0;
    bool     long_fired     = false;
};

// LED state at a given instant (pattern).
inline bool led_on(Led state, uint32_t t_ms) {
    switch (state) {
        case Led::Ready:      return true;                       // solid on
        case Led::Ejected:    return false;                      // off
        case Led::Error:      return (t_ms % 1000) < 500;        // slow blink
        case Led::Access:     return (t_ms % 120) < 60;          // fast blink
        case Led::DiscChange: { uint32_t p = t_ms % 1000;        // double flash
                                return (p < 120) || (p >= 240 && p < 360); }
    }
    return false;
}

} // namespace ui
