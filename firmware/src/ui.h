// =============================================================================
// MatshitaPico -- Solid-state CD-ROM drive replacement for the Commodore CDTV
// Copyright (c) 2026 Nicola Avanzi (na103)
// Licensed under Creative Commons Attribution 4.0 International (CC BY 4.0).
// See LICENSE.
// =============================================================================
// ui.h -- NEXT button (GP3) + status LED (GP25) -- disc changing.
//   - short press: next disc (eject -> insert via cd_media + STCH)
//   - long press (~1.5 s): deliberate eject (stays "no media")
//   - LED: ready / SD access / disc change / ejected / error
// The pure logic (debounce, long threshold, LED patterns) lives in ui_logic.h.
// =============================================================================
#pragma once
#include "ui_logic.h"   // enum Led/BtnEvent, ButtonFsm, led_on

namespace ui {

void init();      // GP3 input pull-up, GP25 LED output
void service();   // call from the loop: button debounce + LED pattern

// Signal SD/seek/read activity -> LED blip (short window). Called from the
// storage/cd_image modules during accesses.
void notify_access();

// True while inside the "access" window (Pico LED fast-blinking). Used by
// subcode::service to mirror the pattern on the panel /INAC LED.
bool access_busy();

// Force a LED state (transient override: service() recomputes next tick).
void set_led(Led state);

} // namespace ui
