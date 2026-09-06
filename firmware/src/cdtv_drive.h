// =============================================================================
// MatshitaPico -- Solid-state CD-ROM drive replacement for the Commodore CDTV
// Copyright (c) 2026 Nicola Avanzi (na103)
// Licensed under Creative Commons Attribution 4.0 International (CC BY 4.0).
// See LICENSE.
// =============================================================================
// cdtv_drive.h -- Drive state machine + command dispatch.
// Ported from winuae-ref/cdtv.cpp (lines ~532-700) and docs/protocol-cdtv.md.
// Holds the drive state flags (cd_*), assembles the variable-length commands,
// executes them and produces the output (status) bytes.
// =============================================================================
#pragma once
#include <cstdint>
#include <cstddef>

namespace cdtv {

// Drive state (matches the cd_* flags of cdtv.cpp / protocol status byte).
struct DriveState {
    bool media   = false;  // bit6 0x40: media present
    bool motor   = false;  // bit5 0x20: motor on
    bool error   = false;  // bit4 0x10: error
    bool finished = false; // bit3 0x08: operation finished
    bool playing = false;  // bit2 0x04: audio playback in progress
    bool ready   = true;   // bit0 0x01 inverted: !ready => NOT ready

    uint16_t sector_size = 2048;   // mode set (0x84) / mode sense (0x85)
    uint8_t  audio_status = 0x15;  // cd_audio_status (NO_STATUS)
    uint32_t cur_lsn = 0;          // current position (for SubQ/audio)
};

// Initialise the state and sub-parts (image, audio...). Call at boot.
void init();

// Feed a command byte received from the CPLD. When the command is complete (the
// expected length for the opcode), execute it and prepare the output. Returns
// true if this byte COMPLETED the command (the caller must push the first output
// byte to the CPLD, see host_link.h).
bool feed_command_byte(uint8_t b);

#if ENABLE_MPDISK
// BULK-RX (hardfile WRITE 0x61): after the WRITE command, cnt*512 DATA bytes
// follow over HWR (not commands). The main loop MUST drain them in a tight loop
// (poll+feed only), or the CPLD's 2-byte FIFO overflows (the host pushes at
// ~1 us/byte, a heavy main loop would take them at ~5-20 us/byte) -> lost bytes
// -> corrupt block on SD. bulk_rx_active() = a write payload is expected.
bool bulk_rx_active();
// Abort the in-progress bulk-rx (guard timeout if the host stops mid-way):
// discard the partial and flag an error -> the next STATUS (0x63) reports it.
void bulk_rx_abort();
#endif

// True if there are output (status) bytes still to deliver to the host.
bool status_pending();
// Return the next status output byte (advances the index).
uint8_t next_status_byte();

// True if a data transfer (read 0x02) is in progress with bytes still to supply.
bool data_pending();
// Next data byte of the current sector (advances the buffer position).
uint8_t next_data_byte();
// One-shot: true if the LAST completed command STARTED a read (valid 0x02). The
// main loop pushes the 1st data byte + DRQ only in that case: a status command
// completed WHILE a transfer is in progress (poll 0x81 between DMA segments) must
// NOT touch the data channel (status/data collision fix).
bool take_xfer_started();

// Periodic events (media/disc-change poll, audio/seek end): may request STCH.
// Returns true if STCH must be raised (async notification). See cdtv.cpp ~769-783.
bool poll_async();

// Disc change (called by the UI). 'eject' = media absent + STCH (if playing ->
// error); 'insert' = media present + STCH (after the UI has loaded the new image
// with cd_image::load).
void media_eject();
void media_insert();

// Set media present WITHOUT requesting STCH (the boot announce loop does the STCH).
// Used to avoid a double STCH at power-on. See main.cpp.
void set_media_present();

// True once the host has read the TOC (command 0x8a) = the disc was MOUNTED. Used
// by the announce loop to stop pulsing STCH once mounted.
bool toc_read();

// Parser RESYNC (protocol robustness): a spurious/lost byte would leave the parser
// misaligned FOREVER (the next command's bytes would be eaten as parameters: the
// "ghost byte" freeze mechanism). A real command's bytes arrive back-to-back
// (~4 us, with interrupts off); if a command stays half-received past a threshold
// (~10 ms) the main loop discards the partial with parser_resync(). Does NOT touch
// the pending reply (mailbox): the discarded command was never executed.
bool parser_midcmd();   // true = a partial command is in progress (cmd_len != 0)
void parser_resync();   // discard the partial command (cmd_len = cmd_need = 0)

// State access (for UI/LED and disc change).
DriveState& state();

// DEBUG: pointer to the buffer of the LAST received command (cmd_buf[0]=opcode,
// [1..] parameters). Valid right after feed_command_byte()==true (execute() does
// not clear cmd_buf, only the indices). Used by the decoded USB_DEBUG log.
const uint8_t* last_cmd();

} // namespace cdtv
