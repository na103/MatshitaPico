// =============================================================================
// MatshitaPico -- Solid-state CD-ROM drive replacement for the Commodore CDTV
// Copyright (c) 2026 Nicola Avanzi (na103)
// Licensed under Creative Commons Attribution 4.0 International (CC BY 4.0).
// See LICENSE.
// =============================================================================
// cue_parser.h -- CUE sheet parser (SDK-free, PC-testable).
// Fills a cd_image::Toc from .cue text. It never touches the filesystem: BIN file
// sizes (needed for multi-file absolute LSNs and for capacity) arrive via a
// callback, keeping the parser pure and unit-testable. Project assumption: 2352
// raw images (1 frame = 2352 bytes), so per-file frame count = size_bytes / 2352.
// =============================================================================
#pragma once
#include "cd_image.h"
#include <cstdint>
#include <cstddef>

namespace cue {

// Returns the byte size of BIN file 'name' (0 if unknown).
typedef uint64_t (*FileSizeFn)(const char* name, void* ctx);

// Convert mm:ss:ff to frames (= sectors). 75 frames/s.
uint32_t msf_to_frames(int m, int s, int f);

// Parse the cue text. Returns true if at least one valid track was found.
// 'size_fn' may be nullptr (multi-file LSNs and last_lsn stay partial).
bool parse(const char* text, size_t len, cd_image::Toc& out,
           FileSizeFn size_fn = nullptr, void* ctx = nullptr);

} // namespace cue
