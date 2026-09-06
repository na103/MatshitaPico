// =============================================================================
// MatshitaPico -- Solid-state CD-ROM drive replacement for the Commodore CDTV
// Copyright (c) 2026 Nicola Avanzi (na103)
// Licensed under Creative Commons Attribution 4.0 International (CC BY 4.0).
// See LICENSE.
// =============================================================================
// storage.cpp -- SD card / FatFs glue (standard ChaN FatFs API).
// Uses ONLY the FatFs f_* API (mount/opendir/readdir/open/read/lseek/stat), so it
// is independent of the chosen SD-SPI library (carlk3 no-OS-FatFS-SD-SPI, or ff15
// + an SPI diskio). The HARDWARE CONFIG (SPI/CS pins, baud) lives in hw_config.c
// or the library's diskio, NOT here.
// SD layout: images in /games/*.cue (+ the referenced .bin). Alphabetical
// (case-insensitive) order = NEXT button order.
// ffconf.h: FF_FS_READONLY=0 REQUIRED (the "last disc" state /matshita.sta is
// WRITTEN, see save_last_disc); FF_USE_LFN>=1 (long names with spaces/parens).
// =============================================================================
#include "storage.h"
#include "ff.h"
#include "ram_func.h"
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <strings.h>   // strcasecmp

