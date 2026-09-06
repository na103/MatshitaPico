// =============================================================================
// MatshitaPico -- Solid-state CD-ROM drive replacement for the Commodore CDTV
// Copyright (c) 2026 Nicola Avanzi (na103)
// Licensed under Creative Commons Attribution 4.0 International (CC BY 4.0).
// See LICENSE.
// =============================================================================
// storage.h -- SD card over SPI (FatFs) -- glue.
// Full-size SD card on SPI (see pinmap.h). rev.0: plain SPI, no SDIO. Sits on top
// of a FatFs port for the Pico (e.g. no-OS-FatFS-SD-SPI-RPi-Pico by carlk3, or
// ff15 + an SPI diskio). See firmware/README.md.
// =============================================================================
#pragma once
#include <cstdint>
#include <cstddef>

namespace storage {

// Initialise SPI + mount the SD's FAT filesystem. Returns true on success.
bool mount();

// Boot-log diagnostics: mount state + last FRESULT of mount/scan (int, so ff.h is
// not exposed; 0 = FR_OK, see fr_str() in main.cpp).
bool is_mounted();
int  last_error();

// Scan /games/*.cue and populate the ordered internal list. Returns the count.
int  scan_cues();

// Path of the i-th CUE; nullptr if out of range.
const char* cue_name(int index);

// Read up to 'cap' bytes of a text file into 'buf'. Returns bytes read (0 = error).
size_t read_text(const char* path, char* buf, size_t cap);

// Byte size of a (BIN) file in the current CUE's folder. 0 if unknown.
uint64_t file_size(const char* name);

// true if the file exists (distinguishes "present but empty" from "absent", which
// file_size cannot). Used for zero-size markers (e.g. <name>.subok).
bool file_exists(const char* name);

// Read 'len' bytes from the BIN starting at 'offset' into 'dst'. Returns bytes read.
size_t read_at(const char* name, uint64_t offset, uint8_t* dst, size_t len);

// Like read_at but on a SECOND dedicated file cache (CD+G): the .sub subchannel is
// read interleaved with the audio BIN (96 bytes/block, 75 Hz), and with a single
// cache the two files would evict each other (open/close every sector). Same
// semantics, its own (smaller) FASTSEEK CLMT.
size_t read_sub_at(const char* name, uint64_t offset, uint8_t* dst, size_t len);

// Like read_at but on a THIRD dedicated AUDIO file cache: on multi-file CUEs
// (Redump: 1 bin/track) DATA and AUDIO live in different .bin. With a single
// cache, the first play after data reads evicted the data file and REBUILT the
// audio bin's LINKMAP (~80 ms measured on E.S.S. Mega) inside the I2S stream ->
// underrun/glitch at track start, and the ~80 ms window with the main loop
// blocked could drop a host command -> CDTV reset. With its own cache, data and
// audio stay warm together. Full FASTSEEK CLMT (256 entries) like read_at.
size_t read_audio_at(const char* name, uint64_t offset, uint8_t* dst, size_t len);

// Write a text file (create/overwrite). Returns true on success.
bool write_text(const char* path, const char* text);

#if ENABLE_MPDISK
// ---- .hdf hardfiles (mpdisk.device, 512 B block device) ----------------------
// The .hdf files live in the SD ROOT (/*.hdf); /games stays for CDs only.
// Alphabetical scan -> unit 0,1,... Fixed 512 B block (WinUAE hardfile style).
// The .hdf is preallocated at fixed size (writes do not extend the file -> the
// FAT chain is immutable). See docs/drive-commands.md and amiga/mpdisk.
static constexpr uint32_t HDF_BLOCK = 512;

int  scan_hardfiles();                  // populate the list, return the count
int  hardfile_count();
const char* hardfile_name(int unit);    // .hdf path; nullptr if out of range
uint32_t hardfile_blocks(int unit);     // size in 512 B blocks (0 if unknown)

// Read/write 'nblk' 512 B blocks from block 'lba'. Returns the blocks actually
// transferred (0 = error / out of range). The write does f_sync.
uint32_t hardfile_read (int unit, uint32_t lba, uint8_t* dst, uint32_t nblk);
uint32_t hardfile_write(int unit, uint32_t lba, const uint8_t* src, uint32_t nblk);
#endif // ENABLE_MPDISK

// Persistent "last disc" state (/matshita.sta): index of the inserted disc,
// -1 = ejected. Survives a Pico reset (in rev.1 the CDTV _RESET reaches RUN via a
// BAT54: a disc change REBOOTS the CDTV, so the Pico restarts) and power-off (the
// disc "stays in the caddy" like a real drive). load_last_disc() returns -1 if
// the file is missing or unreadable too.
void save_last_disc(int index);
int  load_last_disc();

} // namespace storage
