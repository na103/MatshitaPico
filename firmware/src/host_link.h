// =============================================================================
// MatshitaPico -- Solid-state CD-ROM drive replacement for the Commodore CDTV
// Copyright (c) 2026 Nicola Avanzi (na103)
// Licensed under Creative Commons Attribution 4.0 International (CC BY 4.0).
// See LICENSE.
// =============================================================================
// host_link.h -- Link layer to the CPLD (host_if.v contract).
// Pico side of the register protocol towards the CPLD:
//   - receives command bytes (CMD_STB window ~2.8 us, byte on the SD[] bus),
//     captured on a GPIO IRQ and queued in a ring buffer (the main loop pops)
//   - supplies output bytes on request (BYTE_REQ + CHAN, also on IRQ), driving
//     the bus only for the duration of the BYTE_VLD pulse
//   - raises ST_AVAIL / DAT_AVAIL / STCH_REQ per the drive state.
// The first byte of a reply must be PUSHED (send_byte) as soon as it is ready:
// the CPLD asserts STEN/DRQ only with a fresh byte loaded (out_valid, per-byte).
// See docs/protocol-cdtv.md and hdl/host_if.v.
// =============================================================================
#pragma once
#include <cstdint>
#include <cstddef>

namespace host_link {

// Initialise the data-bus and handshake GPIOs. The SD[] bus starts as INPUT (the
// CPLD drives it for commands); we drive it only for status/data.
void init();

// Raw data-bus access (gather/scatter over the non-contiguous GPIOs).
uint8_t  bus_read();           // read SD[7:0] (while the CPLD drives them)
void     bus_write(uint8_t v); // drive SD[7:0]
void     bus_release();        // return the bus to high-Z (input)

// --- status lines to the CPLD (level) ----------------------------------------
void set_status_avail(bool v); // ST_AVAIL  -> STEN
void set_data_avail(bool v);   // DAT_AVAIL -> DRQ

// STCH is level-driven (like the real drive: assert held until the host completes
// a command; WinUAE: stch=0 on cdrom_command_done). assert_stch() raises STCH_REQ;
// the main loop calls release_stch() when a command byte arrives and service_stch()
// each pass for a safety cap (20 ms) if the host does not respond. pulse_stch() is
// a legacy alias of assert.
void assert_stch();
void release_stch();
void service_stch();
void pulse_stch();             // alias: assert_stch()

// --- inbound events from the CPLD (captured on IRQ, popped from the ring) -----
// Returns true and writes *byte when a new command byte arrived (CMD_STB).
bool poll_cmd_byte(uint8_t* byte);

// Returns true when the CPLD requests the next output byte (BYTE_REQ); *is_data
// selects the channel (CHAN: false=status, true=data).
bool poll_byte_req(bool* is_data);

// True if a command byte is queued but not yet consumed (non-destructive peek).
// Used to NOT push data bytes while a command has arrived: the CPLD blocks pushes
// during the command window (~2.8 us) and the byte would be lost (top-up guard).
bool cmd_pending();

// Load an output byte into the CPLD: drive the bus, BYTE_VLD pulse, release.
// (the CPLD latches on the edge; the bus stays free between bytes)
void send_byte(uint8_t v);

// Diagnostics: monotonic counters of captured IRQ events (ring heads). If they
// advance while the main loop logs no rx/rd, the IRQ is alive but the loop is not
// (or vice versa): distinguishes "mute host" from "deaf/dead firmware".
uint32_t cmd_events();   // total CMD_STB edges captured
uint32_t req_events();   // total BYTE_REQ edges captured

} // namespace host_link
