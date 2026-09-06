// =============================================================================
// MatshitaPico -- Solid-state CD-ROM drive replacement for the Commodore CDTV
// Copyright (c) 2026 Nicola Avanzi (na103)
// Licensed under Creative Commons Attribution 4.0 International (CC BY 4.0).
// See LICENSE.
// =============================================================================
// ui.cpp -- UI GPIO glue (the pure logic is in ui_logic.h).
// NEXT button on GP3 (active low, internal pull-up); status LED on GP25 onboard.
// Disc change via cd_image + cdtv::media_eject/insert (+ STCH).
// =============================================================================
#include "ui.h"
#include "pinmap.h"
#include "cd_image.h"
#include "cdtv_drive.h"
#include "storage.h"
#include "audio.h"
#include "pico/stdlib.h"
#include "hardware/gpio.h"
#include <cstdio>      // button-event log over USB (debug/bring-up)

namespace ui {

static ButtonFsm s_btn;
static uint32_t  s_discchange_until = 0;   // "double flash" disc-change window
static uint32_t  s_access_until     = 0;   // "blip" SD-access window
static Led       s_forced           = Led::Ready;
static bool      s_has_forced       = false;

static inline uint32_t now_ms() {
    return to_ms_since_boot(get_absolute_time());
}

// --- button sampling in a TIMER IRQ -------------------------------------------
// Sampling in the main loop lost presses during data activity: sector reads block
// the loop for ms, and long seeks in a large BIN walk FatFs' FAT chain (up to
// hundreds of ms) -> if press+release fall inside the stall, the FSM sees nothing.
// Here the FSM runs in a 5 ms repeating timer (IRQ context, alive even with the
// loop blocked) and enqueues events; ui::service() consumes them in the main loop,
// where the ACTIONS (disc change/eject touch the SD via FatFs, not reentrant) are safe.
static repeating_timer_t   s_btn_timer;
static volatile uint8_t    s_ev_short = 0;   // pending events (counters: no loss)
static volatile uint8_t    s_ev_long  = 0;

static bool btn_timer_cb(repeating_timer_t*) {
    BtnEvent ev = s_btn.update(now_ms(), gpio_get(PIN_NEXT_BTN) == 0);
    if      (ev == BtnEvent::Short) { if (s_ev_short < 255) s_ev_short++; }
    else if (ev == BtnEvent::Long)  { if (s_ev_long  < 255) s_ev_long++;  }
    return true;
}

void init() {
    gpio_init(PIN_NEXT_BTN);
    gpio_set_dir(PIN_NEXT_BTN, GPIO_IN);
    gpio_pull_up(PIN_NEXT_BTN);            // button to GND
    gpio_init(PIN_LED);
    gpio_set_dir(PIN_LED, GPIO_OUT);
    gpio_put(PIN_LED, 0);
    s_btn.reset(now_ms());
    // -5 ms = fixed cadence (from the start, not from the callback end)
    add_repeating_timer_ms(-5, btn_timer_cb, nullptr, &s_btn_timer);
}

void set_led(Led state) { s_forced = state; s_has_forced = true; }
void notify_access()    { s_access_until = now_ms() + 80; }
bool access_busy()      { return (int32_t)(s_access_until - now_ms()) > 0; }

// --- disc-change actions (timed disc-change cycle) ----------------------------
// The CDTV driver (cdtv.device) mounts ONLY on a bit6(media) TRANSITION between two
// poll 0x81 armed by the ROM: its shadow starts at 0x60 and the observed-bit mask
// is 0x70. A single "insert" STCH is not enough: at the poll the host would already
// see media=1 without ever having recorded no-media -> no bit6 change vs shadow ->
// no mount. FIX: TWO STCHs spaced in TIME -> #1 with a no-media status (aligns the
// shadow to bit6=0), then, after the host has completed the intermediate poll, #2
// with media (= the transition that arms the mount).
static int      s_dc_idx       = -1;    // index to insert at step 2 (-1 = none)
static uint32_t s_dc_insert_at = 0;     // time (ms) to emit STCH#2 + media

// Duration of the "no disc" state between eject and insert. Two roles:
//  1) technical (min ~50 ms): the host must complete a poll armed at no-media to
//     bring the shadow to bit6=0 (see above);
//  2) UX: the disc change must LOOK like a disc change -- with a 50 ms gap the audio
//     player switched to the next disc instantly without ever showing the eject (the
//     host polls every ~150 ms: the no-media was not even seen). With a long gap the
//     host records the eject (the player shows the eject ANIMATION, the /INAC LED
//     goes off), then the insert arrives like a real caddy change. 3000 ms tuned on
//     hardware (50 ms invisible, 2000 the animation did not complete, 6000 too long).
// NB on the DATA disc path the CDTV often REBOOTS already on the eject: the Pico is
// reset during the gap (rev.1 _RESET->RUN) and the insert is done by the post-boot
// auto-insert (the index is saved FIRST, see below) -> the long gap changes nothing there.
static const uint32_t DC_GAP_MS = 3000;

// Step 1: bring the driver shadow to a NO-MEDIA baseline (eject + STCH#1) and load
// the TOC; step 2 (insert + STCH#2 = media) starts after DC_GAP_MS in
// service_diskchange(). Robust: if the shadow was already no-media the eject is a
// no-op; if it was media the eject flips it. Either way the insert sees the bit6
// transition -> read-TOC -> mount.
static void begin_diskchange(int idx) {
    int n = cd_image::count();
    if (n <= 0) return;
    idx = ((idx % n) + n) % n;            // wrap-around
    // SD OPS OFF THE AUDIO PATH: load()+save() block the main loop and, if they fall
    // while audio is playing (or fading), the 26 ms queue drains and repeats = a
    // "tick"/scratch on NEXT (a pure eject, which does no SD, is clean). load() is
    // the bottleneck (reads+parses the cold .cue, ~15-20 ms). So:
    //   - save_last_disc: NOW (short block, absorbed by the queue). Kept here because
    //     in rev.1 the CDTV resets the Pico on disc change -> persistence must already
    //     be written.
    //   - media_eject: STCH#1 no-media + stop->fade = LAST op, no SD block after it.
    //   - load()+prewarm(): DEFERRED to service_diskchange (after the gap), when audio
    //     has been silent for a while -> their block is harmless.
    cdtv::media_eject();                  // stop->fade FIRST (audio drops in volume at
                                          // once -> the SD block that follows is not
                                          // heard repeating the queue at full volume)
    storage::save_last_disc(idx);         // save AFTER (during the fade): harmless block,
                                          // and written promptly for rev.1 persistence
    s_dc_idx       = idx;                 // load+prewarm+insert (STCH#2) at the gap
    s_dc_insert_at = now_ms() + DC_GAP_MS;
    s_discchange_until = now_ms() + 1000; // double-flash feedback
    printf("NEXT: disc [%d] %s (eject no-media + %lums + load/insert = bit6 transition -> mount)\n",
           idx, storage::cue_name(idx), (unsigned long)DC_GAP_MS);
}

// Step 2: after the gap (audio now silent), load the new disc's TOC, warm the file,
// and insert (media + STCH#2 = mount). Called by service().
static void service_diskchange(uint32_t t) {
    if (s_dc_idx >= 0 && (int32_t)(s_dc_insert_at - t) <= 0) {
        int idx = s_dc_idx;
        s_dc_idx = -1;
        // Suspend the I2S stream around the cold SD reads (load+prewarm): while Idle
        // the block would underrun the ping-pong DMA = "tick" at disc start. With the
        // SM stopped the DMA halts by backpressure = silence.
        audio::pause_stream();
        bool ok = cd_image::load(idx);    // load NOW: audio silent -> harmless block
        if (ok) cd_image::prewarm();      // warm the file (LINKMAP+XIP) before the 1st play
        audio::resume_stream();
        if (ok) {
            cdtv::media_insert();         // s.media=true + STCH#2: the TRANSITION arms the mount
            printf("NEXT: insert disc [%d] (STCH#2 media -> mount)\n", idx);
        } else {
            printf("NEXT: disc [%d] LOAD FAILED (%s) -> stays ejected\n",
                   idx, storage::cue_name(idx));
        }
    }
}

static void eject() {                     // long press
    s_dc_idx = -1;                        // cancel any disc-change cycle in progress
    cdtv::media_eject();
    storage::save_last_disc(-1);          // persistence: no auto-insert at next boot
    s_discchange_until = 0;
    printf("EJECT: media removed (LED off; short press = insert the next)\n");
}

// --- current LED state (priority: error > change > access > media) ------------
static Led current_led(uint32_t t) {
    if (cd_image::count() <= 0)              return Led::Error;     // no image
    if ((int32_t)(s_discchange_until - t) > 0) return Led::DiscChange;
    if ((int32_t)(s_access_until - t) > 0)     return Led::Access;
    return cdtv::state().media ? Led::Ready : Led::Ejected;
}

void service() {
    uint32_t t = now_ms();

    service_diskchange(t);   // step 2 of the disc-change cycle (STCH#2 + media) after the gap

    // button: events enqueued by the timer IRQ (see btn_timer_cb), actions HERE in
    // the main loop (they touch the SD). One event per pass: the loop runs at ~kHz,
    // so the queue never really accumulates.
    if (s_ev_long) {
        s_ev_long = 0; s_ev_short = 0;    // eject also cancels pending shorts
        eject();
    } else if (s_ev_short) {
        s_ev_short--;
        // disc-change cycle eject->gap->insert (both hot-swap and re-insert after
        // eject): the no-media->media transition is always needed to arm the mount.
        begin_diskchange(cd_image::current_index() + 1);
    }

    // LED
    Led st = current_led(t);
    if (s_has_forced && st == Led::Ready) { st = s_forced; }  // override only from idle
    s_has_forced = false;
    gpio_put(PIN_LED, led_on(st, t) ? 1 : 0);
}

} // namespace ui
