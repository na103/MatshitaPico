// =============================================================================
// MatshitaPico -- Solid-state CD-ROM drive replacement for the Commodore CDTV
// Copyright (c) 2026 Nicola Avanzi (na103)
// Licensed under Creative Commons Attribution 4.0 International (CC BY 4.0).
// See LICENSE.
// =============================================================================
// main.cpp -- firmware main loop (RP2350 / Pico 2).
// The CPLD (ATF1508AS) is the real-time front-end on CN9; the Pico implements the
// drive protocol (ported from winuae-ref/cdtv.cpp), reads BIN/CUE images from the
// SD card, generates I2S audio and the subcode Q channel.
// =============================================================================
#include "pico/stdlib.h"

// ---- USB debug: off in the product firmware --------------------------------
// All bring-up logging (logf over USB CDC + boot-log replay on each connection +
// diagnostic heartbeat) is behind USB_DEBUG: at 0 the USB is not even initialised
// (no stdio_init_all -> no CDC device, no background work, no USBCTRL_IRQ
// interference) and logf is a no-op. Overridable from CMake (a separate diagnostic
// build: cmake -DUSB_DEBUG=ON -> a .uf2 with an 8KB ring buffer + replay + heartbeat).
#ifndef USB_DEBUG
#define USB_DEBUG 0
#endif

// Build marker (printed at the top of the boot log): the debug variant self-
// identifies, so a log can never be attributed to the wrong build.
#if USB_DEBUG
 #if defined(SUBCODE_MUTE)
  #define BUILD_TAG "[qplay-dbg-NOSUB]"
 #elif defined(SUBCODE_NOGATE)
  #define BUILD_TAG "[qplay-dbg-NOGATE]"
 #elif defined(SUBCODE_QNEVER)
  #define BUILD_TAG "[qplay-dbg-QNEVER]"
 #elif defined(SUBCODE_QZERO)
  #define BUILD_TAG "[qplay-dbg-QZERO]"
 #elif defined(SUBCODE_ROT)
  #define BT_STR2(x) #x
  #define BT_STR(x) BT_STR2(x)
  #define BUILD_TAG "[qplay-dbg-ROT" BT_STR(SUBCODE_ROT) "]"
 #else
  #define BUILD_TAG "[qplay-dbg]"
 #endif
#else
#define BUILD_TAG "[qplay]"
#endif

#if USB_DEBUG
#include "pico/stdio_usb.h"
#endif
#include "pinmap.h"
#include "host_link.h"
#include "cdtv_drive.h"
#include "cd_image.h"
#include "storage.h"
#ifndef SD_TEST_BREADBOARD
#include "sd_spi_pio.h"
#endif
#include "audio.h"
#include "subcode.h"
#include "ui.h"
#include <cstdio>
#include <cstdarg>
#include <cstring>

// ---- PERSISTENT AUTO-INSERT (final UX) -------------------------------------
// At boot, if the SD state (/matshita.sta) says a disc was INSERTED, re-insert it
// with a simulated spin-up: boot EJECTED (init 81/82/84 + an armed poll at no-media
// = the host shadow primes to 0x20 on its own) + media_insert AUTOINSERT_DELAY_MS
// after the 1st host command -> bit6 transition -> MOUNT in the grey phase = the
// hardware-confirmed mount path. Covers three cases:
//   (a) DISC CHANGE: NEXT saves "inserted N" and inserts -> the CDTV reboots -> in
//       rev.1 _RESET reaches RUN via a BAT54 -> the Pico restarts -> re-insert disc
//       N -> the NEW disc boots.
//   (b) CDTV RESET button: the disc "stays in the caddy" and reboots (real drive).
//   (c) POWER-ON: the machine remembers the last disc used.
// Absent state / -1 (explicit eject) / invalid index = boot EJECTED: the CDTV goes
// to the boot screen and NEXT inserts (disc-change cycle in ui.cpp).
#define AUTOINSERT_DELAY_MS 1500  // simulated spin-up: from the 1st host command to
                                  // the insert. Not a race (150..10000 ms tested
                                  // equivalent): only the bit6 transition matters.

#define CMD_RESYNC_MS 10          // parser resync timeout: a half-received command
                                  // with no new bytes for >10 ms = the 1st byte was
                                  // spurious (ghost) or one was lost -> discard the
                                  // partial. A real command's bytes arrive ~4 us apart
                                  // (with interrupts off); the min inter-command gap is
                                  // >=90 us: 10 ms = 3 orders of margin.

