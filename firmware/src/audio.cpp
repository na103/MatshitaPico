// =============================================================================
// MatshitaPico -- Solid-state CD-ROM drive replacement for the Commodore CDTV
// Copyright (c) 2026 Nicola Avanzi (na103)
// Licensed under Creative Commons Attribution 4.0 International (CC BY 4.0).
// See LICENSE.
// =============================================================================
// audio.cpp -- see audio.h.
// Scheme: 2 buffers of 1 sector (588 stereo frames = 1176 32-bit words) ping-pong
// on 2 chained DMA channels (A->B->A->...). service() rearms and fills the buffer
// just consumed. The stream never stops: while idle/paused the buffers are filled
// with zeros (digital silence), so the PIO stays locked to LRCK and the DAC always
// gets a valid frame. If service() is late (e.g. slow SD), the channel restarts on
// the old rewound content: one repeated sector but no out-of-buffer access and no
// loss of alignment.
// =============================================================================
#include "audio.h"
#include "pinmap.h"
#include "cd_image.h"
#include "ram_func.h"
#include "hardware/pio.h"
#include "hardware/dma.h"
#include "hardware/gpio.h"
#include "i2s_slave.pio.h"
#if USB_DEBUG
#include "pico/time.h"
#endif

namespace audio {

#if USB_DEBUG
static uint32_t s_dbg_read_max  = 0;   // max read_raw duration (us) in the window
static uint32_t s_dbg_gap_max   = 0;   // max gap between service() calls (us)
static uint32_t s_dbg_svc_last  = 0;   // timestamp of the last service()
static uint32_t s_dbg_underrun  = 0;   // passes with both channels already complete
static uint32_t s_dbg_read_slow = 0;   // read > 8ms (1 = one-off cold; >1 = throughput)
static uint32_t s_dbg_gap_big   = 0;   // gap > 40ms (53ms buffer at underrun risk)
#endif

// 1 audio sector raw = 2352 bytes = 588 stereo samples = 1176 half-frames/words.
// The DMA buffer is 1 SECTOR: so s_cur_lsn (= audio::current_lsn()) advances by 1
// per IRQ and stays fine-grained. NB: the CD+G subcode reads the R-W packs at
// current_lsn() (subcode.cpp) -> deeper buffers would make it advance in jumps and
// early = desynchronised CD+G graphics. Start-of-play scratch is NOT fought here
// but with cd_image::prewarm() at insert, which pays the cold latency (LINKMAP+XIP)
// while audio is stopped.
static constexpr uint32_t WORDS_PER_SECTOR = cd_image::RAW_SECTOR / 2;

static PIO  s_pio = pio0;
static uint s_sm  = 0;
static int  s_dma[2] = {-1, -1};
static uint32_t s_buf[2][WORDS_PER_SECTOR];

// State: Playing = sectors still to convert; Draining = last sector already in the
// DMA buffers but not yet played (2 refills ~ 26 ms); Idle = stopped. playing()
// stays true through Draining: PLAY_COMPLETE/STCH fire at the end of AUDIBLE audio,
// not when the last sector enters the queue.
enum class St { Idle, Playing, Draining };
static St       s_st     = St::Idle;
static int      s_drain  = 0;      // silence refills left before Idle
static bool     s_paused = false;
static uint32_t s_cur_lsn = 0;     // next LSN to convert
static uint32_t s_end_lsn = 0;
static uint8_t  s_raw[cd_image::RAW_SECTOR];

// Convert a raw sector (16-bit LE PCM, L R L R ...) to PIO words: 24-bit value
// sign-extended into [31:8] (see i2s_slave.pio).
static void RAM_FUNC(sector_to_words)(const uint8_t* raw, uint32_t* dst) {
    for (uint32_t i = 0; i < WORDS_PER_SECTOR; ++i) {
        int16_t smp = (int16_t)((uint16_t)raw[2*i] | ((uint16_t)raw[2*i+1] << 8));
        dst[i] = (uint32_t)((int32_t)smp) << 8;
    }
}

static void RAM_FUNC(fill_silence)(uint32_t* dst) {
    for (uint32_t i = 0; i < WORDS_PER_SECTOR; ++i) dst[i] = 0;
}

// Read one audio sector from the file (timing the cold read in dbg). Uses the
// dedicated AUDIO file cache (read_raw_audio): on a multi-file CUE it does not
// evict the data bin -> no LINKMAP rebuild (~80 ms) in here. See storage.h.
static size_t RAM_FUNC(read_one)() {
#if USB_DEBUG
    uint32_t t0 = time_us_32();
    size_t got = cd_image::read_raw_audio(s_raw, 1);
    uint32_t dt = time_us_32() - t0;
    if (dt > s_dbg_read_max) s_dbg_read_max = dt;
    if (dt > 8000) s_dbg_read_slow++;
    return got;
#else
    return cd_image::read_raw_audio(s_raw, 1);
#endif
}

// Fill a buffer: next sector if playing, else silence.
static void RAM_FUNC(refill)(uint32_t* dst) {
    if (s_st == St::Playing && !s_paused) {
        if (s_cur_lsn < s_end_lsn &&
            read_one() == cd_image::RAW_SECTOR) {
            sector_to_words(s_raw, dst);
            s_cur_lsn++;
            if (s_cur_lsn >= s_end_lsn) { s_st = St::Draining; s_drain = 2; }
            return;
        }
        // end of range / end of file / error: drain what is already queued
        s_st = St::Draining; s_drain = 2;
    }
    if (s_st == St::Draining && --s_drain <= 0)
        s_st = St::Idle;                       // the queued buffers have been played
    fill_silence(dst);
}

void init() {
    // BCLK/LRCK from the CPLD: inputs observed ONLY by the PIO 'wait gpio', nothing
    // else configures them. On RP2350 (!= RP2040) unconfigured pads stay ISOLATED/
    // input-disabled: without gpio_init the PIO reads a fixed level and the SM
    // hangs at the first wait -> DMA never completes -> refill() never called ->
    // s_cur_lsn frozen (player timer stuck) and AUDIO MUTE.
    gpio_init(PIN_BCLK); gpio_set_dir(PIN_BCLK, GPIO_IN);
    gpio_init(PIN_LRCK); gpio_set_dir(PIN_LRCK, GPIO_IN);

    // A_EMPH: output, de-emphasis off
    gpio_init(PIN_A_EMPH);
    gpio_set_dir(PIN_A_EMPH, GPIO_OUT);
    gpio_put(PIN_A_EMPH, 0);

    // PIO: program the I2S slave on A_DATA (the 'wait' uses the absolute BCLK/LRCK GPIOs)
    uint offset = pio_add_program(s_pio, &i2s_slave_program);
    s_sm = pio_claim_unused_sm(s_pio, true);
    i2s_slave_program_init(s_pio, s_sm, offset, PIN_A_DATA);

    // 2 ping-pong DMA channels, paced by the PIO TX FIFO
    s_dma[0] = dma_claim_unused_channel(true);
    s_dma[1] = dma_claim_unused_channel(true);
    for (int i = 0; i < 2; ++i) {
        fill_silence(s_buf[i]);
        dma_channel_config c = dma_channel_get_default_config(s_dma[i]);
        channel_config_set_transfer_data_size(&c, DMA_SIZE_32);
        channel_config_set_read_increment(&c, true);
        channel_config_set_write_increment(&c, false);
        channel_config_set_dreq(&c, pio_get_dreq(s_pio, s_sm, /*is_tx=*/true));
        channel_config_set_chain_to(&c, s_dma[1 - i]);
        dma_channel_configure(s_dma[i], &c, &s_pio->txf[s_sm], s_buf[i],
                              WORDS_PER_SECTOR, false);
        // IRQ0 flag enabled only for POLLING via ints0 (no NVIC)
        dma_channel_set_irq0_enabled(s_dma[i], true);
    }
    dma_channel_start(s_dma[0]);
}

// Suspend the stream during an SD block while Idle: stop the SM -> the TX FIFO
// fills and the DMA stalls by backpressure. So a main loop blocked for a long time
// (cold prewarm/load) does NOT complete the channels without service() = no chain
// re-trigger past the buffer (the "walk" that glitched at disc start). With the SM
// stopped the PIO stalls on 'out' holding A_DATA=0 = silence.
void pause_stream() {
    if (s_dma[0] < 0) return;                 // not yet initialised
    pio_sm_set_enabled(s_pio, s_sm, false);
}

// Resume after the block. While Idle the content is all silence (zeros): the SM
// restarts from its PC and re-aligns to the frame on the first LRCK 'wait gpio';
// any half-word straddle is zero anyway -> no audible artefact.
void resume_stream() {
    if (s_dma[0] < 0) return;
    pio_sm_set_enabled(s_pio, s_sm, true);
}

void play(uint32_t start_lsn, uint32_t end_lsn) {
    if (end_lsn <= start_lsn) return;
    cd_image::seek(start_lsn);
    s_cur_lsn = start_lsn;
    s_end_lsn = end_lsn;
    s_paused  = false;
    s_st      = St::Playing;
    // de-emphasis from the CONTROL of the start track (bit 0x01 = pre-emphasis)
    const cd_image::Track* tr = cd_image::track_for_lsn(start_lsn);
    gpio_put(PIN_A_EMPH, tr && (tr->control & 0x01));
}

void pause(bool p) { s_paused = p; }

void stop() {
    s_st     = St::Idle;
    s_paused = false;
    gpio_put(PIN_A_EMPH, 0);
}

bool playing() { return s_st != St::Idle; }

uint32_t current_lsn() { return s_cur_lsn; }

#if USB_DEBUG
void dbg_reset() {
    s_dbg_read_max = 0; s_dbg_gap_max = 0; s_dbg_underrun = 0; s_dbg_svc_last = 0;
    s_dbg_read_slow = 0; s_dbg_gap_big = 0;
}
uint32_t dbg_read_max_us() { return s_dbg_read_max; }
uint32_t dbg_gap_max_us()  { return s_dbg_gap_max; }
uint32_t dbg_underruns()   { return s_dbg_underrun; }
uint32_t dbg_read_slow()   { return s_dbg_read_slow; }
uint32_t dbg_gap_big()     { return s_dbg_gap_big; }
#endif

void RAM_FUNC(service)() {
#if USB_DEBUG
    {
        uint32_t now = time_us_32();
        if (s_dbg_svc_last) {
            uint32_t gap = now - s_dbg_svc_last;
            if (gap > s_dbg_gap_max) s_dbg_gap_max = gap;
            if (gap > 40000) s_dbg_gap_big++;
        }
        s_dbg_svc_last = now;
        // BOTH channels already complete = service missed >=1 pass (it was late)
        if (dma_channel_get_irq0_status(s_dma[0]) &&
            dma_channel_get_irq0_status(s_dma[1]))
            s_dbg_underrun++;
    }
#endif
    // A channel finished? Rewind IMMEDIATELY (so a chain re-trigger cannot leave
    // the buffer), then fill with the next sector/silence.
    for (int i = 0; i < 2; ++i) {
        if (dma_channel_get_irq0_status(s_dma[i])) {
            dma_channel_acknowledge_irq0(s_dma[i]);
            dma_channel_set_read_addr(s_dma[i], s_buf[i], false);
            refill(s_buf[i]);
        }
    }
}

} // namespace audio
