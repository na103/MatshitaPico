// =============================================================================
// MatshitaPico -- Solid-state CD-ROM drive replacement for the Commodore CDTV
// Copyright (c) 2026 Nicola Avanzi (na103)
// Licensed under Creative Commons Attribution 4.0 International (CC BY 4.0).
// See LICENSE.
// =============================================================================
// cd_image.h -- CD image on SD card (BIN/CUE 2352 raw) + TOC.
// Parses the CUE (FILE/TRACK/INDEX/PREGAP) and maps LSN -> offset in the BIN.
// Raw 2352 images (data + audio). LSN/MSF conventions (as in cdtv.cpp): 1 frame =
// 1 sector, 75 frames/s; LSN is the disc logical address, the host-visible MSF
// time = LSN + 150 (2 s lead-in). CUE times (INDEX/PREGAP) are in-file positions
// in frames. See docs/protocol-cdtv.md.
// =============================================================================
#pragma once
#include <cstdint>
#include <cstddef>

namespace cd_image {

static constexpr uint32_t RAW_SECTOR = 2352;
static constexpr int      MAX_TRACKS = 99;
static constexpr int      MAX_FILES  = 99;  // Redump = 1 bin/track (up to 99)
static constexpr int      MAX_NAME   = 128; // long Redump names need > 64

// A BIN file referenced by the CUE.
struct CueFile {
    char     name[MAX_NAME] = {0};
    uint64_t size_bytes     = 0;   // from the filesystem at parse; 0 = missing bin
                                   // -> parse FAILS (TOC would be unusable)
};

struct Track {
    uint8_t  number      = 0;
    uint8_t  control     = 0;      // 0x04 = data, 0x00 = audio (bit 0x0c)
    uint8_t  file_index  = 0;      // index into Toc::files
    uint16_t sector_size = RAW_SECTOR;
    uint32_t pregap_lsn  = 0;      // LSN of INDEX 00 (or = start_lsn if absent)
    uint32_t start_lsn   = 0;      // LSN of INDEX 01 (start of user data)
    uint64_t file_offset = 0;      // byte offset of start_lsn in its own file
};

struct Toc {
    uint8_t  first_track = 0;
    uint8_t  last_track  = 0;
    uint32_t last_lsn    = 0;      // end of disc (for capacity)
    bool     data_first  = false;  // first track = data
    int      n_tracks    = 0;
    Track    tracks[MAX_TRACKS];
    int      n_files     = 0;
    CueFile  files[MAX_FILES];
};

// Mount the SD, scan /games/*.cue, build the ordered (alphabetical) list. Does
// NOT load any disc (boots ejected): true if mount ok and at least one image found.
bool boot();

// Number of images found and current index.
int  count();
int  current_index();

// Load image i (parse CUE -> TOC). Used by the NEXT button.
bool load(int index);

// TOC of the current image.
const Toc& toc();

// Find the track that contains a given LSN (or nullptr if out of range).
const Track* track_for_lsn(uint32_t lsn);

// Position the read cursor at the given LSN (for read 0x02 / play). False if out of range.
bool seek(uint32_t lsn);

// Read 'n_sectors' raw 2352 sectors from the current position into 'dst', from
// the DATA file cache (host DMA / read 0x02).
size_t read_raw(uint8_t* dst, uint32_t n_sectors);

// Like read_raw but from the dedicated AUDIO file cache (storage::read_audio_at):
// the I2S play path does NOT evict the data bin, so on a multi-file CUE (data and
// audio in different .bin) the first play does not rebuild the LINKMAP (~80 ms)
// inside the audio stream. Used by audio.cpp. Does not touch the subcode or the
// DMA buffers (current_lsn granularity for CD+G stays 1 sector).
size_t read_raw_audio(uint8_t* dst, uint32_t n_sectors);

// Warm the file caches (open + build the FAT LINKMAP + bring the FatFs path into
// the XIP cache) by reading 1 sector per cache. Call at disc INSERT, while audio
// is idle and the host is spinning up, so the FIRST steady-state access does not
// pay cold latency inside the audio/host stream. Warms BOTH the first data track
// file (read_at cache) AND the first audio track file (read_audio_at cache): on a
// multi-file CUE those are different .bin and stay warm together -> no start-of-
// track "scratch"/underrun and no blocking window that drops host commands
// (reset). Universal: on a single-file disc it warms the same file in both caches
// (harmless). Does not touch the subcode or the drive state.
void prewarm();

// ---- CloneCD subchannel (CD+G) ----------------------------------------------
// At load(), if the CUE has a SINGLE image file (CloneCD rip: one .img) and a
// sibling <name>.sub exists with size = sectors*96, the subchannel is available:
// 96 bytes/sector, DEINTERLEAVED per channel (P=0..11, Q=12..23, R=24..35, ...
// W=84..95; each channel = 96 bits MSB-first, bit f = frame f).
static constexpr uint32_t SUB_SECTOR = 96;

// true if the current image has the .sub (CD+G possible).
bool has_sub();

// Name of the .sub looked up at load() (diagnostic: set even if has_sub()==false,
// empty if the CUE is multi-file).
const char* sub_name();

// Read the 96 subchannel bytes of the sector at the given LSN. false if
// unavailable or out of range.
bool read_sub(uint32_t lsn, uint8_t dst[SUB_SECTOR]);

// ---- Per-disc subcode gate ---------------------------------------------------
// true if the current disc has at least one DATA track (control & 0x04).
bool has_data_track();

// true if the SBCP subcode MUST be transmitted for the current disc:
//   - CD+G (.sub present)     -> ON  (graphics + LED)
//   - all-AUDIO disc          -> ON  (LED; nothing polls port A)
//   - pure DATA disc          -> ON  (LED; no audio playback -> never reads Q ->
//                                     no crash with subcode, e.g. Grolier)
//   - MIXED disc (data+audio) -> OFF (Sherlock and similar read Q via port A
//                                     during play and ANY subcode traffic crashes
//                                     them)  ...BUT a <name>.subok marker beside
//                                     the .cue forces ON (a mixed disc the user
//                                     knows is safe, e.g. Xenon 2 -> LED alive)
// CD+G (music + R-W) and games that read Q via port A never coexist on the same
// disc, so the choice is per-disc, entirely in firmware (CPLD unchanged).
bool subcode_wanted();

} // namespace cd_image