// ---- boot log (USB) with replay --------------------------------------------
// Boot prints happen BEFORE the USB terminal connects and would be lost: they
// accumulate in a buffer and are reprinted on EACH new connection
// (stdio_usb_connected() = terminal DTR). RING BUFFER: keeps the last ~8KB, so a
// NEXT pressed after boot is always visible when draining the buffer.
#if USB_DEBUG
static char   s_bootlog[8192];
static size_t s_bootpos = 0;        // write position (wraps)
static bool   s_bootwrapped = false;

// NOT static: cdtv_drive.cpp uses it (extern) for the [84-dbg] mode-set capture log.
// Writes to the 8KB ring + live, with a timestamp -> coherent timeline.
void logf(const char* fmt, ...) {
    char line[180];
    // TIMESTAMP (ms since boot) at the head of each line: for the mount timeline.
    uint32_t t = to_ms_since_boot(get_absolute_time());
    int pre = snprintf(line, sizeof(line), "[%lu] ", (unsigned long)t);
    if (pre < 0 || pre >= (int)sizeof(line)) pre = 0;
    va_list ap; va_start(ap, fmt);
    vsnprintf(line + pre, sizeof(line) - pre, fmt, ap);
    va_end(ap);
    fputs(line, stdout);
    size_t l = strlen(line);
    for (size_t i = 0; i < l; ++i) {           // circular append (keeps the last 8KB)
        s_bootlog[s_bootpos++] = line[i];
        if (s_bootpos >= sizeof(s_bootlog)) { s_bootpos = 0; s_bootwrapped = true; }
    }
}
#else
// no-op: the arguments are evaluated (no "unused" warnings) but nothing happens;
// the linker drops what was used only inside the prints.
static inline void logf(const char*, ...) {}
#endif

static bool s_boot_ok = false;
static void try_boot();

// AUTO-INSERT (simulated spin-up): armed by try_boot if the persistent state says
// "disc inserted" and the TOC loads; the timer starts at the 1st host command; on
// expiry the main loop does media_insert(). See PERSISTENT AUTO-INSERT.
static bool            s_autoinsert_armed = false;
static int             s_autoinsert_idx   = -1;
static absolute_time_t s_autoinsert_at;

#if USB_DEBUG
static void log_service() {
    static bool was_connected = false;
    bool now = stdio_usb_connected();
    if (now && !was_connected) {
        fputs("\n==== MatshitaPico boot log ====\n", stdout);
        if (s_bootwrapped) {   // ring full: from the oldest (at s_bootpos) to the newest
            fwrite(s_bootlog + s_bootpos, 1, sizeof(s_bootlog) - s_bootpos, stdout);
            fwrite(s_bootlog, 1, s_bootpos, stdout);
        } else {
            fwrite(s_bootlog, 1, s_bootpos, stdout);
        }
        fputs("==== end of boot log ====\n", stdout);
        // Bring-up aid: if boot failed, retry the mount NOW that the terminal is
        // connected -> the SD library init logs are visible live.
        if (!s_boot_ok) {
            fputs("boot failed: retrying the mount...\n", stdout);
            try_boot();
        }
    }
    was_connected = now;
}
#else
static inline void log_service() {}
#endif

// Most common FatFs FRESULTs in clear text (for the log)
static const char* fr_str(int fr) {
    switch (fr) {
        case 0:  return "OK";
        case 1:  return "DISK_ERR (SPI I/O: wiring/contacts?)";
        case 3:  return "NOT_READY (card not responding: CS/power?)";
        case 4:  return "NO_FILE";
        case 5:  return "NO_PATH (missing folder?)";
        case 13: return "NO_FILESYSTEM (reformat FAT32)";
        default: return "see FatFs FRESULT";
    }
}

