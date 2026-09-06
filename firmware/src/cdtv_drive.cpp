// =============================================================================
// MatshitaPico -- Solid-state CD-ROM drive replacement for the Commodore CDTV
// Copyright (c) 2026 Nicola Avanzi (na103)
// Licensed under Creative Commons Attribution 4.0 International (CC BY 4.0).
// See LICENSE.
// =============================================================================
// cdtv_drive.cpp -- see cdtv_drive.h.
// Ported from winuae-ref/cdtv.cpp (cdrom_command_thread lines ~524-696 + helpers
// cdrom_subq/cdrom_info/read_toc/cdrom_modeset/play_cd/play_cdtrack/read_sectors)
// and docs/protocol-cdtv.md. The TOC comes from the CUE parser (cd_image); storage
// and audio are implemented (see storage.cpp / audio.cpp).
// =============================================================================
#include "cdtv_drive.h"
#include "cd_image.h"
#include "cdrom_util.h"
#include "audio.h"
#include "storage.h"
#include <cstring>

// Bring-up diagnostic: report "disc ready" status (media+motor+ready=0x60) instead
// of 0x41, to test whether the ROM waits for drive-ready before reading the TOC.
// Set to 0 for the reference behaviour (0x41).
#define BRINGUP_STATUS_READY 0

#if USB_DEBUG
extern void logf(const char* fmt, ...);   // defined in main.cpp (8KB ring + live)
#endif

