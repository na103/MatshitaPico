// =============================================================================
// MatshitaPico -- Solid-state CD-ROM drive replacement for the Commodore CDTV
// Copyright (c) 2026 Nicola Avanzi (na103)
// Licensed under Creative Commons Attribution 4.0 International (CC BY 4.0).
// See LICENSE.
// =============================================================================
// subcode.cpp -- see subcode.h (CD+G capable).
// The subpw_tx PIO consumes a CONTINUOUS stream of P..W bytes (98 per block, see
// subpw_block.h), 24.5 32-bit words per block at 75 Hz (~1840 words/s). The TX
// FIFO (8 words = ~4.3 ms) must be kept full by service() in the main loop; if it
// drains, the SM stalls on 'out' and loses phase -> we detect TXSTALL (FDEBUG) and
// RESTART the SM from the program start (resync on SCOR) with a fresh stream
// aligned to block+word. The stall is also the intended mechanism for pauses (LED
// gating, ejected). Position: while playing it follows audio::current_lsn(); when
// stopped it repeats the last known position. R..W (CD+G graphics): from the
// CloneCD .sub, ONLY while playing (the real drive emits subcode only with the
// disc spinning). Transmission stays GATED to mirror the Pico LED onto the panel
// /INAC LED: see the comment in service().
// =============================================================================
#include "subcode.h"
#include "subq_packet.h"
#include "subpw_block.h"
#include "pinmap.h"
#include "audio.h"
#include "cd_image.h"
#include "cdtv_drive.h"
#include "ui.h"
#include "hardware/pio.h"
#include "hardware/gpio.h"
#include "pico/time.h"
#include "subq_tx.pio.h"
#include <cstring>

