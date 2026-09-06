// =============================================================================
// MatshitaPico -- Solid-state CD-ROM drive replacement for the Commodore CDTV
// Copyright (c) 2026 Nicola Avanzi (na103)
// Licensed under Creative Commons Attribution 4.0 International (CC BY 4.0).
// See LICENSE.
// =============================================================================
// audio.h -- I2S-slave audio transmitter (A_DATA) to the CPLD / LC7883M DAC.
// The CPLD derives BCLK/LRCK from C16M (hdl/clkgen.v); the Pico shifts samples
// synchronised to those clocks via PIO ('wait gpio'). Frame format: 48 BCLK per
// frame, 24-bit half-frames = sign-extend(8) + 16-bit sample MSB-first, data on
// BCLK high, LRCK high = left. Pipeline: cd_image::read_raw -> 24-bit words ->
// ping-pong DMA -> PIO TX FIFO. The PIO is always fed (silence = zeros) so frame
// alignment is never lost. See pio/i2s_slave.pio.
// =============================================================================
#pragma once
#include <cstdint>
#include <cstddef>

namespace audio {

// Set up PIO + DMA for the I2S slave on A_DATA and start the stream (silence).
void init();

// Play the LSN range [start, end). Samples are read from the BIN (cd_image) and
// queued to the PIO. Also sets A_EMPH from the start track's CONTROL flag.
void play(uint32_t start_lsn, uint32_t end_lsn);
void pause(bool p);
void stop();

// True while still playing (drives cd_playing / audio_status).
bool playing();

// Current playback LSN (for SubQ 0x87 and the subcode Q channel).
uint32_t current_lsn();

// Call from the loop: refills the DMA buffers from the file's sectors.
void service();

// Suspend/resume the I2S stream around a BLOCKING SD op while Idle (mount/scan/
// load/prewarm): stopping the PIO SM fills the TX FIFO and stalls the DMA by
// backpressure, so no channel completes and the chain never re-triggers past the
// buffer end (the "walk" = tick at disc start). With the FIFO stalled the PIO
// holds A_DATA at its last level (0 = clean silence). Use ONLY while Idle: during
// playback it would freeze the last sample (click).
void pause_stream();
void resume_stream();

#if USB_DEBUG
// Instrumentation for the first-play "scratch": measures how late service()
// arrives (main-loop starvation) and how long a cold read costs. Call
// dbg_reset() when a play starts; read the getters afterwards.
void     dbg_reset();
uint32_t dbg_read_max_us();   // max read_raw duration in the window
uint32_t dbg_gap_max_us();    // max gap between service() calls
uint32_t dbg_underruns();     // passes where BOTH channels were already complete
uint32_t dbg_read_slow();     // reads > 8 ms (1 = one-off cold latency; >1 = throughput)
uint32_t dbg_gap_big();       // gaps > 40 ms (53 ms buffer at underrun risk)
#endif

} // namespace audio