// Boot/retry: mount SD + scan /games + load(0), with detailed log and LED. Called
// at power-on and (if it failed) on each new terminal connection.
static void try_boot() {
    // This whole function does blocking SD I/O (mount + TOC scan + load + prewarm)
    // while audio runs empty (silence): without service() the ping-pong DMA would
    // underrun and glitch ("tick" at boot). Suspend the I2S stream: SM stopped ->
    // DMA in backpressure -> A_DATA=0 (silence). (pause/resume are no-ops if audio
    // is not yet initialised.)
    audio::pause_stream();
    s_boot_ok = cd_image::boot();

    if (!storage::is_mounted()) {
        logf("SD: mount FAILED, FR=%d %s\n", storage::last_error(),
             fr_str(storage::last_error()));
    } else {
#ifdef SD_TEST_BREADBOARD
        logf("SD: mount OK - SPI1 hardware backend (BREADBOARD variant, carlk3 lib)\n");
#else
        logf("SD: mount OK - PIO-SPI backend on pio2 (PCB pins), card %lu MB\n",
             (unsigned long)(sd_pio::sector_count() / 2048));
#endif
        int n = cd_image::count();
        if (n == 0)
            logf("/games: no .cue found, FR=%d %s\n",
                 storage::last_error(), fr_str(storage::last_error()));
        else {
            logf("/games: %d images\n", n);
            for (int i = 0; i < n; ++i)
                logf("  [%d] %s\n", i, storage::cue_name(i));
        }
        if (n > 0 && !s_boot_ok)
            logf("load(0) FAILED: unreadable cue or parse error\n");
#if ENABLE_MPDISK
        // .hdf hardfiles (mpdisk.device): SCAN the SD root -> populate the list.
        // Without this, hardfile_count() stays 0 -> DETECT reports nunits=0 -> every
        // read = BADADDRESS -> the OFS loops.
        int nh = storage::scan_hardfiles();
        if (nh == 0)
            logf(".hdf in root: none (mpdisk will have 0 units)\n");
        else {
            logf(".hdf in root: %d\n", nh);
            for (int i = 0; i < nh; ++i)
                logf("  hdf[%d] %s (%lu blocks)\n", i, storage::hardfile_name(i),
                     (unsigned long)storage::hardfile_blocks(i));
        }
#endif
    }

    if (s_boot_ok) {
        // Diagnostic check: parse the TOC of ALL images (stresses .bin resolution and
        // file_size() on the real filesystem, e.g. multi-bin Redump). Leaves the index
        // on the last one: the first NEXT inserts [0] by wrap-around.
        for (int i = 0; i < cd_image::count(); ++i) {
            if (cd_image::load(i)) {
                const cd_image::Toc& d = cd_image::toc();
                logf("  [%d] TOC ok: tracks %d-%d, %d files, last_lsn=%lu%s\n",
                     i, d.first_track, d.last_track, d.n_files,
                     (unsigned long)d.last_lsn,
                     d.data_first ? ", track 1 = data" : ", audio only");
                // CD+G diagnostic: was the .sub detected?
                if (d.n_files == 1) {
                    uint64_t want = (d.files[0].size_bytes / cd_image::RAW_SECTOR)
                                    * cd_image::SUB_SECTOR;
                    logf("      sub %s: %s (expected %lu B, found %lu B)\n",
                         cd_image::sub_name(),
                         cd_image::has_sub() ? "OK" : "NO",
                         (unsigned long)want,
                         (unsigned long)storage::file_size(cd_image::sub_name()));
                }
            } else {
                logf("  [%d] TOC FAILED: unreadable cue or parse error\n", i);
            }
        }
        // Persistent state: if a disc was inserted (NEXT saves it BEFORE the insert,
        // a long eject saves -1), re-insert it with the simulated spin-up = the
        // confirmed mount path. See PERSISTENT AUTO-INSERT.
        int last = storage::load_last_disc();
        if (last >= 0 && last < cd_image::count() && cd_image::load(last)) {
            cd_image::prewarm();         // warm the file (LINKMAP+XIP) before play
            s_autoinsert_idx   = last;
            s_autoinsert_armed = true;   // the timer starts at the 1st host command
            ui::set_led(ui::Led::Ejected);
            logf("SD state: disc [%d] inserted -> auto-insert in %ums after the 1st host command: %s\n",
                 last, (unsigned)AUTOINSERT_DELAY_MS, storage::cue_name(last));
        } else {
            // Boot EJECTED: no media inserted, like a real drive with no caddy. The
            // CDTV stays on the wait screen until NEXT inserts a disc (cd_media = 0).
            ui::set_led(ui::Led::Ejected);
            logf("boot EJECTED (SD state: %d): NEXT inserts disc [0]\n", last);
        }
    } else {
        ui::set_led(ui::Led::Error);    // no image / SD error
        logf("LED: Error (1 Hz blink)\n");
    }

    audio::resume_stream();             // SD I/O done: resume the I2S stream
}

