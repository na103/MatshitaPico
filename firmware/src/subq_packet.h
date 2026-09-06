// =============================================================================
// MatshitaPico -- Solid-state CD-ROM drive replacement for the Commodore CDTV
// Copyright (c) 2026 Nicola Avanzi (na103)
// Licensed under Creative Commons Attribution 4.0 International (CC BY 4.0).
// See LICENSE.
// =============================================================================
// subq_packet.h -- Build the subcode Q packet (Red Book, ADR=1).
// Physical Q packet = 96 bits = 12 bytes, transmitted MSB-first:
//   [0] CONTROL(4) | ADR(4)      ADR=1 = current position
//   [1] TNO   track (BCD)
//   [2] INDEX                (BCD; 00 = pregap)
//   [3..5] MIN/SEC/FRAME     position RELATIVE to the track (BCD)
//   [6] ZERO
//   [7..9] AMIN/ASEC/AFRAME  ABSOLUTE disc position (BCD, = LSN + 150)
//   [10..11] CRC-16-CCITT (x^16+x^12+x^5+1, init 0) over the first 10 bytes,
//            transmitted INVERTED (one's complement), MSB-first.
// Pure header (no Pico SDK): used by subcode.cpp and the PC tests. LSN/MSF/BCD
// conversions from cdrom_util.h.
// =============================================================================
#pragma once
#include <cstdint>
#include "cdrom_util.h"

namespace subq {

static constexpr int PACKET_BYTES = 12;

// CRC-16-CCITT (poly 0x1021, init 0x0000), bit-by-bit MSB-first.
inline uint16_t crc16_ccitt(const uint8_t* p, int n) {
    uint16_t crc = 0;
    for (int i = 0; i < n; ++i) {
        crc ^= (uint16_t)p[i] << 8;
        for (int b = 0; b < 8; ++b)
            crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021) : (uint16_t)(crc << 1);
    }
    return crc;
}

// frame count (binary, no offset) -> 3 BCD bytes MIN/SEC/FRAME.
inline void frames_to_msf_bcd(uint32_t frames, uint8_t* out) {
    out[0] = cdrom::tobcd((uint8_t)(frames / (75 * 60)));
    out[1] = cdrom::tobcd((uint8_t)((frames / 75) % 60));
    out[2] = cdrom::tobcd((uint8_t)(frames % 75));
}

// Build the ADR=1 (position) Q packet for the current audio position.
//   control   : track CONTROL nibble (0x0 = audio, 0x4 = data, bit0 = pre-emphasis)
//   track     : track number (binary, 1..99)
//   index     : 0 = pregap, 1.. = index (binary)
//   rel_frames: track-relative position in frames (binary, >= 0)
//   abs_lsn   : absolute disc position as LSN (AMSF adds the 150 lead-in frames)
// out = 12 bytes ready for MSB-first serialisation.
inline void build_position_packet(uint8_t control, uint8_t track, uint8_t index,
                                  uint32_t rel_frames, uint32_t abs_lsn,
                                  uint8_t out[PACKET_BYTES]) {
    out[0] = (uint8_t)((control << 4) | 0x01);          // CONTROL | ADR=1
    out[1] = cdrom::tobcd(track);
    out[2] = cdrom::tobcd(index);
    frames_to_msf_bcd(rel_frames, out + 3);             // relative: from track start
    out[6] = 0;
    frames_to_msf_bcd(abs_lsn + cdrom::LEADIN_FRAMES, out + 7);  // absolute (= LSN+150)
    uint16_t crc = (uint16_t)~crc16_ccitt(out, 10);     // inverted CRC (Red Book)
    out[10] = (uint8_t)(crc >> 8);
    out[11] = (uint8_t)(crc & 0xff);
}

// Pack the 12 bytes into 3 big-endian-in-word 32-bit words for the subq_tx PIO
// TX FIFO (left shift: bit 31 = MSB of the first byte goes out first).
inline void packet_to_words(const uint8_t pkt[PACKET_BYTES], uint32_t w[3]) {
    for (int i = 0; i < 3; ++i)
        w[i] = ((uint32_t)pkt[i*4] << 24) | ((uint32_t)pkt[i*4+1] << 16) |
               ((uint32_t)pkt[i*4+2] << 8) | (uint32_t)pkt[i*4+3];
}

} // namespace subq