namespace storage {

static const char* GAMES_DIR = "/games";

static constexpr int MAX_IMAGES = 64;
static constexpr int PATH_LEN   = 256;   // /games/<long name>.cue

static FATFS  s_fs;
static bool   s_mounted = false;
static int    s_last_fr = 0;     // last FRESULT of mount/opendir (diagnostic)

static char   s_cues[MAX_IMAGES][PATH_LEN];   // full .cue paths (sorted)
static int    s_ncues = 0;

static char   s_dir[PATH_LEN] = "/games";     // current cue's folder (for relative .bin)

// 1-file cache for read_at()'s sequential reads
static FIL     s_fil;
static char    s_fil_name[PATH_LEN] = {0};
static bool    s_fil_open = false;
static FSIZE_t s_fil_pos  = 0;

// FASTSEEK: CLMT (Cluster Link Map Table) of the open BIN. Without it, every
// f_lseek to a distant point walks the FAT chain on the SD (hundreds of ms on a
// multi-hundred-MB BIN) = the main-loop stall that delayed NEXT under streaming
// and long audio jumps. With the CLMT (built ONCE at open, f_lseek
// CREATE_LINKMAP) a seek resolves offset->cluster in RAM, no SD reads. 256 entries
// = 1 KB = up to ~127 fragments; if the file is more fragmented
// (FR_NOT_ENOUGH_CORE) it falls back to classic seeks (slower, no functional error).
static DWORD   s_clmt[256];

// base folder from the cue path (to resolve relative BINs)
static void set_base_dir(const char* cue_path) {
    strncpy(s_dir, cue_path, sizeof(s_dir) - 1);
    s_dir[sizeof(s_dir) - 1] = 0;
    char* sl = strrchr(s_dir, '/');
    if (sl) { if (sl == s_dir) s_dir[1] = 0; else *sl = 0; }
    else    { strcpy(s_dir, "."); }
}

// resolve a name (absolute or relative to s_dir) into a full path
static void resolve(const char* name, char* out, size_t cap) {
    if (strchr(name, '/')) { strncpy(out, name, cap - 1); out[cap - 1] = 0; }
    else                   { snprintf(out, cap, "%s/%s", s_dir, name); }
}

bool mount() {
    if (s_mounted) return true;
    // SPI/SD init is done by the library's diskio (hw_config). Here we mount.
    FRESULT fr = f_mount(&s_fs, "", 1);
    s_last_fr = (int)fr;
    if (fr != FR_OK) return false;
    s_mounted = true;
    return true;
}

bool is_mounted()  { return s_mounted; }
int  last_error()  { return s_last_fr; }

static int cmp_cue(const void* a, const void* b) {
    return strcasecmp((const char*)a, (const char*)b);
}

int scan_cues() {
    s_ncues = 0;
    if (!s_mounted) return 0;
    DIR dir;
    FILINFO fno;
    FRESULT fr = f_opendir(&dir, GAMES_DIR);
    s_last_fr = (int)fr;
    if (fr != FR_OK) return 0;
    while (s_ncues < MAX_IMAGES) {
        if (f_readdir(&dir, &fno) != FR_OK || fno.fname[0] == 0) break;
        if (fno.fattrib & AM_DIR) continue;
        size_t L = strlen(fno.fname);
        if (L > 4 && strcasecmp(fno.fname + L - 4, ".cue") == 0) {
            snprintf(s_cues[s_ncues], PATH_LEN, "%s/%s", GAMES_DIR, fno.fname);
            s_ncues++;
        }
    }
    f_closedir(&dir);
    qsort(s_cues, s_ncues, PATH_LEN, cmp_cue);  // alphabetical = NEXT order
    return s_ncues;
}

const char* cue_name(int index) {
    if (index < 0 || index >= s_ncues) return nullptr;
    return s_cues[index];
}

size_t read_text(const char* path, char* buf, size_t cap) {
    set_base_dir(path);             // the cue's .bin resolve in this folder
    FIL f;
    if (f_open(&f, path, FA_READ) != FR_OK) return 0;
    UINT br = 0;
    f_read(&f, buf, (UINT)cap, &br);
    f_close(&f);
    return br;
}

uint64_t file_size(const char* name) {
    char p[PATH_LEN];
    resolve(name, p, sizeof(p));
    FILINFO fno;
    if (f_stat(p, &fno) != FR_OK) return 0;
    return (uint64_t)fno.fsize;
}

bool file_exists(const char* name) {
    char p[PATH_LEN];
    resolve(name, p, sizeof(p));
    FILINFO fno;
    return f_stat(p, &fno) == FR_OK;
}

size_t RAM_FUNC(read_at)(const char* name, uint64_t offset, uint8_t* dst, size_t len) {
    char p[PATH_LEN];
    resolve(name, p, sizeof(p));
    // (re)open only if the file changes (contiguous sector reads = same file)
    if (!s_fil_open || strcmp(s_fil_name, p) != 0) {
        if (s_fil_open) f_close(&s_fil);
        if (f_open(&s_fil, p, FA_READ) != FR_OK) { s_fil_open = false; return 0; }
        // FASTSEEK: build the CLMT of the just-opened file (see s_clmt above)
        s_clmt[0]    = sizeof(s_clmt) / sizeof(s_clmt[0]);
        s_fil.cltbl  = s_clmt;
        if (f_lseek(&s_fil, CREATE_LINKMAP) != FR_OK)
            s_fil.cltbl = nullptr;  // too fragmented: fall back to FAT seeks
        strncpy(s_fil_name, p, PATH_LEN - 1); s_fil_name[PATH_LEN - 1] = 0;
        s_fil_open = true;
        s_fil_pos  = (FSIZE_t)-1;   // force the lseek
    }
    if (s_fil_pos != (FSIZE_t)offset) {
        if (f_lseek(&s_fil, (FSIZE_t)offset) != FR_OK) return 0;
        s_fil_pos = (FSIZE_t)offset;
    }
    UINT br = 0;
    if (f_read(&s_fil, dst, (UINT)len, &br) != FR_OK) return 0;
    s_fil_pos += br;
    return br;
}

// --- second file cache: .sub subcode (CD+G) -----------------------------------
// Like read_at() but on a separate FIL: the .sub reads (96 B/block at 75 Hz during
// play) alternate with the audio BIN's, and with a single cache each read would
// reopen the file. 64-entry CLMT (256 B): the .sub is ~1/25 of the BIN, ~31
// fragments covered; if not enough -> classic FAT seeks.
static FIL     s_sub_fil;
static char    s_sub_name[PATH_LEN] = {0};
static bool    s_sub_open = false;
static FSIZE_t s_sub_pos  = 0;
static DWORD   s_sub_clmt[64];

size_t read_sub_at(const char* name, uint64_t offset, uint8_t* dst, size_t len) {
    char p[PATH_LEN];
    resolve(name, p, sizeof(p));
    if (!s_sub_open || strcmp(s_sub_name, p) != 0) {
        if (s_sub_open) f_close(&s_sub_fil);
        if (f_open(&s_sub_fil, p, FA_READ) != FR_OK) { s_sub_open = false; return 0; }
        s_sub_clmt[0]   = sizeof(s_sub_clmt) / sizeof(s_sub_clmt[0]);
        s_sub_fil.cltbl = s_sub_clmt;
        if (f_lseek(&s_sub_fil, CREATE_LINKMAP) != FR_OK)
            s_sub_fil.cltbl = nullptr;   // too fragmented: fall back to FAT seeks
        strncpy(s_sub_name, p, PATH_LEN - 1); s_sub_name[PATH_LEN - 1] = 0;
        s_sub_open = true;
        s_sub_pos  = (FSIZE_t)-1;
    }
    if (s_sub_pos != (FSIZE_t)offset) {
        if (f_lseek(&s_sub_fil, (FSIZE_t)offset) != FR_OK) return 0;
        s_sub_pos = (FSIZE_t)offset;
    }
    UINT br = 0;
    if (f_read(&s_sub_fil, dst, (UINT)len, &br) != FR_OK) return 0;
    s_sub_pos += br;
    return br;
}

// --- third file cache: audio BIN (multi-file CUE) -----------------------------
// Like read_at() but on a separate FIL: on a multi-file CUE (data and audio in
// different .bin) the play's audio reads must NOT evict the data file from the
// read_at cache (which stays warm for any READ 0x02), otherwise each data<->audio
// switch rebuilds the LINKMAP (~80 ms) inside the I2S stream = glitch/underrun at
// track start and a reset window. Full CLMT like read_at (audio bins are as large
// as data ones). See storage.h.
static FIL     s_afil;
static char    s_afil_name[PATH_LEN] = {0};
static bool    s_afil_open = false;
static FSIZE_t s_afil_pos  = 0;
static DWORD   s_aclmt[256];

size_t RAM_FUNC(read_audio_at)(const char* name, uint64_t offset, uint8_t* dst, size_t len) {
    char p[PATH_LEN];
    resolve(name, p, sizeof(p));
    if (!s_afil_open || strcmp(s_afil_name, p) != 0) {
        if (s_afil_open) f_close(&s_afil);
        if (f_open(&s_afil, p, FA_READ) != FR_OK) { s_afil_open = false; return 0; }
        s_aclmt[0]    = sizeof(s_aclmt) / sizeof(s_aclmt[0]);
        s_afil.cltbl  = s_aclmt;
        if (f_lseek(&s_afil, CREATE_LINKMAP) != FR_OK)
            s_afil.cltbl = nullptr;   // too fragmented: fall back to FAT seeks
        strncpy(s_afil_name, p, PATH_LEN - 1); s_afil_name[PATH_LEN - 1] = 0;
        s_afil_open = true;
        s_afil_pos  = (FSIZE_t)-1;
    }
    if (s_afil_pos != (FSIZE_t)offset) {
        if (f_lseek(&s_afil, (FSIZE_t)offset) != FR_OK) return 0;
        s_afil_pos = (FSIZE_t)offset;
    }
    UINT br = 0;
    if (f_read(&s_afil, dst, (UINT)len, &br) != FR_OK) return 0;
    s_afil_pos += br;
    return br;
}

#if ENABLE_MPDISK
// --- .hdf hardfiles (mpdisk.device, 512 B block device) -----------------------
// The .hdf live in the SD ROOT; /games is for CDs only. A dedicated R/W FIL cache
// separate from the CD ones: HDD access and CD streaming are EXCLUSIVE in v1 (the
// Amiga device uses Forbid), but a private FIL avoids evicting the CD caches.
// FASTSEEK: the .hdf is preallocated at fixed size -> writes do not extend the
// file -> the CLMT stays valid.
static constexpr int HDF_MAX = 4;
static char s_hdfs[HDF_MAX][PATH_LEN];
static int  s_nhdf = 0;

// ONE handle per unit (opened lazily, left open): no f_close/f_open/CLMT on every
// unit switch. When working on MPD1 the C: commands are reread from MPD0, so the
// firmware constantly alternates between the two .hdf; with a SHARED handle each
// alternation redid close+open+CREATE_LINKMAP mid write stream = corruption on
// unit>0. Separate handles remove the alternation. FF_FS_TINY=0 -> each FIL has
// its own buffer = 4 open OK.
static FIL     s_hdf_fil [HDF_MAX];
static bool    s_hdf_open[HDF_MAX] = { false, false, false, false };
static FSIZE_t s_hdf_pos [HDF_MAX];
static DWORD   s_hdf_clmt[HDF_MAX][256];

static int cmp_str(const void* a, const void* b) {
    return strcasecmp((const char*)a, (const char*)b);
}

int scan_hardfiles() {
    s_nhdf = 0;
    if (!s_mounted) return 0;
    DIR dir;
    FILINFO fno;
    if (f_opendir(&dir, "/") != FR_OK) return 0;
    while (s_nhdf < HDF_MAX) {
        if (f_readdir(&dir, &fno) != FR_OK || fno.fname[0] == 0) break;
        if (fno.fattrib & AM_DIR) continue;
        size_t L = strlen(fno.fname);
        if (L > 4 && strcasecmp(fno.fname + L - 4, ".hdf") == 0) {
            snprintf(s_hdfs[s_nhdf], PATH_LEN, "/%s", fno.fname);
            s_nhdf++;
        }
    }
    f_closedir(&dir);
    qsort(s_hdfs, s_nhdf, PATH_LEN, cmp_str);
    return s_nhdf;
}

int hardfile_count() { return s_nhdf; }

const char* hardfile_name(int unit) {
    if (unit < 0 || unit >= s_nhdf) return nullptr;
    return s_hdfs[unit];
}

uint32_t hardfile_blocks(int unit) {
    const char* p = hardfile_name(unit);
    if (!p) return 0;
    FILINFO fno;
    if (f_stat(p, &fno) != FR_OK) return 0;
    return (uint32_t)((uint64_t)fno.fsize / HDF_BLOCK);
}

// open (once, lazily) the unit's .hdf in READ|WRITE and LEAVE it open. Returns
// true if ready. Builds the FASTSEEK CLMT at open.
static bool hardfile_ensure(int unit) {
    if (unit < 0 || unit >= HDF_MAX) return false;
    if (s_hdf_open[unit]) return true;
    const char* p = hardfile_name(unit);
    if (!p) return false;
    if (f_open(&s_hdf_fil[unit], p, FA_READ | FA_WRITE) != FR_OK) return false;
    s_hdf_clmt[unit][0]    = 256;               // CLMT capacity (entries)
    s_hdf_fil[unit].cltbl  = s_hdf_clmt[unit];
    if (f_lseek(&s_hdf_fil[unit], CREATE_LINKMAP) != FR_OK)
        s_hdf_fil[unit].cltbl = nullptr;        // too fragmented: classic FAT seeks
    s_hdf_open[unit] = true;
    s_hdf_pos[unit]  = (FSIZE_t)-1;
    return true;
}

uint32_t hardfile_read(int unit, uint32_t lba, uint8_t* dst, uint32_t nblk) {
    if (!hardfile_ensure(unit)) return 0;
    FIL*    fp  = &s_hdf_fil[unit];
    FSIZE_t off = (FSIZE_t)lba * HDF_BLOCK;
    if (s_hdf_pos[unit] != off) {
        if (f_lseek(fp, off) != FR_OK) return 0;
        s_hdf_pos[unit] = off;
    }
    UINT br = 0;
    if (f_read(fp, dst, nblk * HDF_BLOCK, &br) != FR_OK) return 0;
    s_hdf_pos[unit] += br;
    return (uint32_t)(br / HDF_BLOCK);
}

uint32_t hardfile_write(int unit, uint32_t lba, const uint8_t* src, uint32_t nblk) {
    if (!hardfile_ensure(unit)) return 0;
    FIL*    fp  = &s_hdf_fil[unit];
    FSIZE_t off = (FSIZE_t)lba * HDF_BLOCK;
    if (s_hdf_pos[unit] != off) {
        if (f_lseek(fp, off) != FR_OK) return 0;
        s_hdf_pos[unit] = off;
    }
    UINT bw = 0;
    if (f_write(fp, src, nblk * HDF_BLOCK, &bw) != FR_OK) return 0;
    s_hdf_pos[unit] += bw;
    f_sync(fp);                        // durability: flush FAT/directory on each write
    return (uint32_t)(bw / HDF_BLOCK);
}
#endif // ENABLE_MPDISK

bool write_text(const char* path, const char* text) {
    if (!s_mounted) return false;
    FIL f;
    if (f_open(&f, path, FA_WRITE | FA_CREATE_ALWAYS) != FR_OK) return false;
    UINT len = (UINT)strlen(text), bw = 0;
    FRESULT fr = f_write(&f, text, len, &bw);
    f_close(&f);
    return fr == FR_OK && bw == len;
}

// --- persistent "last disc" state (see storage.h) -----------------------------
static const char* STATE_FILE = "/matshita.sta";

void save_last_disc(int index) {
    char buf[16];
    snprintf(buf, sizeof buf, "%d\n", index);
    write_text(STATE_FILE, buf);
}

int load_last_disc() {
    if (!s_mounted) return -1;
    // direct f_open (NOT read_text: that calls set_base_dir and would dirty the
    // current cue's .bin base folder).
    FIL f;
    if (f_open(&f, STATE_FILE, FA_READ) != FR_OK) return -1;
    char buf[16]; UINT br = 0;
    f_read(&f, buf, sizeof(buf) - 1, &br);
    f_close(&f);
    buf[br] = 0;
    char* end = nullptr;
    long v = strtol(buf, &end, 10);
    if (end == buf) return -1;          // non-numeric content = ejected
    return (int)v;
}

} // namespace storage