int main() {
#if USB_DEBUG
    stdio_init_all();          // log over USB (debug, off the connector GPIOs)
#endif

    // Initialisation
    host_link::init();         // data bus + handshake with the CPLD
    audio::init();             // I2S slave (A_DATA), clocks from the CPLD
    subcode::init();           // Q channel (A_SUBQ)
    ui::init();                // NEXT button (GP3) + LED (GP25)
    cdtv::init();              // drive state

    // Build marker at the top of the boot log: each test self-identifies.
    logf("firmware build: %s %s %s\n", __DATE__, __TIME__, BUILD_TAG);

    // Boot: mount SD, pick the image, build the TOC, signal media present. RETRY on
    // cold boot: from cold the card may not be ready yet (mount FR=3 NOT_READY on the
    // first power-on, OK on later reboots via RESET->RUN). Without retry, a cold
    // USB-off boot stays disc-less. The 1st host command arrives at ~2.4 s: 5 tries
    // x 300 ms fit comfortably.
    try_boot();
    for (int r = 0; r < 5 && !s_boot_ok; ++r) {
        sleep_ms(300);
        logf("SD not ready: retry mount %d/5...\n", r + 1);
        try_boot();
    }

    // ---- drive announce to the host (STCH at boot) ---------------------------
    // The real drive asserts STCH at reset (cdtv.cpp cdtv_reset_int: stch=1) to make
    // itself detected: that initial STCH makes the CDTV start the CD task (TPI INT2);
    // only then does the host poll status and write commands. We boot EJECTED and did
    // not announce -> the CDTV never started the dialogue. We announce here and RE-
    // announce periodically until the host writes the first command, so we do not
    // depend on the CDTV's reset<->TPI-setup timing. After the first command, STCH is
    // event-driven again.
    bool host_seen = false;
    bool s_suppress_rd = false;   // suppresses the "rd st" of a duplicate 0x81 poll (log dedup)
    absolute_time_t next_announce = get_absolute_time();

    // ---- model of the CPLD's output FIFO (word engine) -----------------------
    // The CPLD has a 2-slot alternating FIFO (host_if.v): the CDTV DMAC is a WORD
    // engine (2 back-to-back _HRD reads ~212 ns per DRQ cycle) and DRQ rises only on
    // a complete word (fill==2). Each command byte FLUSHES the CPLD FIFO (realignment
    // + DRQ suspension during commands): the firmware keeps the model of the contents
    // here and re-pushes the flushed data bytes at the end of a reply ("interlude").
    // BYTE_REQ is a toggle: ONE event per byte consumed (even in a pair).
    uint8_t s_fifo_val[2] = {0, 0};    // FIFO contents in order (0 = next read)
    uint8_t s_fifo_n      = 0;         // bytes we believe are still in the FIFO
    bool    s_fifo_is_data = false;    // true = holds DATA bytes (replay candidates)
    uint8_t s_replay[2]   = {0, 0};    // data bytes flushed by a command, to re-push
    uint8_t s_replay_n    = 0;
    bool    s_status_interlude = false; // status reply in progress WHILE a data transfer is active

    // FIFO DESYNC DIAGNOSTIC: invariant D = push - cons - replay. push = DATA bytes
    // pushed to the CPLD; cons = data-channel BYTE_REQ events (host consumes); replay
    // = re-pushed bytes. With the channel quiet D MUST be 0: D>0 = pushes swallowed,
    // never reaching the host (LOST bytes); D<0 = the host consumed bytes never
    // pushed (STALE reads = the ~300 ns flush race inside an _HRD pair).
    uint32_t s_dbg_push = 0, s_dbg_cons = 0, s_dbg_replay = 0;

#if USB_DEBUG
    // "scratch" glitch measurement window at the 1st play: armed by a play command,
    // counts host traffic and at the end dumps the audio metrics (accumulated in RAM
    // by audio.cpp -> no live logf during the critical window, so the USB Heisenbug
    // does not inflate the very measurements we want).
    absolute_time_t s_audbg_at{};
    bool            s_audbg_armed = false;
    uint32_t        s_audbg_cmds  = 0;   // host command bytes seen in the window
#endif

    // push a byte to the CPLD, updating the model
    auto push_out = [&](uint8_t v, bool is_data) {
        host_link::send_byte(v);
        if (s_fifo_n < 2) s_fifo_val[s_fifo_n++] = v;
        s_fifo_is_data = is_data;
        if (is_data) s_dbg_push++;
    };
    // (re)start/resume the data channel: replay the flushed bytes + top-up to 2 + DRQ
    auto resume_data = [&]() {
        s_dbg_replay += s_replay_n;
        for (uint8_t i = 0; i < s_replay_n; ++i) push_out(s_replay[i], true);
        s_replay_n = 0;
        while (s_fifo_n < 2 && cdtv::data_pending())
            push_out(cdtv::next_data_byte(), true);
        host_link::set_data_avail(s_fifo_n != 0);
    };

    // ---- main loop -----------------------------------------------------------
    // CMD_STB/BYTE_REQ edges are captured on IRQ (host_link) and queued; here we
    // consume the rings. The CPLD asserts STEN/DRQ only with a fresh byte loaded
    // (out_valid) -> the FIRST byte of a reply is pushed as soon as the command
    // completes; the rest on BYTE_REQ.
    while (true) {
        // 0) periodic STCH announce until the disc is mounted (TOC read). The 6525
        //    latches STCH in the AIR ONLY in INTERRUPT mode; in I/O mode it discards
        //    it. At boot we are in I/O mode, so we keep pulsing STCH slowly until the
        //    host enters interrupt mode and the ROM arms its shadow to bit6=0, then
        //    the insert (media) drives the mount. Announce only while media is ABSENT
        //    (the real drive emits ONE STCH per media change); resumes after an eject.
        if (!cdtv::state().media && !cdtv::toc_read() && time_reached(next_announce)) {
            host_link::pulse_stch();
            next_announce = make_timeout_time_ms(150);
        }

        // 1) command byte arriving from the CPLD?
        static uint32_t s_last_cmd_byte_ms = 0;     // for the resync (see 1-bis)
        uint8_t cb;
        if (host_link::poll_cmd_byte(&cb)) {
#if USB_DEBUG
            if (s_audbg_armed) s_audbg_cmds++;   // host traffic in the measurement window
#endif
            s_last_cmd_byte_ms = to_ms_since_boot(get_absolute_time());
            bool first = !host_seen;
            host_seen = true;                       // the host talks -> stop the boot announce
            if (first && s_autoinsert_armed)        // simulated spin-up: timer from the 1st command
                s_autoinsert_at = make_timeout_time_ms(AUTOINSERT_DELAY_MS);
            host_link::release_stch();              // the host is servicing the event -> release
                                                    // the STCH level (real drive/WinUAE: stch=0
                                                    // on the command)

            // BEFORE snapshotting the FIFO for replay: apply the consumes ALREADY
            // queued (BYTE_REQ events that arrived before this command byte = before
            // the CPLD flush). Without this drain, with DMA active the model is stale
            // and replay saves bytes ALREADY consumed by the host -> duplicates in the
            // stream on resume = corrupt buffer. No top-up here: the hw FIFO is about
            // to be / already is flushed.
            {
                bool req_id;
                while (host_link::poll_byte_req(&req_id)) {
                    if (req_id) s_dbg_cons++;
                    if (s_fifo_n) { s_fifo_n--; s_fifo_val[0] = s_fifo_val[1]; }
                }
            }

            // EACH command byte FLUSHES the CPLD FIFO (host_if.v, realignment): align
            // the model. In-flight DATA bytes are saved for replay at the end of the
            // reply; flushed status contents = discarded (stale output). DRQ is
            // already down in the CPLD (fill=0) = transfer suspended.
            if (s_fifo_n && s_fifo_is_data && s_replay_n == 0) {
                s_replay_n  = s_fifo_n;
                s_replay[0] = s_fifo_val[0];
                s_replay[1] = s_fifo_val[1];
            }
            s_fifo_n = 0;

            // REPLY FIRST, LOG LATER. USB CDC is very slow (~60 us/line): logging
            // before set_status_avail delayed _STEN by ~180 us -> the CDTV timed out
            // on the status read. The real drive updates STEN within ~105 ns; we must
            // stay within a few us.
            bool complete = cdtv::feed_command_byte(cb);

#if ENABLE_MPDISK
            // BULK-RX (hardfile write): if the byte just processed was the WRITE
            // command (0x61), bulk-rx is now ARMED. The cnt*512 payload bytes arrive
            // ~1 us/byte (the host pushes them back-to-back); a heavy main loop would
            // take them at ~5-20 us/byte -> the CPLD's 2-byte FIFO overflows -> lost
            // bytes -> corrupt block on SD. Drain the payload HERE, at max speed
            // (poll+feed only), until written. Guard timeout: if the host stops mid-
            // way we do not hang.
            if (cdtv::bulk_rx_active()) {
                uint32_t idle = 0;
                while (cdtv::bulk_rx_active()) {
                    uint8_t wb;
                    if (host_link::poll_cmd_byte(&wb)) { cdtv::feed_command_byte(wb); idle = 0; }
                    else if (++idle > 2000000u) { cdtv::bulk_rx_abort(); break; }
                }
                continue;   // payload handled: 0x61 has no status/data, restart the loop
            }
#endif

            bool st = false, da = false; uint8_t b0 = 0;
            if (complete) {
                st = cdtv::status_pending();
                da = cdtv::data_pending();
                bool xfer_new = cdtv::take_xfer_started();  // this command STARTS a read
                if (st) {
                    // Command with a status reply WHILE a data transfer is active (poll
                    // 0x81 between bootstrap reads): INTERLUDE. The FIFO is already
                    // flushed and DRQ down; status occupies it alone; the data channel
                    // resumes at the end of the reply (resume_data with replay).
                    if (s_replay_n || da) {
                        host_link::set_data_avail(false);
                        s_status_interlude = true;
                    }
                    b0 = cdtv::next_status_byte();
                    push_out(b0, false);                // push 1st byte
                    host_link::set_status_avail(true);  // <-- _STEN asserts HERE
                }
                if (xfer_new) {
                    // ONLY the command that starts the transfer (0x02) primes the FIFO
                    // (2 bytes = the first word). Any previous stream abandoned by the
                    // host dies here (replay cleared).
                    s_status_interlude = false;
                    s_replay_n = 0;
                    resume_data();
                    ui::notify_access();
                } else if (!st && da) {
                    // command WITHOUT a reply (seek, motor, A3...) completed with an
                    // active data transfer: the flush suspended it -> resume at once.
                    resume_data();
                } else if (!st && s_replay_n) {
                    // command that CANCELLED the transfer (play 09/0A/0B, stop) with
                    // data bytes still in flight: they belong to a DEAD stream. Re-
                    // pushing them would raise DRQ with 2 stale bytes while the host's
                    // DMAC might still hold the abandoned read's residual WTC = risk of
                    // garbage DMA into chip RAM. Discard and keep DRQ down.
                    s_replay_n = 0;
                    host_link::set_data_avail(false);
                }
            }
            // log outside the STEN critical path. RUN-LENGTH: repeated 0x81 polls with
            // the SAME status are COUNTED and printed as "rx 81 xN" when the value
            // changes or >2 s pass. Unlike pure dedup it hides nothing: an infinite
            // loop of identical polls shows as a growing counter. Non-polls (0x8a,
            // 0x82...) are always logged.
            static uint8_t  s_rl_b0    = 0xFF;   // status of the last poll run
            static uint32_t s_rl_count = 0;      // consecutive identical polls accumulated
            static uint32_t s_rl_t0    = 0;      // ms of the run's first poll
            bool is_poll = complete && st && cb == 0x81;
            bool same    = is_poll && b0 == s_rl_b0 && s_rl_count > 0;
            uint32_t now = to_ms_since_boot(get_absolute_time());
            // close the accumulated run if: a different event arrives, or status differs, or >2 s
            if (s_rl_count > 1 && (!same || (now - s_rl_t0) > 2000)) {
                logf("rx 81 x%lu (ST b0=%02X, %lums)\n", (unsigned long)s_rl_count,
                     s_rl_b0, (unsigned long)(now - s_rl_t0));
                s_rl_count = 0;
            }
            if (same) {
                if (s_rl_count == 1) s_rl_t0 = now;  // start of silent accumulation
                s_rl_count++;
                s_suppress_rd = true;                // no "rd st" for the repeats
            } else {
                s_suppress_rd = false;
                if (first) logf("host: 1st command received\n");
                logf("rx %02X\n", cb);
                if (complete) {
                    logf("  done st=%d da=%d f=%u r=%u D=%ld\n", st, da,
                         (unsigned)s_fifo_n, (unsigned)s_replay_n,
                         (long)s_dbg_push - (long)s_dbg_cons - (long)s_dbg_replay);
                    if (st) logf("  ST b0=%02X\n", b0);
                    // Decoded read/play/subq trace: op + params + current positions.
                    // cur=cdtv::cur_lsn (advances with DMA in reads), aud=audio lsn
                    // (advances only in play): their DIVERGENCE during FMV is the suspect.
                    const uint8_t* c = cdtv::last_cmd();
                    uint32_t cur = cdtv::state().cur_lsn, aud = audio::current_lsn();
                    if (c[0] == 0x02)
                        logf("  CMD 02 READ lsn=%lu n=%u cur=%lu aud=%lu\n",
                             (unsigned long)(((uint32_t)c[1] << 16) | (c[2] << 8) | c[3]),
                             (unsigned)((c[4] << 8) | c[5]),
                             (unsigned long)cur, (unsigned long)aud);
                    else if (c[0] == 0x09 || c[0] == 0x0a || c[0] == 0x0b || c[0] == 0x87)
                        logf("  CMD %02X p=%02X%02X%02X.%02X%02X%02X cur=%lu aud=%lu\n",
                             c[0], c[1], c[2], c[3], c[4], c[5], c[6],
                             (unsigned long)cur, (unsigned long)aud);
#if USB_DEBUG
                    // a play arms the "scratch" glitch measurement window
                    if (c[0] == 0x09 || c[0] == 0x0a || c[0] == 0x0b) {
                        audio::dbg_reset();
                        s_audbg_at    = make_timeout_time_ms(1200);
                        s_audbg_armed = true;
                        s_audbg_cmds  = 0;
                    }
#endif
                }
                if (is_poll) { s_rl_b0 = b0; s_rl_count = 1; s_rl_t0 = now; }
                else if (complete) s_rl_count = 0;   // a real command closes the run
            }
        }

        // 1-bis) parser RESYNC: a half-received command with no byte for >CMD_RESYNC_MS
        // -> the partial is garbage (ghost byte / lost byte): discard it, so the NEXT
        // real byte is read as an opcode. Without this, a single spurious byte
        // misaligns the parser forever (irreversible freeze with the fire-and-forget
        // READ 02). Protocol robustness: kept in rev.1 too.
        if (cdtv::parser_midcmd()) {
            uint32_t now_rs = to_ms_since_boot(get_absolute_time());
            if ((uint32_t)(now_rs - s_last_cmd_byte_ms) > CMD_RESYNC_MS) {
                cdtv::parser_resync();
                logf("parser RESYNC: partial command discarded (>%u ms without a byte)\n",
                     (unsigned)CMD_RESYNC_MS);
            }
        }

        // 2) the host consumed a byte (BYTE_REQ toggle event, one per byte)
        bool is_data;
        if (host_link::poll_byte_req(&is_data)) {
            // REPLY FIRST (see latency note above), LOG LATER
            bool more = false;
            uint8_t v = 0;
            // update the model: a byte left the CPLD FIFO
            if (s_fifo_n) { s_fifo_n--; s_fifo_val[0] = s_fifo_val[1]; }
            if (is_data) {
                s_dbg_cons++;
                if (!s_status_interlude && !cdtv::parser_midcmd()
                    && !host_link::cmd_pending()) {
                    // top-up: keep the FIFO full (2 = one word) while data remains. The
                    // word pairs arrive as TWO back-to-back events: the first tops up
                    // to 2, the second tops up again. GUARD cmd_pending: a command byte
                    // already queued is about to flush the FIFO (or already has in hw):
                    // pushing now risks the CPLD's blocked window (~2.8 us) = lost byte.
                    // The command completion does the resume (resume_data).
                    uint32_t ce0 = host_link::cmd_events();
                    while (s_fifo_n < 2 && cdtv::data_pending())
                        push_out(cdtv::next_data_byte(), true);
                    more = (s_fifo_n != 0);
                    if (s_fifo_n == 0)
                        host_link::set_data_avail(false);   // transfer exhausted
                    ui::notify_access();   // LED blip during the transfer
                    if (host_link::cmd_events() != ce0)
                        logf("PUSH-RACE: command arrived during the top-up (f=%u)\n",
                             (unsigned)s_fifo_n);
                }
                // in interlude: residual pre-flush consume, no action (the good bytes
                // are in s_replay, they return with resume_data at status end). MID-
                // COMMAND (parser_midcmd, bytes 2..7 of a command arriving mid-stream):
                // same, only a model consume -- a top-up here would be FLUSHED by the
                // next command byte without landing in replay (already occupied) = lost
                // image bytes, shifted stream. Resume is done by the command completion.
            } else {
                if (cdtv::status_pending()) {
                    more = true;
                    v = cdtv::next_status_byte();
                    push_out(v, false);     // status: ONE byte at a time (each push re-
                                            // pulses _STEN in the CPLD = 1 INT2/byte)
                } else {
                    // status reply exhausted: channel down
                    host_link::set_status_avail(false);
                    if (s_status_interlude) {
                        // END OF INTERLUDE: status consumed -> resume the data channel
                        // (replay the flushed bytes + top-up + DRQ up).
                        s_status_interlude = false;
                        resume_data();
                    }
                }
            }
            // log AFTER the reply; status only (data would flood the USB), and not if
            // the poll that caused the read was a suppressed duplicate (s_suppress_rd)
            if (!is_data && !s_suppress_rd) logf("  rd st (more=%d v=%02X)\n", more, v);
        }

        // 3) async events (disc change, audio/seek end) -> STCH
        if (cdtv::poll_async()) {
            host_link::pulse_stch();
            logf("STCH async (media=%d motor=%d)\n",
                 cdtv::state().media, cdtv::state().motor);
        }

        // 3-bis) persistent AUTO-INSERT: simulated spin-up (see try_boot and the header)
        if (s_autoinsert_armed && host_seen && time_reached(s_autoinsert_at)) {
            s_autoinsert_armed = false;
            cdtv::media_insert();       // media=1, motor off (0x41) + async STCH (step 3)
            ui::set_led(ui::Led::Ready);
            logf("AUTO-INSERT: disc [%d] inserted (simulated spin-up)\n", s_autoinsert_idx);
        }

#if USB_DEBUG
        // 3-ter) diagnostic HEARTBEAT: proof of life of the main loop + IRQ counters.
        // In a post-insert freeze the log is silent: if the heartbeat is silent too =
        // OUR firmware stopped; if it beats with the counters frozen = the HOST is
        // really mute.
        {
            static uint32_t s_hb_next = 5000;   // starts after boot, every 2 s
            uint32_t now_hb = to_ms_since_boot(get_absolute_time());
            if ((int32_t)(now_hb - s_hb_next) >= 0) {
                s_hb_next = now_hb + 2000;
                logf("hb %lus cmd=%lu req=%lu sub=%lu/%lu D=%ld\n",
                     (unsigned long)(now_hb / 1000),
                     (unsigned long)host_link::cmd_events(),
                     (unsigned long)host_link::req_events(),
                     (unsigned long)subcode::sub_ok(),
                     (unsigned long)subcode::sub_fail(),
                     (long)s_dbg_push - (long)s_dbg_cons - (long)s_dbg_replay);
            }
        }
#endif

        // 4) periodic services
        host_link::service_stch();  // 20ms safety cap of the STCH level (mute host)
        audio::service();
        subcode::service();
        ui::service();

#if USB_DEBUG
        // window end: dump the "scratch" glitch metrics. large read_max = a cold read
        // (LINKMAP) blocks the loop; gap_max >13ms = service starved (underrun/repeat =
        // the scratch); high cmds = a host burst starving it.
        if (s_audbg_armed && time_reached(s_audbg_at)) {
            s_audbg_armed = false;
            logf("[audio-dbg] +1.2s play: read_max=%luus gap_max=%luus underruns=%lu "
                 "read_slow=%lu gap_big=%lu host_bytes=%lu\n",
                 (unsigned long)audio::dbg_read_max_us(),
                 (unsigned long)audio::dbg_gap_max_us(),
                 (unsigned long)audio::dbg_underruns(),
                 (unsigned long)audio::dbg_read_slow(),
                 (unsigned long)audio::dbg_gap_big(),
                 (unsigned long)s_audbg_cmds);
        }
#endif
        log_service();      // replay the boot log on each USB connection
    }
}
