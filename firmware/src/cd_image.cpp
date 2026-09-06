// =============================================================================
// MatshitaPico -- Solid-state CD-ROM drive replacement for the Commodore CDTV
// Copyright (c) 2026 Nicola Avanzi (na103)
// Licensed under Creative Commons Attribution 4.0 International (CC BY 4.0).
// See LICENSE.
// =============================================================================
// cd_image.cpp -- see cd_image.h.
// The real CUE parser is in cue_parser.cpp (SDK-free, tested). Here we glue it to
// the filesystem (storage) and map LSN -> file/offset for reads.
// =============================================================================
#include "cd_image.h"
#include "cue_parser.h"
#include "storage.h"
#include "ram_func.h"
#include <cstring>

namespace cd_image {

static Toc      s_toc;
static int      s_count = 0;
static int      s_index = -1;
static uint32_t s_pos_lsn = 0;

// parser callback: BIN size from the filesystem
static uint64_t cb_size(const char* name, void* /*ctx*/) {
    return storage::file_size(name);
}

// ---- CloneCD subchannel (.sub beside the image, CD+G) ------------------------
// Single-file CUEs only (CloneCD rips have one .img; multi-bin Redump sets have no
// subchannel anyway). The .sub is valid if its size is EXACTLY sectors*96.
static bool s_has_sub = false;
static char s_sub_name[MAX_NAME] = {0};
static bool s_has_data  = false;  // the disc has at least one DATA track
static bool s_has_audio = false;  // ... and at least one AUDIO track (-> mixed)
static bool s_subok     = false;  // <name>.subok marker: force subcode ON on a mixed disc

static void detect_sub() {
    s_has_sub = false;
    s_sub_name[0] = 0;
    if (s_toc.n_files != 1 || s_toc.files[0].size_bytes == 0) return;
    // <name>.<ext> -> <name>.sub
    const char* bin = s_toc.files[0].name;
    size_t L = strlen(bin);
    if (L + 1 > sizeof(s_sub_name)) return;
    strcpy(s_sub_name, bin);
    char* dot = strrchr(s_sub_name, '.');
    if (!dot || (size_t)(dot - s_sub_name) + 5 > sizeof(s_sub_name)) return;
    strcpy(dot, ".sub");
    uint64_t want = (s_toc.files[0].size_bytes / RAW_SECTOR) * SUB_SECTOR;
    s_has_sub = (want != 0) && (storage::file_size(s_sub_name) == want);
}

bool has_sub() { return s_has_sub; }

const char* sub_name() { return s_sub_name; }

bool has_data_track() { return s_has_data; }

// Per-disc subcode gate: OFF ONLY on MIXED discs (data + audio, like Sherlock,
// which reads Q via port A during play and crashes with ANY subcode). ON on
// everything else: CD+G (.sub), all-audio discs, and pure DATA discs (they do not
// play audio -> never read Q -> do not crash, and keep the activity LED alive).
// OVERRIDE: a <name>.subok marker beside the .cue forces ON even on a mixed disc
// the user knows is safe (e.g. Xenon 2) -> LED alive, at their own risk.
bool subcode_wanted() { return s_has_sub || s_subok || !(s_has_data && s_has_audio); }

static bool in_pregap(const Track* t, uint64_t off);   // defined below

bool read_sub(uint32_t lsn, uint8_t dst[SUB_SECTOR]) {
    if (!s_has_sub) return false;
    const Track* t = track_for_lsn(lsn);
    if (!t || lsn < t->start_lsn) return false;   // pregap R-W = zeros (no graphics)
    // same in-file sector index as the image (file_offset is always a multiple of
    // 2352: CUE INDEX values are whole frames)
    uint64_t off = t->file_offset + (uint64_t)(lsn - t->start_lsn) * RAW_SECTOR;
    if (in_pregap(t, off)) return false;          // PREGAP gap: R-W = zeros
    uint64_t sec = off / RAW_SECTOR;
    return storage::read_sub_at(s_sub_name, sec * SUB_SECTOR, dst, SUB_SECTOR)
           == SUB_SECTOR;
}

// Mount and scan; loads no disc: boots EJECTED (like a real drive with no caddy).
// The first NEXT inserts disc [0] (s_index = -1, or wrap-around after TOC check).
bool boot() {
    if (!storage::mount()) return false;
    s_count = storage::scan_cues();
    return s_count > 0;
}

int count()         { return s_count; }
int current_index() { return s_index; }

bool load(int index) {
    if (index < 0 || index >= s_count) return false;
    const char* path = storage::cue_name(index);
    if (!path) return false;

    static char cue_text[16384];   // multi-bin Redump 99-track cue ~14 KB
    size_t len = storage::read_text(path, cue_text, sizeof(cue_text));
    if (len == 0) return false;

    static Toc tmp;   // ~16 KB (99 tracks + 99 files): too large for the stack
    if (!cue::parse(cue_text, len, tmp, cb_size, nullptr)) return false;

    s_toc   = tmp;
    s_index = index;
    s_pos_lsn = 0;
    detect_sub();      // CloneCD subchannel beside the image? (CD+G)

    // Per-disc subcode gate: classify the disc (data and/or audio).
    s_has_data = s_has_audio = false;
    for (int i = 0; i < s_toc.n_tracks; ++i) {
        if (s_toc.tracks[i].control & 0x04) s_has_data  = true;   // data track
        else                                s_has_audio = true;   // audio track
    }
    // Override: <name>.subok marker beside the .cue -> force subcode ON on a mixed
    // disc the user knows is safe (Xenon 2). Empty file = just needs to exist.
    s_subok = false;
    {
        char mk[256];
        strncpy(mk, path, sizeof(mk) - 1);
        mk[sizeof(mk) - 1] = 0;
        char* dot = strrchr(mk, '.');                       // .cue -> .subok
        if (dot && (size_t)(dot - mk) + 7 <= sizeof(mk)) {
            strcpy(dot, ".subok");
            s_subok = storage::file_exists(mk);
        }
    }
    return true;
}

const Toc& toc() { return s_toc; }

const Track* RAM_FUNC(track_for_lsn)(uint32_t lsn) {
    const Track* hit = nullptr;
    for (int i = 0; i < s_toc.n_tracks; ++i) {
        if (s_toc.tracks[i].start_lsn <= lsn) hit = &s_toc.tracks[i];
        else break;  // tracks in increasing start_lsn order
    }
    if (hit && lsn <= s_toc.last_lsn) return hit;
    return nullptr;
}

bool seek(uint32_t lsn) {
    if (s_toc.last_lsn && lsn > s_toc.last_lsn) return false;
    s_pos_lsn = lsn;
    return true;
}

// LSN inside a PREGAP declared in the CUE (a gap on the disc NOT present in the
// file): the file continues with the next track's data, so the offset computed
// from the previous track would point at SHIFTED data. The real drive/WinUAE
// deliver zero sectors there.
static bool RAM_FUNC(in_pregap)(const Track* t, uint64_t off) {
    int ti = (int)(t - s_toc.tracks);
    if (ti + 1 >= s_toc.n_tracks) return false;          // last track: end of file
    const Track& n = s_toc.tracks[ti + 1];
    return n.file_index == t->file_index && off >= n.file_offset;
}

// Shared body of read_raw / read_raw_audio: identical LSN->file/offset mapping and
// PREGAP handling; only the storage file cache used for the f_read differs.
//   audio_cache=false -> storage::read_at        (DATA / host DMA path)
//   audio_cache=true  -> storage::read_audio_at  (AUDIO / I2S play path)
// On a multi-file CUE this keeps the data bin and the audio bin warm TOGETHER (no
// LINKMAP rebuild when switching data<->audio, see storage.h).
static size_t RAM_FUNC(read_raw_impl)(uint8_t* dst, uint32_t n_sectors, bool audio_cache) {
    size_t total = 0;
    for (uint32_t i = 0; i < n_sectors; ++i) {
        const Track* t = track_for_lsn(s_pos_lsn);
        if (!t) break;
        // offset of the sector in its own file
        uint64_t off = t->file_offset + (uint64_t)(s_pos_lsn - t->start_lsn) * RAW_SECTOR;
        if (in_pregap(t, off)) {                         // gap: zero sector
            memset(dst + total, 0, RAW_SECTOR);
            total += RAW_SECTOR;
            s_pos_lsn++;
            continue;
        }
        const char* name = s_toc.files[t->file_index].name;
        size_t got = audio_cache
            ? storage::read_audio_at(name, off, dst + total, RAW_SECTOR)
            : storage::read_at      (name, off, dst + total, RAW_SECTOR);
        total += got;
        s_pos_lsn++;
        if (got < RAW_SECTOR) break;  // end of file / error
    }
    return total;
}

size_t RAM_FUNC(read_raw)(uint8_t* dst, uint32_t n_sectors) {
    return read_raw_impl(dst, n_sectors, /*audio_cache=*/false);
}

size_t RAM_FUNC(read_raw_audio)(uint8_t* dst, uint32_t n_sectors) {
    return read_raw_impl(dst, n_sectors, /*audio_cache=*/true);
}

// Index of the first track of a given type (data if want_data, else audio);
// -1 if the disc has none. control & 0x04 = DATA track.
static int first_track_of(bool want_data) {
    for (int i = 0; i < s_toc.n_tracks; ++i) {
        bool is_data = (s_toc.tracks[i].control & 0x04) != 0;
        if (is_data == want_data) return i;
    }
    return -1;
}

void prewarm() {
    if (s_toc.n_tracks <= 0) return;
    // Warm the file caches by reading 1 sector: open the file, build the FAT
    // LINKMAP and bring the FatFs path into the XIP cache, so the first steady-
    // state access does not pay the cold latency (~80 ms on E.S.S. Mega) inside
    // the audio/host stream. On a multi-file CUE, data and audio are in different
    // .bin with separate caches (read_at vs read_audio_at): warm BOTH so they stay
    // warm together and no data<->audio switch rebuilds the LINKMAP.
    static uint8_t tmp[RAW_SECTOR];
    int di = first_track_of(/*want_data=*/true);
    int ai = first_track_of(/*want_data=*/false);
    if (di >= 0) { seek(s_toc.tracks[di].start_lsn); read_raw      (tmp, 1); }
    if (ai >= 0) { seek(s_toc.tracks[ai].start_lsn); read_raw_audio(tmp, 1); }
    // leave the cursor wherever it lands: each play/read seeks before reading.
}

} // namespace cd_image
