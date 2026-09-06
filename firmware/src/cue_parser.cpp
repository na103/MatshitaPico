// =============================================================================
// MatshitaPico -- Solid-state CD-ROM drive replacement for the Commodore CDTV
// Copyright (c) 2026 Nicola Avanzi (na103)
// Licensed under Creative Commons Attribution 4.0 International (CC BY 4.0).
// See LICENSE.
// =============================================================================
// cue_parser.cpp -- see cue_parser.h.
// Handles: FILE "x.bin" BINARY | TRACK nn MODE1/2352|MODE2/2352|AUDIO |
//          INDEX 00/01 mm:ss:ff | PREGAP mm:ss:ff. Ignores REM/CATALOG/TITLE/...
// LSN model: for each FILE, in-file frames map to the disc as
//   disc_lsn = file_base_lsn + gap_acc + frame_in_file
// where file_base_lsn accumulates the previous files' lengths (size/2352) and
// gap_acc accumulates the PREGAPs (silence sectors on the disc, absent from the
// file). For the common case (single BIN, no PREGAP) -> start_lsn = frame(INDEX
// 01), file_offset = frame(INDEX 01) * sector_size: absolute disc addresses.
// =============================================================================
#include "cue_parser.h"
#include <cstring>
#include <cstdlib>
#include <cctype>
#include <cstdio>

namespace cue {

uint32_t msf_to_frames(int m, int s, int f) {
    return (uint32_t)(((m * 60 + s) * 75) + f);
}

// skip leading spaces/tabs
static const char* skip_ws(const char* p) {
    while (*p == ' ' || *p == '\t') ++p;
    return p;
}

// case-insensitive keyword compare, advances p on a match
static bool match_kw(const char*& p, const char* kw) {
    size_t n = strlen(kw);
    for (size_t i = 0; i < n; ++i)
        if (toupper((unsigned char)p[i]) != kw[i]) return false;
    // must be followed by space/end
    char c = p[n];
    if (c && c != ' ' && c != '\t') return false;
    p = skip_ws(p + n);
    return true;
}

// extract a quoted name (or a whitespace-free token)
static void parse_name(const char* p, char* dst, size_t cap) {
    p = skip_ws(p);
    size_t i = 0;
    if (*p == '"') {
        ++p;
        while (*p && *p != '"' && i + 1 < cap) dst[i++] = *p++;
    } else {
        while (*p && *p != ' ' && *p != '\t' && i + 1 < cap) dst[i++] = *p++;
    }
    dst[i] = 0;
}

// mm:ss:ff -> frames
static bool parse_msf(const char* p, uint32_t* out) {
    int m, s, f;
    if (sscanf(skip_ws(p), "%d:%d:%d", &m, &s, &f) != 3) return false;
    *out = msf_to_frames(m, s, f);
    return true;
}

bool parse(const char* text, size_t len, cd_image::Toc& out,
           FileSizeFn size_fn, void* ctx) {
    using namespace cd_image;
    out = Toc{};

    int      cur_file   = -1;     // index into out.files
    uint64_t file_base  = 0;      // LSN of the current file's first sector
    uint32_t gap_acc    = 0;      // accumulated PREGAPs (frames)
    int      cur_trk    = -1;     // index into out.tracks
    bool     idx0_seen  = false;

    char line[256];
    size_t pos = 0;
    while (pos < len) {
        // extract a line
        size_t e = pos;
        while (e < len && text[e] != '\n' && text[e] != '\r') ++e;
        size_t n = e - pos;
        if (n >= sizeof(line)) n = sizeof(line) - 1;
        memcpy(line, text + pos, n);
        line[n] = 0;
        pos = e;
        while (pos < len && (text[pos] == '\n' || text[pos] == '\r')) ++pos;

        const char* p = skip_ws(line);
        if (!*p) continue;

        if (match_kw(p, "FILE")) {
            // close the previous file's count
            if (cur_file >= 0) {
                uint64_t sz = out.files[cur_file].size_bytes;
                file_base += (sz / RAW_SECTOR);
            }
            if (out.n_files < MAX_FILES) {
                cur_file = out.n_files++;
                parse_name(p, out.files[cur_file].name, MAX_NAME);
                out.files[cur_file].size_bytes =
                    size_fn ? size_fn(out.files[cur_file].name, ctx) : 0;
                // missing/unreadable bin -> unusable TOC (later start_lsn and
                // last_lsn depend on sizes): fail NOW and loudly instead of
                // producing a TOC with last_lsn=0.
                if (size_fn && out.files[cur_file].size_bytes == 0) return false;
            }
            idx0_seen = false;
        }
        else if (match_kw(p, "TRACK")) {
            if (out.n_tracks < MAX_TRACKS) {
                cur_trk = out.n_tracks++;
                Track& t = out.tracks[cur_trk];
                t = Track{};
                t.number     = (uint8_t)atoi(p);            // "01", "02", ...
                t.file_index = (uint8_t)(cur_file < 0 ? 0 : cur_file);
                // track type (after the number)
                const char* q = p;
                while (*q && *q != ' ' && *q != '\t') ++q;  // skip the number
                q = skip_ws(q);
                if (strncmp(q, "AUDIO", 5) == 0) {
                    t.control = 0x00;
                    t.sector_size = RAW_SECTOR;
                } else {                                     // MODE1/MODE2 = data
                    t.control = 0x04;
                    const char* slash = strchr(q, '/');
                    t.sector_size = slash ? (uint16_t)atoi(slash + 1) : RAW_SECTOR;
                    if (t.sector_size == 0) t.sector_size = RAW_SECTOR;
                }
            }
            idx0_seen = false;
        }
        else if (match_kw(p, "PREGAP")) {
            uint32_t g;
            if (parse_msf(p, &g)) gap_acc += g;             // gap not present in the file
        }
        else if (match_kw(p, "INDEX")) {
            int idx = atoi(p);
            const char* q = p;
            while (*q && *q != ' ' && *q != '\t') ++q;       // skip the index number
            uint32_t fr;
            if (cur_trk >= 0 && parse_msf(q, &fr)) {
                Track& t = out.tracks[cur_trk];
                uint32_t lsn = (uint32_t)(file_base + gap_acc + fr);
                if (idx == 0) {
                    t.pregap_lsn = lsn;
                    idx0_seen = true;
                } else if (idx == 1) {
                    t.start_lsn   = lsn;
                    t.file_offset = (uint64_t)fr * t.sector_size;
                    if (!idx0_seen) t.pregap_lsn = lsn;
                }
            }
        }
        // REM / CATALOG / TITLE / PERFORMER / FLAGS / POSTGAP: ignored
    }

    if (out.n_tracks == 0) return false;

    // TOC metadata
    out.first_track = out.tracks[0].number;
    out.last_track  = out.tracks[out.n_tracks - 1].number;
    out.data_first  = (out.tracks[0].control & 0x04) != 0;

    // end of disc = last file base + its length + accumulated gaps
    if (cur_file >= 0) {
        uint64_t sz = out.files[cur_file].size_bytes;
        out.last_lsn = (uint32_t)(file_base + (sz / RAW_SECTOR) + gap_acc);
    }
    return true;
}

} // namespace cue