namespace subcode {

static PIO  s_pio = pio1;          // pio0 is the audio's (separate programs)
static uint s_sm  = 0;
static uint s_offset = 0;

// current wire block + consume position (98 = exhausted, build the next)
static uint8_t s_blk[subpw::BLOCK_BYTES];
static int     s_blk_pos = subpw::BLOCK_BYTES;
static uint8_t s_sub[subpw::SUB_BYTES];

// diagnostic counters (see subcode.h)
static uint32_t s_sub_ok = 0, s_sub_fail = 0;
uint32_t sub_ok()   { return s_sub_ok; }
uint32_t sub_fail() { return s_sub_fail; }

void init() {
    // FRAME/SCOR (GP6): input observed only by the PIO 'wait gpio' -- on RP2350 the
    // pad must be enabled explicitly (see the note in audio.cpp; LRCK GP7, used
    // here too, is initialised by audio::init).
    gpio_init(PIN_FRAME); gpio_set_dir(PIN_FRAME, GPIO_IN);

    s_offset = pio_add_program(s_pio, &subpw_tx_program);
    s_sm = pio_claim_unused_sm(s_pio, true);
    subpw_tx_program_init(s_pio, s_sm, s_offset, PIN_A_SUBQ);
}

// Restart the SM from the program start (sync on SCOR) with a fresh stream: after
// ANY stall (empty FIFO) the byte/LRCK phase is lost and must be reacquired.
static void restart_sm() {
    pio_sm_set_enabled(s_pio, s_sm, false);
    pio_sm_clear_fifos(s_pio, s_sm);
    pio_sm_restart(s_pio, s_sm);                       // clear OSR/shift counters
    pio_sm_exec(s_pio, s_sm, pio_encode_jmp(s_offset)); // restart from the SCOR sync
    s_pio->fdebug = 1u << (PIO_FDEBUG_TXSTALL_LSB + s_sm);  // clear sticky
    pio_sm_set_enabled(s_pio, s_sm, true);
    s_blk_pos = subpw::BLOCK_BYTES;                    // fresh stream = fresh block
}

// Build the next wire block (Q position packet + R-W from the .sub).
static void build_block() {
    // Position = audio::current_lsn() (the position of the AUDIO being played).
    // During a CD+G play the R-W graphics MUST follow the audio, so the R-W packs
    // are read from the correct sector. Sherlock-safety is handled entirely by the
    // per-disc gate in service() (mixed discs get no subcode at all).
    uint32_t lsn = audio::current_lsn();
    const cd_image::Track* tr = cd_image::track_for_lsn(lsn);

    uint8_t  control = tr ? (tr->control & 0x0f) : 0;
    uint8_t  track   = tr ? tr->number : 0;
    uint8_t  index   = (tr && lsn < tr->start_lsn) ? 0 : 1;   // 0 = pregap
    uint32_t rel     = (tr && lsn >= tr->start_lsn) ? lsn - tr->start_lsn
                     : (tr ? tr->start_lsn - lsn : 0);        // pregap counts down

    uint8_t pkt[subq::PACKET_BYTES];
    subq::build_position_packet(control, track, index, rel, lsn, pkt);
    // Q is ALWAYS VALID (faithful to the real drive). Sherlock-safety is the
    // per-disc gate in service() (cd_image::subcode_wanted): on data/mixed discs
    // the subcode is not transmitted at all, so a valid Q reaches only where it is
    // wanted. Corruptions below are opt-in TEST options.
#if defined(SUBCODE_QNEVER)
    pkt[10] ^= 0x55; pkt[11] ^= 0x55;                              // TEST: CRC always broken
#elif defined(SUBCODE_QPLAYONLY)
    if (!audio::playing()) { pkt[10] ^= 0x55; pkt[11] ^= 0x55; }   // TEST: valid only in play
#endif

    // R..W: CD+G graphics from the CloneCD .sub, only while playing (like the real
    // drive, which emits subcode only with the disc spinning) and if the rip has it.
    const uint8_t* sub = nullptr;
    if (audio::playing() && cd_image::has_sub()) {
        if (cd_image::read_sub(lsn, s_sub)) { sub = s_sub; ++s_sub_ok; }
        else                                ++s_sub_fail;
    }

    subpw::build_wire_block(pkt, sub, s_blk);
#if defined(SUBCODE_ROT) && (SUBCODE_ROT != 0)
    // TEST: rotate the stream by ROT frames relative to SCOR, to compensate a
    // block offset in the PIO->CPLD->motherboard chain (cyclic stream: no CPLD
    // change). ROT=+1 / -1 (=97).
    {
        uint8_t tmp[subpw::BLOCK_BYTES];
        constexpr int R = ((SUBCODE_ROT % subpw::BLOCK_BYTES) + subpw::BLOCK_BYTES)
                          % subpw::BLOCK_BYTES;
        for (int i = 0; i < subpw::BLOCK_BYTES; ++i)
            tmp[i] = s_blk[(i + R) % subpw::BLOCK_BYTES];
        memcpy(s_blk, tmp, subpw::BLOCK_BYTES);
    }
#endif
#ifdef SUBCODE_QZERO
    // TEST: data channel all ZERO (S0/S1 intact) = "silence with structure".
    for (int i = 0; i < 96; ++i) s_blk[i] = 0;
#endif
    s_blk_pos = 0;
}

void service() {
#ifdef SUBCODE_MUTE
    // TEST build: subcode transmission fully off (SBCP mute). Side effects: /INAC
    // LED off, player display frozen, no CD+G. Only to check whether a crash comes
    // from the app consuming the subcode.
    return;
#endif
    // Gating the transmission = MIRRORING THE PICO LED: the CPLD drives /INAC (the
    // CD-ROM LED on the VFD panel) with a monostable on A_SUBQ activity, so
    // deciding WHEN to transmit = deciding the LED pattern.
    //   - ejected          -> silence          -> LED off    (Pico: off)
    //   - audio play/pause  -> continuous stream (Q to the player display, CD+G
    //                          R-W) -> LED solid (Pico: solid; real drive: solid)
    //   - data access       -> stream gated ~3 Hz -> LED blinks
    //   - mounted, stopped  -> continuous stream   -> LED solid (Pico: Ready)
    // Every pause stalls the SM: on resume restart_sm() resyncs on SCOR (the block
    // straddling the stall is lost: an invalid-CRC Q the host drops; for CD+G a
    // lost pack = a glitch the pack parity absorbs. The real drive also "dirties"
    // the subcode during pauses). /INAC pattern priority: error > ejected > access
    // > solid.
    uint32_t t = to_ms_since_boot(get_absolute_time());
    if (cd_image::count() <= 0) {
        // ERROR (no image / SD unavailable): 1 Hz blink, like the Pico onboard LED
        // (ui::Led::Error). ON phase = feed the stream (A_SUBQ active -> CPLD
        // monostable -> LED on); OFF phase = do not feed -> SM stalls -> LED off.
        // The content is irrelevant (no disc, no host reads Q): only the wire
        // ACTIVITY matters. build_block with no disc = null packet, harmless.
        if ((t % 1000u) >= 500u) return;        // off phase of the 1 Hz blink
    } else if (!cdtv::state().media) {
        return;                                 // ejected -> LED off
    } else if (!cd_image::subcode_wanted()) {
        // DATA/MIXED disc without CD+G (Sherlock): subcode fully off (= NOSUB for
        // this disc) because ANY subcode traffic crashes it. /INAC stays off on
        // these discs: the price, in firmware. Audio/CD+G: subcode ON.
        return;
#ifndef SUBCODE_NOGATE
    } else if (!audio::playing() && ui::access_busy() && (t % 300u) < 150u) {
        return;                                 // data access -> off phase ~3 Hz
#endif
    }
    // otherwise: mounted/play (solid) or error/access ON phase -> feed

    // did the SM stall (gating pause or slow main loop)? -> resync
    if (s_pio->fdebug & (1u << (PIO_FDEBUG_TXSTALL_LSB + s_sm)))
        restart_sm();

    // keep the TX FIFO full (8 words): 4 stream bytes per word, blocks are built on
    // demand (1 .sub read of 96 B per block, 75 Hz max)
    while (pio_sm_get_tx_fifo_level(s_pio, s_sm) < 8) {
        uint32_t w = 0;
        for (int i = 0; i < 4; ++i) {
            if (s_blk_pos >= subpw::BLOCK_BYTES)
                build_block();
            w = (w << 8) | s_blk[s_blk_pos++];
        }
        pio_sm_put(s_pio, s_sm, w);
    }
}

} // namespace subcode
