// =============================================================================
// MatshitaPico -- Solid-state CD-ROM drive replacement for the Commodore CDTV
// Copyright (c) 2026 Nicola Avanzi (na103)
// Licensed under Creative Commons Attribution 4.0 International (CC BY 4.0).
// See LICENSE.
// =============================================================================
// subcode.h -- Subcode Q channel generation (A_SUBQ) to the CPLD.
// The Pico emits ONLY the Q data stream on A_SUBQ (1 line); the CPLD generates
// SCCK/SBCP/SCOR/EFFK (hdl/MatshitaPico_top.v: SCCK=LRCK/6, block=98 SCCK=75 Hz)
// and returns the block phase to the Pico on FRAME/GP6 (= SCOR). The subq_tx PIO
// serialises a 12-byte packet per block, locked to SCOR/LRCK. The Q content
// (ADR=1: track/index/MSF rel+abs in BCD + CRC) follows the current audio
// position (audio::current_lsn()), like the real drive.
// =============================================================================
#pragma once
#include <cstdint>

namespace subcode {

// Initialise the subq_tx PIO on A_SUBQ (sync on FRAME=SCOR + LRCK from the CPLD).
void init();

// Call from the loop: build the Q packet for the current position (playing ->
// real position; stopped -> last known LSN) and keep the PIO TX FIFO full (one
// packet per block, 75 Hz).
void service();

// CD+G diagnostics: monotonic counters of successful/failed .sub reads
// (incremented only while playing with a .sub present). For the USB_DEBUG heartbeat.
uint32_t sub_ok();
uint32_t sub_fail();

} // namespace subcode
