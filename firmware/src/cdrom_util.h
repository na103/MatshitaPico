// =============================================================================
// MatshitaPico -- Solid-state CD-ROM drive replacement for the Commodore CDTV
// Copyright (c) 2026 Nicola Avanzi (na103)
// Licensed under Creative Commons Attribution 4.0 International (CC BY 4.0).
// See LICENSE.
// =============================================================================
// cdrom_util.h -- Address/BCD conversions (ported from WinUAE blkdev).
// Packed MSF = 3 binary bytes: (minutes<<16)|(seconds<<8)|frame. 75 frames/s, a
// 150-frame (2 s lead-in) offset between MSF and LSN. See docs/protocol-cdtv.md.
// =============================================================================
#pragma once
#include <cstdint>

namespace cdrom {

constexpr int LEADIN_FRAMES = 150;  // 2 s

// packed MSF -> LSN
inline int msf2lsn(int msf) {
    int s = (msf >> 16) & 0xff;     // minutes
    s = s * 60 + ((msf >> 8) & 0xff);
    s = s * 75 + (msf & 0xff);
    return s - LEADIN_FRAMES;
}

// frame count -> packed MSF WITHOUT lead-in: for track-RELATIVE times (they start
// at 00:00, like the Red Book Q-code). Using lsn2msf for the relative time would
// add +2 s to the player counter, so "<<" would never reach the previous track
// because the time was never 00:00.
inline int frames2msf(int frames) {
    int m = (frames / (75 * 60));
    int s = (frames / 75) % 60;
    int f = (frames % 75);
    return (m << 16) | (s << 8) | f;
}

// LSN -> ABSOLUTE disc packed MSF (includes the 2 s lead-in)
inline int lsn2msf(int lsn) {
    return frames2msf(lsn + LEADIN_FRAMES);
}

// BCD
inline uint8_t tobcd(uint8_t v)   { return (uint8_t)(((v / 10) << 4) | (v % 10)); }
inline uint8_t frombcd(uint8_t v) { return (uint8_t)((v >> 4) * 10 + (v & 0x0f)); }

} // namespace cdrom
