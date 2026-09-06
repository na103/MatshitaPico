// =============================================================================
// MatshitaPico -- Solid-state CD-ROM drive replacement for the Commodore CDTV
// Copyright (c) 2026 Nicola Avanzi (na103)
// Licensed under Creative Commons Attribution 4.0 International (CC BY 4.0).
// See LICENSE.
// =============================================================================
// host_link.cpp -- see host_link.h.
// IRQ CAPTURE: the CPLD holds CMD_STB/BYTE_REQ high for a ~2.8 us window
// (host_if.v, CMD_WIN) and drives the command byte on SD[] for the whole window.
// Polling in the main loop cannot work (the loop blocks for milliseconds on SD
// reads): the edges are captured by a GPIO IRQ handler that reads the bus at once
// (latency << window) and enqueues into an SPSC ring buffer; the main loop pops
// them with poll_*(). The fine CN9 bus timing (polarity, 120 ns, per-byte
// DRQ/STEN) is handled by the CPLD.
// =============================================================================
#include "host_link.h"
#include "pinmap.h"
#include "hardware/gpio.h"
#include "hardware/sync.h"
#include "hardware/irq.h"
#include "hardware/structs/io_bank0.h"
#include "hardware/structs/sio.h"
#include "pico/stdlib.h"
#include <initializer_list>

