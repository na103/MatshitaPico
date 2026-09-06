// =============================================================================
// MatshitaPico -- Solid-state CD-ROM drive replacement for the Commodore CDTV
// Copyright (c) 2026 Nicola Avanzi (na103)
// Licensed under Creative Commons Attribution 4.0 International (CC BY 4.0).
// See LICENSE.
// =============================================================================
// subpw_block.h -- Assemble the P..W subcode "wire" block (CD+G).
// The subpw_tx PIO transmits 98 bytes per subcode block to the CPLD; each byte is
// the 8 bits P,Q,R,S,T,U,V,W (MSB-first, P=bit7) of ONE frame's SCCK burst:
//   [0..95]  = the 96 data frames (Q = the synthesised ADR=1 position packet, 1
//              bit/frame; R..W = CD+G graphics from the CloneCD .sub if present,
//              else zeros; P always 0)
//   [96]     = 0xFF  -> S0 sync-frame pattern
//   [97]     = 0x00  -> S1 filler (the motherboard suppresses that burst)
// The CloneCD .sub is DEINTERLEAVED per channel: P=bytes 0..11, Q=12..23,
// R=24..35, S=36..47, T=48..59, U=60..71, V=72..83, W=84..95; each channel = 96
// bits MSB-first (bit f = frame f). The .sub R-W is the disc's RAW subcode
// (interleave and pack parity included): pass it through as-is; the host decoder
// (cdg.library) does the de-interleave/correction. Q is still synthesised from the
// playback position (consistent with our player); the .sub's Q is unused.
// Pure header (no Pico SDK): used by subcode.cpp and the PC tests.
// =============================================================================
#pragma once
#include <cstdint>
#include "subq_packet.h"

namespace subpw {

static constexpr int BLOCK_BYTES = 98;   // 96 data + S0 + S1 filler
static constexpr int SUB_BYTES   = 96;   // CloneCD subchannel sector

// Extract the 6 R..W bits of frame f (0..95) from a CloneCD .sub sector.
inline uint8_t rw6_from_clonecd(const uint8_t sub96[SUB_BYTES], int f) {
    uint8_t v = 0;
    for (int c = 0; c < 6; ++c)          // channels R..W = 2..7 (offset 24 + 12*c)
        v = (uint8_t)((v << 1) |
                      ((sub96[24 + 12 * c + (f >> 3)] >> (7 - (f & 7))) & 1));
    return v;
}

// Build the 98-byte wire block. sub96 = .sub sector (nullptr = R-W all 0).
inline void build_wire_block(const uint8_t q_pkt[subq::PACKET_BYTES],
                             const uint8_t* sub96, uint8_t out[BLOCK_BYTES]) {
    for (int f = 0; f < 96; ++f) {
        uint8_t qb = (uint8_t)((q_pkt[f >> 3] >> (7 - (f & 7))) & 1);
        uint8_t rw = sub96 ? rw6_from_clonecd(sub96, f) : (uint8_t)0;
        out[f] = (uint8_t)((qb << 6) | rw);        // P (bit7) = 0
    }
    out[96] = 0xFF;                                // S0
    out[97] = 0x00;                                // S1: don't care (suppressed)
}

} // namespace subpw