namespace cdtv {

static DriveState s;

// ---- command / output buffers ------------------------------------------------
static uint8_t  cmd_buf[8];
static uint8_t  cmd_len = 0, cmd_need = 0;
static uint8_t  out_buf[16];
static uint8_t  out_len = 0, out_idx = 0;

#if ENABLE_MPDISK
// ---- bulk-rx (hardfile WRITE 0x61): the HWR bytes after a WRITE command are data,
// not commands -> feed_command_byte diverts them here while bulk_rx_left>0.
static uint8_t  hdf_wbuf[storage::HDF_BLOCK];  // block being accumulated
static uint32_t bulk_rx_left = 0;              // data bytes still expected via HWR
static uint32_t hdf_wlba = 0;                  // current write LBA
static uint8_t  hdf_wunit = 0;
static uint16_t hdf_wpos = 0;                  // position in the current block
static bool     hdf_werr = false;              // write error during bulk-rx
#endif

// ---- data transfer (CD read 0x02 / hardfile read 0x62) -----------------------
static uint32_t data_sectors_left = 0;   // sectors/blocks still to deliver
static uint8_t  raw_sec[cd_image::RAW_SECTOR];
static uint16_t raw_pos = 0;             // position in the current sector payload
static uint16_t raw_payload_off = 0;     // payload offset within the raw 2352
static uint16_t xfer_size = 2352;        // bytes per unit (CD=sector_size, HDF=512)
static bool     raw_valid = false;       // raw_sec holds a valid sector

// DMA data channel source: CD (cd_image) or hardfile (.hdf via storage). The mpdisk
// pipe (0x60-0x64) and the CD are EXCLUSIVE in v1 -> they share the same transfer
// machine (data_sectors_left/raw_*), one transfer at a time. With ENABLE_MPDISK OFF
// only SRC_CD remains and the hardfile branches disappear.
enum DataSource { SRC_CD, SRC_HDF };
static DataSource data_src = SRC_CD;
#if ENABLE_MPDISK
static uint8_t    hdf_unit = 0;          // hardfile unit being read
static uint32_t   hdf_lba  = 0;          // current read LBA (0x62)
#endif
static bool     xfer_started = false;    // the just-executed command STARTED a read
                                         // (one-shot for main: take_xfer_started).
                                         // Distinguishes "this command starts the
                                         // transfer" (push 1st byte) from "command
                                         // completed WHILE a transfer is in progress"
                                         // (poll 0x81: do NOT push data).

// ---- STCH (async notification) -----------------------------------------------
static bool stch_pending = false;
static void request_stch() { stch_pending = true; }

// ---- "mounted": the host has read the TOC (0x8a) = mount done ----------------
// Used by the announce loop (main.cpp) to STOP pulsing STCH once mounted. While
// false, boot pulses STCH slowly -> the host enters interrupt mode (the 6525
// latches STCH only in interrupt mode) and the ROM arms its shadow to bit6=0.
static bool s_toc_read = false;
bool toc_read() { return s_toc_read; }

DriveState& state() { return s; }

// Expected command length per opcode (protocol §3).
static uint8_t cmd_length(uint8_t op) {
    switch (op) {
        // 0x00 = 1-byte NOP/filler: the ROM's armed poll sends it as the trailing
        // byte of "81 00" (size=1, count=1 -> sends 2 bytes: 81 00). 0x81 is 1 byte,
        // so the trailing 00 is filler. Treating it as a 2-byte command would eat
        // the next opcode 0x82 -> the parser would misalign and 0x82/0x84 would
        // NEVER execute. 1 byte, no reply (see execute()).
        case 0x00: return 1;
        case 0x80: return 2;
        case 0x81: case 0x85: case 0x86: case 0x88: case 0xa2: return 1;
        default: return 7;  // 0x01/02/04/05/09/0a/0b/82/83/84/87/89/8a/8b/a3 + unknown
    }
}

static void put_out(const uint8_t* p, int n) {
    if (n < 0) n = 0;
    if (n > (int)sizeof(out_buf)) n = sizeof(out_buf);
    memcpy(out_buf, p, n); out_len = (uint8_t)n; out_idx = 0;
}

static uint8_t status_byte() {
    uint8_t v = 0;
    // bit0 (0x01): in the reference it is !cd_isready, but cd_isready is ALWAYS 0
    // (the countdown is #if 0 and never set positive, cdtv.cpp) -> the real drive
    // reports bit0 ALWAYS set: ejected=0x01, with media=0x41. The CDTV expects this
    // pattern; with 0x40 (bit0 missing) it did not proceed to read the TOC. s.ready
    // stays informational only.
#if BRINGUP_STATUS_READY
    // Bring-up diagnostic: with media report 0x60 = media + motor + READY (bit0=0);
    // ejected stays 0x01. Temporary.
    if (s.media) { v |= 0x40 | 0x20; }   // media + motor, bit0 (not-ready) NOT set
    else         { v |= 0x01; }          // ejected: not-ready
    if (s.playing)   v |= 0x04;
    if (s.finished)  v |= 0x08;
    if (s.error)     v |= 0x10;
    return v;
#else
    v |= 0x01;
    if (s.playing)   v |= 0x04;
    if (s.finished)  v |= 0x08;
    if (s.error)     v |= 0x10;
    if (s.motor)     v |= 0x20;
    if (s.media)     v |= 0x40;
    return v;
#endif
}

// Offset of the user payload inside a raw 2352 sector, for the requested sector
// size (protocol §5). Mode1: 12 sync + 4 header = 16. For "user data" sizes
// (<=2048) the offset depends on the sector MODE (Mode2 form1 has an 8-byte
// subheader: data at 24) -> recomputed per-sector in next_data_byte, like
// sys_command_cd_read of the reference. (Sherlock Holmes, MODE2/2352 tracks, stalled
// after the logo because Mode2 sectors were served at offset 16 = shifted data.)
static uint16_t payload_offset(uint16_t size) {
    switch (size) {
        case 2352: return 0;     // full raw
        case 2340: return 12;    // header + data + EDC/ECC
        case 2052: return 12;    // header + 2048
        case 2336: return 16;    // Mode2 data (subheader included, the app skips it)
        case 2048: default: return 16; // user data (Mode1; Mode2 -> 24 per-sector)
    }
}

// ---- 24-bit MSB-first output helper ------------------------------------------
static void put24(uint8_t* o, uint32_t v) {
    o[0] = (v >> 16) & 0xff; o[1] = (v >> 8) & 0xff; o[2] = v & 0xff;
}

// ---- commands that use the TOC -----------------------------------------------
static int do_capacity(uint8_t* o) {
    const cd_image::Toc& t = cd_image::toc();
    if (!s.media) { s.error = true; return -1; }
    uint32_t size = t.last_lsn ? t.last_lsn - 1 : 0;
    put24(o, size);
    o[3] = s.sector_size >> 8; o[4] = s.sector_size & 0xff;
    return 5;
}

static int do_disc_info(uint8_t* o) {           // 0x89 (cdrom_info)
    const cd_image::Toc& t = cd_image::toc();
    if (!s.media) return -1;
    s.motor = true;
    o[0] = t.first_track;
    o[1] = t.last_track;
    put24(o + 2, (uint32_t)cdrom::lsn2msf(t.last_lsn));
    s.finished = true;
    return 5;
}

static int do_read_toc(uint8_t point, bool msf, uint8_t* o) { // 0x8a
    const cd_image::Toc& t = cd_image::toc();
    if (!s.media) return -1;
    s.motor = true;
    for (int j = 0; j < t.n_tracks; ++j) {
        if (t.tracks[j].number == point) {
            uint32_t lsn = t.tracks[j].start_lsn;
            o[0] = 0;
            o[1] = (1 << 4) | (t.tracks[j].control & 0x0f);  // ADR=1
            o[2] = t.tracks[j].number;
            o[3] = (uint8_t)t.n_tracks;
            o[4] = 0;
            put24(o + 5, msf ? (uint32_t)cdrom::lsn2msf(lsn) : lsn);
            s.finished = true;
            return 8;
        }
    }
    return -1;
}

static int do_subq(bool msf, uint8_t* o) {       // 0x87
    const cd_image::Track* tr = cd_image::track_for_lsn(s.cur_lsn);
    uint32_t diskpos = s.cur_lsn;
    // track-RELATIVE time: starts at 00:00 at track start (counts down in the
    // pregap, like the Q-code). Must NOT add the lead-in, or the player counter
    // starts at 00:02 and "<<" never reaches the previous track (its gate is the
    // time reaching 00:00); in the reference the relative time comes from the disc
    // Q-code = without lead-in.
    uint32_t trackpos = 0;
    if (tr) trackpos = (s.cur_lsn >= tr->start_lsn) ? s.cur_lsn - tr->start_lsn
                                                    : tr->start_lsn - s.cur_lsn;
    // Exact mirror of cdrom_subq (cdtv.cpp:414-441): positions arise in MSF (rel
    // from the Q-code = no lead-in; abs = with lead-in) and the LSN mode converts
    // BOTH with msf2lsn (which SUBTRACTS the 150 lead-in). For abs the -150 yields
    // the true LSN; for REL it is a reference QUIRK: at track start rel_lsn = -150
    // (0xFFFF6A on 24 bits) and the ROM reconverts for the display by ADDING 150 ->
    // counter at 00:00. The player uses 0x87 in LSN mode, so fixing only the MSF
    // branch is not enough.
    uint32_t relmsf = (uint32_t)cdrom::frames2msf((int)trackpos);
    uint32_t absmsf = (uint32_t)cdrom::lsn2msf((int)diskpos);
    o[0] = s.audio_status;
    o[1] = (1 << 4) | (tr ? (tr->control & 0x0f) : 0);  // ADR=1 | CONTROL
    o[2] = tr ? tr->number : 0;                          // track
    o[3] = (s.cur_lsn < (tr ? tr->start_lsn : 0)) ? 0 : 1; // index (0=pregap)
    o[4] = 0;
    put24(o + 5, msf ? absmsf : (uint32_t)cdrom::msf2lsn((int)absmsf));  // = diskpos
    o[8] = 0;
    put24(o + 9, msf ? relmsf : (uint32_t)cdrom::msf2lsn((int)relmsf));  // = trackpos-150
    o[12] = 0;
    return 13;
}

static bool valid_sector_size(uint16_t sz) {
    return sz==512||sz==1024||sz==2048||sz==2052||sz==2336||sz==2340;
}

// ---- read (0x02): start the data transfer ------------------------------------
static void start_read(uint32_t start_lsn, uint16_t n_sectors) {
    // Defensive guard: if a command opcode is lost on the bus, the mode-set payload
    // (84 02 08 00 00 0F 00) would be read as "read 0x02" with an absurd LSN
    // (0x080000) -> a phantom DMA the CDTV's DMAC waits on forever -> frozen screen.
    // If media is missing or the LSN is beyond disc capacity, do NOT start the
    // transfer (error, no data_pending -> no stuck DRQ).
    const cd_image::Toc& t = cd_image::toc();
    if (!s.media || (t.last_lsn && start_lsn >= t.last_lsn)) {
        s.error = true;
        data_sectors_left = 0; raw_valid = false; raw_pos = 0;
        return;
    }
    if (s.playing) { audio::stop(); s.playing = false; }
    cd_image::seek(start_lsn);
    s.cur_lsn = start_lsn;
    data_src = SRC_CD;                       // source = CD (see next_data_byte)
    xfer_size = s.sector_size;               // CD bytes per sector (2048/2352/...)
    data_sectors_left = n_sectors;
    raw_valid = false; raw_pos = 0;
    raw_payload_off = payload_offset(s.sector_size);
    xfer_started = true;    // one-shot for main: push the 1st byte + DRQ (even if a
                            // previous transfer was abandoned half-way)
    s.motor = true;
    s.audio_status = 0x00;  // AUDIO_STATUS_NOT_SUPPORTED (data)
}

// ---- play (0x09 lsn / 0x0a msf) ----------------------------------------------
static void start_play(uint8_t op, const uint8_t* p) {
    // cancel any pending data transfer: audio and data share cd_image's file cursor
    // and cannot both be active
    data_sectors_left = 0; raw_valid = false; raw_pos = 0;
    uint32_t start = (p[1] << 16) | (p[2] << 8) | p[3];
    uint32_t end   = (p[4] << 16) | (p[5] << 8) | p[6];
    if (op == 0x09) end += start;          // in LSN mode 'end' is a length
    if (start == 0 && end == 0) {          // stop
        audio::stop();                     // STOP the audio engine: without this the
                                           // engine stayed St::Playing and a later
                                           // 0x8b unpause would RESUME it (audio
                                           // restarting from a stopped player).
        if (s.playing) s.finished = true;
        s.playing = false; s.motor = false;
        s.audio_status = 0x15;             // NO_STATUS
        s.error = true;
        request_stch();
        return;
    }
    if (op != 0x09) {                      // MSF -> LSN
        start = (uint32_t)cdrom::msf2lsn(start);
        if (end < 0x00ffffff) end = (uint32_t)cdrom::msf2lsn(end);
    }
    const cd_image::Toc& t = cd_image::toc();
    if (end >= 0x00ffffff || (t.last_lsn && end > t.last_lsn)) end = t.last_lsn;
    s.playing = true;
    s.audio_status = 0x11;                 // IN_PROGRESS
    s.cur_lsn = start;
    audio::play(start, end);
    // STCH at play START, like the reference statusfunc (transition to IN_PROGRESS
    // -> activate_stch). It completes the play's deferred io: the armed poll this
    // STCH triggers sees bit2 (playing)=1 and the gate arms bit2 in shadow+mask; at
    // play end poll_async's STCH shows bit2=0 -> change -> the play io completes.
    // Without this STCH the app waits forever (Sherlock Holmes hung after the logo).
    request_stch();
}

// ---- play track range (0x0b) -- port of play_cdtrack (cdtv.cpp:328) ----------
// p[1]=start track, p[3]=end track (p[2]/p[4] = index, ignored as in the
// reference). end = the end track's START (not its end!) or end of disc if the end
// track does not exist. A non-existent start track -> stop + error + STCH.
static void start_play_track(const uint8_t* p) {
    data_sectors_left = 0; raw_valid = false; raw_pos = 0;  // like start_play
    uint8_t track_start = p[1];
    uint8_t track_end   = p[3];
    const cd_image::Toc& t = cd_image::toc();
    if (track_start == 0 && track_end == 0) {
        // DEVIATION from the reference (which no-ops): the ROM's CD+G screen sends
        // "04 motor-on, 0B 00.., A3" then polls status at 75 Hz waiting for the
        // playing bit; with a no-op it waits forever (no graphics). On the real
        // drive "0B all-zeros" starts play; WinUAE's guess was never exercised (its
        // CD+G delivers no subchannel). Chosen semantics: play from the CURRENT
        // position to end of disc (from a fresh position = the whole disc, karaoke
        // mode). If the position is outside the tracked area (stale beyond end of
        // disc, or BEFORE track 1's start: many rips start at LSN>0) -> latch to the
        // first track's start.
        uint32_t start = s.cur_lsn;
        if (start >= t.last_lsn || !cd_image::track_for_lsn(start))
            start = t.n_tracks ? t.tracks[0].start_lsn : 0;
        s.cur_lsn = start;
        s.playing = true;
        s.audio_status = 0x11;                  // IN_PROGRESS
        audio::play(start, t.last_lsn);
        request_stch();                         // STCH at play start (statusfunc, see start_play)
        return;
    }
    uint32_t start = 0, end = t.last_lsn;
    bool start_found = false;
    for (int j = 0; j < t.n_tracks; ++j) {
        if (t.tracks[j].number == track_start) { start = t.tracks[j].start_lsn; start_found = true; }
        if (t.tracks[j].number == track_end)   { end   = t.tracks[j].start_lsn; }
    }
    if (!start_found) {
        audio::stop();
        s.finished = s.playing;                 // cdaudiostop: finished only if it was playing
        s.audio_status = s.playing ? 0x13 : 0x15; // PLAY_COMPLETE / NO_STATUS
        s.playing = false;
        s.error = true;
        request_stch();
        return;
    }
    s.playing = true;
    s.audio_status = 0x11;                      // IN_PROGRESS
    s.cur_lsn = start;
    audio::play(start, end);
    request_stch();                             // STCH at play start (statusfunc, see start_play)
}

#if USB_DEBUG
// [84-dbg] MODE-SET 0x84 capture: logs every 0x84 in full (rare, non-perturbing) +
// the READ(0x02)/PLAY(0x09/0a) context only at an opcode change (FMV boundary). Does
// NOT log the 0x87/0x81 polls (~16 Hz spam that would disturb the FMV DMA timing).
static void dbg_cmd(const uint8_t* c) {   // ::logf = main.cpp's (global scope)
    switch (c[0]) {
        case 0x84:
            ::logf("[84-dbg] MODESET 84 %02x %02x %02x %02x %02x %02x  (byte1=page)\n",
                 c[1], c[2], c[3], c[4], c[5], c[6]);
            break;
        case 0x02: case 0x09: case 0x0a: {
            static uint8_t last_op = 0xff;      // rate-limit: only READ<->PLAY transition
            if (c[0] != last_op) {
                last_op = c[0];
                ::logf("[84-dbg] %s   %02x %02x %02x %02x %02x %02x %02x\n",
                     c[0] == 0x02 ? "READ" : "PLAY",
                     c[0], c[1], c[2], c[3], c[4], c[5], c[6]);
            }
            break;
        }
        default: break;                          // 0x81/0x87/... = poll, silent
    }
}
#endif

// ---- execute a completed command ---------------------------------------------
static void execute() {
    const uint8_t op = cmd_buf[0];
#if USB_DEBUG
    dbg_cmd(cmd_buf);
#endif
    out_len = out_idx = 0;
    switch (op) {
        case 0x00:                              // NOP/filler (trailing byte of the
            // armed poll "81 00"): NO reply, NO STEN -> misaligns neither the 6525
            // nor our parser. out_len stays 0 -> the main loop asserts no STEN for
            // this byte. See cmd_length(0x00)=1.
            break;
        case 0x80: {                            // test/ping
            uint8_t r[2] = {0xAA, 0x55}; put_out(r, 2); break;
        }
        case 0x01:                               // seek
            s.finished = true; request_stch(); break;
        case 0x02:                               // read data
            start_read((cmd_buf[1] << 16) | (cmd_buf[2] << 8) | cmd_buf[3],
                       (uint16_t)((cmd_buf[4] << 8) | cmd_buf[5]));
            break;
        case 0x04: s.motor = true;  s.finished = true; break;
        case 0x05: s.motor = false; s.finished = true; break;
        case 0x09: case 0x0a:                    // play (lsn/msf)
            start_play(op, cmd_buf); break;
        case 0x0b:                               // play track range
            start_play_track(cmd_buf); break;
        case 0x81: {                             // status byte
            uint8_t r = status_byte(); put_out(&r, 1); s.finished = false; break;
        }
        case 0x82: {                             // read/clear error
            uint8_t r[6] = {0}; if (s.error) r[2] |= 0x10; put_out(r, 6);
            s.error = false; s.ready = false; s.finished = true; break;
        }
        case 0x83: {                             // INQUIRY -> "MATSHITA0.96"
            static const char id[12] = {'M','A','T','S','H','I','T','A','0','.','9','6'};
            put_out((const uint8_t*)id, 12); s.finished = true; break;
        }
        case 0x84:                               // mode set
            if (valid_sector_size((cmd_buf[2] << 8) | cmd_buf[3]))
                s.sector_size = (cmd_buf[2] << 8) | cmd_buf[3];
            else
                s.error = true;
            s.finished = true; break;
        case 0x85: {                             // mode sense
            uint8_t r[2] = {(uint8_t)(s.sector_size >> 8), (uint8_t)s.sector_size};
            put_out(r, 2); break;
        }
        case 0x86: put_out(out_buf, do_capacity(out_buf)); break;   // capacity
        case 0x87: put_out(out_buf, do_subq(cmd_buf[1] & 2, out_buf)); break;
        case 0x88: { uint8_t r[14] = {0}; put_out(r, 14); } break;
        case 0x89: put_out(out_buf, do_disc_info(out_buf)); break;
        case 0x8a: put_out(out_buf, do_read_toc(cmd_buf[2], cmd_buf[1] & 2, out_buf));
                   s_toc_read = true;   // host read the TOC = mounted -> stop STCH announce
                   break;
        case 0x8b:                               // pause (param 0x00) / resume
            audio::pause(cmd_buf[1] == 0x00);
            if (audio::playing())
                // in pause the reference does not touch cd_playing (pause_audio only
                // sets cd_paused): status bit2 stays SET even while paused-stopped.
                s.audio_status = (cmd_buf[1] == 0x00) ? 0x12 : 0x11; // PAUSED / IN_PROGRESS
            else
                s.playing = false;               // pause/unpause with motor stopped: nothing to resume
            s.finished = true; break;
        case 0xa2: { uint8_t r[4] = {0}; put_out(r, 4); } break;
        case 0xa3: s.finished = true; break;     // front panel enable

#if ENABLE_MPDISK
        // ---- mpdisk vendor pipe (hardfile .hdf, 0x60-0x64) -------------------
        // Exclusive with the CD in v1 (the Amiga device uses Forbid). 32-bit MSB-
        // first LBA in cmd_buf[2..5], unit in [1], count(1..255 blocks) in [6].
        case 0x60: {                             // DETECT: signature + ver + n_units + blocksize
            storage::scan_hardfiles();           // refresh: reflect the .hdf present NOW on the SD
            uint8_t r[8] = {'M','P','D','K', 0x01,
                            (uint8_t)storage::hardfile_count(),
                            (uint8_t)(storage::HDF_BLOCK >> 8),
                            (uint8_t)storage::HDF_BLOCK};
            put_out(r, 8); s.finished = true;
#if USB_DEBUG
            ::logf("[mpd] DETECT -> nunits=%d blk=%d (8B via STEN)\n",
                   (int)storage::hardfile_count(), (int)storage::HDF_BLOCK);
#endif
            break;
        }
        case 0x64: {                             // GEOMETRY(unit): total_blocks(4)+blocksize(2)
            uint32_t nb = storage::hardfile_blocks(cmd_buf[1]);
            uint8_t r[6] = {(uint8_t)(nb>>24),(uint8_t)(nb>>16),(uint8_t)(nb>>8),(uint8_t)nb,
                            (uint8_t)(storage::HDF_BLOCK>>8),(uint8_t)storage::HDF_BLOCK};
            if (nb == 0) s.error = true;
            put_out(r, 6); s.finished = true;
#if USB_DEBUG
            ::logf("[mpd] GEO unit=%d -> blocks=%lu (6B via STEN)%s\n",
                   (int)cmd_buf[1], (unsigned long)nb, nb ? "" : " ERR");
#endif
            break;
        }
        case 0x62: {                             // READ(unit,LBA[4],count): DMA hardfile->host
            uint8_t  unit = cmd_buf[1];
            uint32_t lba  = ((uint32_t)cmd_buf[2]<<24)|((uint32_t)cmd_buf[3]<<16)|
                            ((uint32_t)cmd_buf[4]<<8)|cmd_buf[5];
            uint8_t  cnt  = cmd_buf[6] ? cmd_buf[6] : 1;
            uint32_t nb   = storage::hardfile_blocks(unit);
            if (nb == 0 || lba >= nb || (uint64_t)lba + cnt > nb) {  // out of range: no phantom DMA
                s.error = true;
                data_sectors_left = 0; raw_valid = false; raw_pos = 0;
#if USB_DEBUG
                ::logf("[mpd] READ unit=%d lba=%lu cnt=%d RANGE-ERR (nb=%lu, NO DMA)\n",
                       (int)unit, (unsigned long)lba, (int)cnt, (unsigned long)nb);
#endif
                break;
            }
            if (s.playing) { audio::stop(); s.playing = false; }
            s.error = false;
            data_src = SRC_HDF; hdf_unit = unit; hdf_lba = lba;
            xfer_size = storage::HDF_BLOCK;
            data_sectors_left = cnt;
            raw_valid = false; raw_pos = 0; raw_payload_off = 0;
            xfer_started = true;                 // main: push the 1st byte + DRQ
            s.motor = true;
#if USB_DEBUG
            ::logf("[mpd] READ unit=%d lba=%lu cnt=%d OK -> DMA %d B started\n",
                   (int)unit, (unsigned long)lba, (int)cnt, (int)cnt * storage::HDF_BLOCK);
#endif
            break;
        }
        case 0x61: {                             // WRITE(unit,LBA[4],count): enter bulk-rx
            uint8_t  unit = cmd_buf[1];
            uint32_t lba  = ((uint32_t)cmd_buf[2]<<24)|((uint32_t)cmd_buf[3]<<16)|
                            ((uint32_t)cmd_buf[4]<<8)|cmd_buf[5];
            uint8_t  cnt  = cmd_buf[6] ? cmd_buf[6] : 1;
            uint32_t nb   = storage::hardfile_blocks(unit);
            if (nb == 0 || lba >= nb || (uint64_t)lba + cnt > nb) { s.error = true;
#if USB_DEBUG
                ::logf("[mpd] WRITE unit=%d lba=%lu cnt=%d RANGE-ERR (nb=%lu)\n",
                       (int)unit, (unsigned long)lba, (int)cnt, (unsigned long)nb);
#endif
                break; }
            s.error = false; hdf_werr = false;
            hdf_wunit = unit; hdf_wlba = lba; hdf_wpos = 0;
            bulk_rx_left = (uint32_t)cnt * storage::HDF_BLOCK;   // cnt*512 bytes follow via HWR
            s.motor = true;
#if USB_DEBUG
            ::logf("[mpd] WRITE unit=%d lba=%lu cnt=%d OK -> bulk-rx %lu B\n",
                   (int)unit, (unsigned long)lba, (int)cnt, (unsigned long)bulk_rx_left);
#endif
            break;
        }
        case 0x63: {                             // STATUS: ready/error/busy
            uint8_t st = 0x01;                   // bit0 = ready (pipe active)
            if (s.error || hdf_werr) st |= 0x02; // bit1 = last error
            if (bulk_rx_left > 0)    st |= 0x04; // bit2 = write (bulk-rx) in progress
            uint8_t r[2] = { st, 0 };
            put_out(r, 2); s.finished = true;
#if USB_DEBUG
            ::logf("[mpd] STATUS -> %02X (2B via STEN)\n", (int)st);
#endif
            break;
        }
#endif // ENABLE_MPDISK

        default:   s.error = true; break;        // unknown
    }
}

void init() {
    cmd_len = cmd_need = 0;
    out_len = out_idx = 0;
    data_sectors_left = 0; raw_valid = false; raw_pos = 0;
    data_src = SRC_CD; xfer_size = 2352;
#if ENABLE_MPDISK
    bulk_rx_left = 0; hdf_wpos = 0; hdf_werr = false;
#endif
    stch_pending = false;
    s_toc_read = false;
    s = DriveState{};
}

#if ENABLE_MPDISK
bool bulk_rx_active() { return bulk_rx_left > 0; }

void bulk_rx_abort() {
    // Payload timeout/stall: discard the partial and flag an error for the STATUS.
    bulk_rx_left = 0; hdf_wpos = 0;
    hdf_werr = true; s.error = true; s.finished = true;
}
#endif

bool feed_command_byte(uint8_t b) {
#if ENABLE_MPDISK
    // BULK-RX (hardfile WRITE 0x61): after a WRITE the HWR bytes are DATA, not
    // commands -> accumulate in hdf_wbuf and write a full block. No output.
    if (bulk_rx_left > 0) {
        hdf_wbuf[hdf_wpos++] = b;
        bulk_rx_left--;
        if (hdf_wpos >= storage::HDF_BLOCK) {
            if (storage::hardfile_write(hdf_wunit, hdf_wlba, hdf_wbuf, 1) < 1) hdf_werr = true;
            hdf_wlba++; hdf_wpos = 0;
        }
        if (bulk_rx_left == 0) { s.error = hdf_werr; s.finished = true; }  // result -> 0x63
        return false;              // not a completed command: no output push
    }
#endif
    if (cmd_len == 0) {
        cmd_need = cmd_length(b);
        out_len = out_idx = 0;     // new command: discard the unconsumed output
    }
    if (cmd_len < sizeof(cmd_buf)) cmd_buf[cmd_len] = b;
    cmd_len++;
    if (cmd_len >= cmd_need) { execute(); cmd_len = cmd_need = 0; return true; }
    return false;
}

bool parser_midcmd() { return cmd_len != 0; }
void parser_resync() { cmd_len = cmd_need = 0; }   // does NOT touch out_len: the
                                                   // pending reply (mailbox) stays readable

bool status_pending()      { return out_idx < out_len; }
uint8_t next_status_byte() { return (out_idx < out_len) ? out_buf[out_idx++] : 0; }

bool data_pending() { return data_sectors_left > 0 || (raw_valid && raw_pos < xfer_size); }

bool take_xfer_started() { bool v = xfer_started; xfer_started = false; return v; }

const uint8_t* last_cmd() { return cmd_buf; }   // DEBUG: see cdtv_drive.h

uint8_t next_data_byte() {
    // need a new sector/block?
    if (!raw_valid || raw_pos >= xfer_size) {
        if (data_sectors_left == 0) return 0;
#if ENABLE_MPDISK
        if (data_src == SRC_HDF) {
            // hardfile: 1 block of 512 B, no header (payload from offset 0)
            uint32_t got = storage::hardfile_read(hdf_unit, hdf_lba, raw_sec, 1);
            if (got < 1) { data_sectors_left = 0; raw_valid = false; s.error = true;
#if USB_DEBUG
                ::logf("[mpd] READ FAIL: hardfile_read(lba=%lu) -> %lu\n",
                       (unsigned long)hdf_lba, (unsigned long)got);
#endif
                return 0; }
            raw_valid = true; raw_pos = 0; raw_payload_off = 0;
            hdf_lba++;
            data_sectors_left--;
        } else
#endif
        {
            size_t got = cd_image::read_raw(raw_sec, 1);   // read 1 raw 2352 sector
            if (got < cd_image::RAW_SECTOR) {              // end of file / error
                data_sectors_left = 0; raw_valid = false;
                s.error = true; return 0;
            }
            raw_valid = true; raw_pos = 0;
            // user data (<=2048): per-sector offset from the MODE byte (Mode1=16, Mode2=24)
            if (s.sector_size <= 2048)
                raw_payload_off = (raw_sec[15] == 0x02) ? 24 : 16;
            data_sectors_left--;
            s.cur_lsn++;
        }
    }
    uint8_t v = raw_sec[raw_payload_off + raw_pos];
    raw_pos++;
    if (raw_pos >= xfer_size && data_sectors_left == 0) {
        s.finished = true;                              // last byte of the last sector/block
#if USB_DEBUG && ENABLE_MPDISK
        if (data_src == SRC_HDF)
            ::logf("[mpd] READ DMA drained to the end (end lba=%lu)\n",
                   (unsigned long)hdf_lba);
#endif
    }
    return v;
}

bool poll_async() {
    // SubQ (0x87) position tied to the real playback
    if (s.playing && audio::playing())
        s.cur_lsn = audio::current_lsn();
    // audio end -> update state (the real audio signals it via audio::playing()).
    if (s.playing && !audio::playing()) {
        s.playing = false;
        s.audio_status = 0x13;       // PLAY_COMPLETE
        s.finished = true;
        request_stch();
    }
    if (stch_pending) { stch_pending = false; return true; }
    return false;
}

// ---- disc change (from the UI) -----------------------------------------------
void media_eject() {
    if (s.playing) {
        audio::stop();
        s.playing = false;
        s.audio_status = 0x14;   // PLAY_ERROR (interrupted by eject)
        s.error = true;
    }
    data_sectors_left = 0; raw_valid = false; raw_pos = 0;
    s.media = false;
    s.motor = false;             // no disc = motor stopped (consistent with wait-motor)
    s.ready = false;
    s_toc_read = false;          // no longer mounted -> resume the STCH announce (re-prime shadow)
    request_stch();              // the CDTV rereads the status -> "no media"
}

void media_insert() {
    s.media = true;
    s.motor = false;             // the REAL drive reports MOTOR OFF at mount (status 0x49,
                                 // bit5=0). With media+motor-OFF the host enters wait-motor,
                                 // EMITS 0x04 (motor-on) and arms a poll -> mount. Reporting
                                 // motor-ON already (0x61) would make the host SKIP that branch
                                 // and NOT mount.
    s.ready = true;
    s.finished = true;           // spin-up complete = bit3 SET -> status 0x49, not 0x41. The
                                 // mount proceeds past the ROM's media-change softint only when
                                 // the first post-insert poll reports 0x49 (bit3=1 = drive done);
                                 // with 0x41 (bit3=0) it stalls ("drive still working").
    s.cur_lsn = 0;
    s.audio_status = 0x15;       // NO_STATUS
    request_stch();              // on the STCH edge the CDTV rereads status/TOC/capacity
}

void set_media_present() {       // like media_insert but WITHOUT STCH (boot diagnostic)
    s.media = true;
    s.motor = false;             // see media_insert: motor OFF at mount (status 0x49); the
                                 // host's 0x04 turns it on.
    s.ready = true;
    s.finished = true;           // see media_insert: bit3 SET at end of spin-up (status 0x49)
    s.cur_lsn = 0;
    s.audio_status = 0x15;       // NO_STATUS
}

} // namespace cdtv