namespace host_link {

// ---- SPSC ring buffers (producer = IRQ, consumer = main loop) ----------------
// Power-of-two sizes. Commands are at most 7 bytes; 16 = ample margin.
static volatile uint8_t  s_cmd_ring[16];
static volatile uint32_t s_cmd_head = 0, s_cmd_tail = 0;   // head=IRQ, tail=main

// BYTE_REQ events: BYTE_REQ is a TOGGLE in the CPLD (one edge per byte consumed:
// the DMAC's word pairs arrive as RISE+FALL ~212 ns apart) -> IRQ on BOTH edges,
// up to 2 events per invocation. Ring of 8. Value = CHAN (0=status, 1=data).
static volatile uint8_t  s_req_ring[8];
static volatile uint32_t s_req_head = 0, s_req_tail = 0;

// EXCLUSIVE ISR IN RAM: an ISR in flash pays cold XIP-cache misses on its first
// invocation, and the first CMD_STB edge of a burst could be served > 4 us late
// (the inter-byte spacing) -> the next edge merges into the sticky GPIO bit ->
// the first byte is lost. Fix: an exclusive RAM handler (__not_in_flash_func) that
// reads/acknowledges the GP21/GP9 INTS directly, with no SDK dispatcher -> short,
// CONSTANT latency independent of the cache.
static constexpr uint32_t CMD_STB_REG   = PIN_CMD_STB / 8;                  // INTS/INTR word
static constexpr uint32_t CMD_STB_MASK  = 0x8u << (4 * (PIN_CMD_STB % 8)); // EDGE_HIGH bit
static constexpr uint32_t BYTE_REQ_REG  = PIN_BYTE_REQ / 8;
static constexpr uint32_t BYTE_REQ_RISE = 0x8u << (4 * (PIN_BYTE_REQ % 8)); // EDGE_HIGH bit
static constexpr uint32_t BYTE_REQ_FALL = 0x4u << (4 * (PIN_BYTE_REQ % 8)); // EDGE_LOW bit

static void __not_in_flash_func(bank0_isr)() {
    io_bank0_irq_ctrl_hw_t* ctl = &io_bank0_hw->proc0_irq_ctrl;   // all on core0
    if (ctl->ints[CMD_STB_REG] & CMD_STB_MASK) {
        io_bank0_hw->intr[CMD_STB_REG] = CMD_STB_MASK; // ack NOW: a later edge re-latches
        // the CPLD drives SD[] for the whole window (~2.8 us): read immediately.
        // Gather inline (bus_read() is in flash: calling it would defeat the RAM ISR).
        uint32_t all = sio_hw->gpio_in;
        uint8_t  b   = 0;
        for (int i = 0; i < 8; ++i)
            if (all & (1u << PIN_SD[i])) b |= (uint8_t)(1u << i);
        uint32_t h = s_cmd_head;
        if (h - s_cmd_tail < sizeof(s_cmd_ring)) {
            s_cmd_ring[h % sizeof(s_cmd_ring)] = b;
            __dmb();
            s_cmd_head = h + 1;
        } // ring full: byte lost (must not happen: main faster than 16 bytes)
    }
    uint32_t rq = ctl->ints[BYTE_REQ_REG] & (BYTE_REQ_RISE | BYTE_REQ_FALL);
    if (rq) {
        io_bank0_hw->intr[BYTE_REQ_REG] = rq;   // ack both pending edges
        uint8_t ch = (uint8_t)((sio_hw->gpio_in >> PIN_CHAN) & 1u);
        // BYTE_REQ is a TOGGLE: each edge = one byte consumed. A DMAC word pair
        // (~212 ns) arrives here as RISE+FALL together (separate sticky bits) = TWO
        // events. CHAN is common to the pair (both data).
        uint32_t n = ((rq & BYTE_REQ_RISE) ? 1u : 0u) + ((rq & BYTE_REQ_FALL) ? 1u : 0u);
        while (n--) {
            uint32_t h = s_req_head;
            if (h - s_req_tail < sizeof(s_req_ring)) {
                s_req_ring[h % sizeof(s_req_ring)] = ch;
                __dmb();
                s_req_head = h + 1;
            }
        }
    }
    // an edge arriving after reading INTS stays pending in the NVIC -> the handler
    // re-enters at once: the single pass loses no events.
}

void init() {
    // data bus: input by default (the CPLD drives it for commands)
    for (uint pin : PIN_SD) {
        gpio_init(pin);
        gpio_set_dir(pin, GPIO_IN);
    }
    // inbound handshake from the CPLD
    for (uint pin : {PIN_CMD_STB, PIN_BYTE_REQ, PIN_CHAN}) {
        gpio_init(pin);
        gpio_set_dir(pin, GPIO_IN);
    }
    // outbound status lines to the CPLD (initial level = inactive)
    for (uint pin : {PIN_BYTE_VLD, PIN_ST_AVAIL, PIN_DAT_AVAIL, PIN_STCH_REQ}) {
        gpio_init(pin);
        gpio_set_dir(pin, GPIO_OUT);
        gpio_put(pin, 0);
    }
    // IRQ: CMD_STB on the rising edge (~2.8 us window from the CPLD); BYTE_REQ on
    // BOTH edges (it is a TOGGLE: one edge per byte consumed). EXCLUSIVE RAM handler
    // (bank0_isr) instead of the SDK dispatcher: gpio_set_irq_enabled only arms the
    // INTE, we install the vector ourselves.
    gpio_set_irq_enabled(PIN_CMD_STB, GPIO_IRQ_EDGE_RISE, true);
    gpio_set_irq_enabled(PIN_BYTE_REQ, GPIO_IRQ_EDGE_RISE | GPIO_IRQ_EDGE_FALL, true);
    irq_set_exclusive_handler(IO_IRQ_BANK0, bank0_isr);
    irq_set_enabled(IO_IRQ_BANK0, true);

    // HIGHEST priority for the capture IRQ. By default IO_IRQ_BANK0 has the SAME
    // priority as USBCTRL_IRQ (PICO_DEFAULT_IRQ_PRIORITY=0x80): equal-priority IRQs
    // do not preempt each other, they queue. With the USB terminal attached (to
    // read the live log) each printf drives CDC traffic -> USBCTRL_IRQ runs; if it
    // is running when the first burst byte's CMD_STB arrives, the capture IRQ WAITS.
    // If that wait exceeds the ~4 us between burst bytes, the 2nd edge merges into
    // the sticky bit -> the first byte is lost. Raising IO_IRQ_BANK0 above USB makes
    // the capture PREEMPT USBCTRL_IRQ -> every edge is served within a few cycles.
    // The handler is minimal (bus read + ring push) -> briefly preempting USB is safe.
    irq_set_priority(IO_IRQ_BANK0, PICO_HIGHEST_IRQ_PRIORITY);
}

// ---- gather/scatter on the data bus (non-contiguous GPIO, see pinmap.h) -------
uint8_t bus_read() {
    uint32_t all = gpio_get_all();
    uint8_t v = 0;
    for (int b = 0; b < 8; ++b)
        if (all & (1u << PIN_SD[b])) v |= (1u << b);
    return v;
}

void bus_write(uint8_t v) {
    uint32_t set = 0;
    for (int b = 0; b < 8; ++b)
        if ((v >> b) & 1) set |= (1u << PIN_SD[b]);
    gpio_set_dir_out_masked(SD_BUS_MASK);
    gpio_put_masked(SD_BUS_MASK, set);
}

void bus_release() {
    gpio_set_dir_in_masked(SD_BUS_MASK);
}

// ---- status lines ------------------------------------------------------------
void set_status_avail(bool v) { gpio_put(PIN_ST_AVAIL, v); }
void set_data_avail(bool v)   { gpio_put(PIN_DAT_AVAIL, v); }

// STCH is LEVEL-driven (replica of the real drive): captures of the original drive
// show the assert held ~3.4 ms and released when the host completes the poll
// command; WinUAE (cdtv.cpp hsync): "if (cdrom_command_done) { sten=1; stch=0; }" =
// a level cleared on command completion. CN9 polarity = ACTIVE HIGH (see host_if.v,
// n_stch <= stc_sync). The main loop calls release_stch() on the first command byte
// received (the poll servicing the event) and service_stch() as a 20 ms safety cap
// if the host stays silent.
static bool            s_stch_active = false;
static absolute_time_t s_stch_deadline;

void assert_stch() {
    gpio_put(PIN_STCH_REQ, 1);
    s_stch_active   = true;
    s_stch_deadline = make_timeout_time_ms(20);
}

void release_stch() {
    if (s_stch_active) {
        gpio_put(PIN_STCH_REQ, 0);
        s_stch_active = false;
    }
}

void service_stch() {
    if (s_stch_active && time_reached(s_stch_deadline))
        release_stch();
}

void pulse_stch() { assert_stch(); }   // alias for legacy callers (ui, main)

// ---- events from the CPLD (pop from the IRQ-filled rings) ---------------------
bool poll_cmd_byte(uint8_t* byte) {
    if (s_cmd_tail == s_cmd_head) return false;
    *byte = s_cmd_ring[s_cmd_tail % sizeof(s_cmd_ring)];
    __dmb();
    s_cmd_tail = s_cmd_tail + 1;
    return true;
}

bool poll_byte_req(bool* is_data) {
    if (s_req_tail == s_req_head) return false;
    *is_data = s_req_ring[s_req_tail % sizeof(s_req_ring)] != 0;
    __dmb();
    s_req_tail = s_req_tail + 1;
    return true;
}

bool cmd_pending() { return s_cmd_tail != s_cmd_head; }

uint32_t cmd_events() { return s_cmd_head; }
uint32_t req_events() { return s_req_head; }

void send_byte(uint8_t v) {
    // The CPLD latches out_byte on the EDGE of BYTE_VLD (2-FF synchronised,
    // ~120 ns): hold the bus stable for the pulse duration, then release at once
    // (the byte is in the CPLD register). The capture IRQ is at highest priority
    // and reads immediately, so it is NOT masked around this window (masking it
    // delayed opcode capture past the ~4 us between bytes -> opcode corruption).
    bus_write(v);
    gpio_put(PIN_BYTE_VLD, 1);
    busy_wait_us(1);
    gpio_put(PIN_BYTE_VLD, 0);
    busy_wait_us(1);          // margin: CPLD synchroniser + fall time
    bus_release();
}

} // namespace host_link
