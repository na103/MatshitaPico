#!/usr/bin/env python3
# =============================================================================
# MatshitaPico -- Solid-state CD-ROM drive replacement for the Commodore CDTV
# Copyright (c) 2026 Nicola Avanzi (na103)
# Licensed under Creative Commons Attribution 4.0 International (CC BY 4.0).
# See LICENSE.
# =============================================================================
"""iso2bincue.py -- converts an ISO (Mode 1, 2048 B/sector) into a raw 2352 BIN/CUE.

MatshitaPico only accepts raw 2352 BIN/CUE: an ISO is just the user-data field,
so the physical CD-ROM Mode 1 format is regenerated for every sector:

    offset  size  contents
    0x000     12  sync (00 FF*10 00)
    0x00C      3  MSF address in BCD (LBA+150)
    0x00F      1  mode = 01
    0x010   2048  user data (from the ISO)
    0x810      4  EDC (32-bit CRC, reflected poly 0xD8018001, over 0x000-0x80F)
    0x814      8  reserved (zero)
    0x81C    172  P parity (RS over GF(256), poly 0x11D, over 0x00C-0x81B)
    0x8C8    104  Q parity (same, diagonals)

EDC/ECC = the canonical ecm/cdrtools algorithm (Neill Corlett), vectorised with
numpy over the sectors of a batch (a 650 MB ISO converts in tens of seconds).
Also used as a library by mkbootcd.py (build_batch).

Usage:
    python3 iso2bincue.py image.iso [output_base]
        -> output_base.bin + output_base.cue (default: same name as the ISO)
    python3 iso2bincue.py --check image.bin
        -> verifies sync/MSF/EDC/ECC of an existing Mode1/2352 BIN
"""

import os
import sys

import numpy as np

SECTOR_RAW = 2352
SECTOR_ISO = 2048
LEADIN = 150            # standard MSF offset: LBA 0 = 00:02:00
BATCH = 8192            # sectors per batch (~18 MB raw)

SYNC = np.frombuffer(b"\x00" + b"\xff" * 10 + b"\x00", dtype=np.uint8)

# ---------------------------------------------------------------- EDC/ECC tables


def _build_luts():
    edc = np.zeros(256, dtype=np.uint32)
    for i in range(256):
        v = i
        for _ in range(8):
            v = (v >> 1) ^ (0xD8018001 if (v & 1) else 0)
        edc[i] = v
    ecc_f = np.zeros(256, dtype=np.uint8)
    ecc_b = np.zeros(256, dtype=np.uint8)
    for i in range(256):
        j = ((i << 1) ^ (0x11D if (i & 0x80) else 0)) & 0xFF
        ecc_f[i] = j
        ecc_b[i ^ j] = i
    return edc, ecc_f, ecc_b


EDC_LUT, ECC_F, ECC_B = _build_luts()


# ---------------------------------------------------------------- per-batch computation


def edc_batch(raw):
    """EDC of the sectors: raw = (N, 2352) uint8; covers bytes 0x000-0x80F."""
    n = raw.shape[0]
    edc = np.zeros(n, dtype=np.uint32)
    for j in range(0x810):
        edc = (edc >> 8) ^ EDC_LUT[(edc ^ raw[:, j]) & 0xFF]
    return edc


def _ecc_block(block, major_count, minor_count, major_mult, minor_inc):
    """One parity plane (P or Q) for all sectors of the batch.

    block = (N, >=major*minor) uint8 starting at byte 0x00C of the raw sector.
    P covers 86*24=2064 bytes (header+data+EDC+reserved); Q covers 52*43=2236
    bytes = the same 2064 PLUS the P parity -> P must be written BEFORE computing
    Q. 1:1 port of ecm's ecc_computeblock.
    """
    n = block.shape[0]
    size = major_count * minor_count
    out = np.empty((n, major_count * 2), dtype=np.uint8)
    for major in range(major_count):
        index = (major >> 1) * major_mult + (major & 1)
        ecc_a = np.zeros(n, dtype=np.uint8)
        ecc_b = np.zeros(n, dtype=np.uint8)
        for _ in range(minor_count):
            temp = block[:, index]
            index += minor_inc
            if index >= size:
                index -= size
            ecc_a ^= temp
            ecc_b ^= temp
            ecc_a = ECC_F[ecc_a]
        ecc_a = ECC_B[ECC_F[ecc_a] ^ ecc_b]
        out[:, major] = ecc_a
        out[:, major + major_count] = ecc_a ^ ecc_b
    return out


def ecc_p(raw):
    """P parity (172 B): raw = (N, 2352) uint8, uses bytes 0x00C-0x81B."""
    return _ecc_block(raw[:, 0x00C:0x81C], 86, 24, 2, 86)


def ecc_q(raw):
    """Q parity (104 B): uses 0x00C-0x8C7 (P included: must already be written)."""
    return _ecc_block(raw[:, 0x00C:0x8C8], 52, 43, 86, 88)


def msf_bcd(lba0, n):
    """BCD MSF header for n sectors starting at LBA lba0: (n, 3) uint8."""
    frames = np.arange(lba0, lba0 + n, dtype=np.int64) + LEADIN
    m = frames // 4500
    s = (frames % 4500) // 75
    f = frames % 75
    bcd = lambda v: ((v // 10) << 4 | (v % 10)).astype(np.uint8)
    return np.stack([bcd(m), bcd(s), bcd(f)], axis=1)


def build_batch(iso_data, lba0):
    """Build the complete raw sectors from a batch of ISO data (N*2048 bytes)."""
    n = len(iso_data) // SECTOR_ISO
    raw = np.zeros((n, SECTOR_RAW), dtype=np.uint8)
    raw[:, 0:12] = SYNC
    raw[:, 12:15] = msf_bcd(lba0, n)
    raw[:, 15] = 0x01
    raw[:, 16:0x810] = np.frombuffer(iso_data, dtype=np.uint8).reshape(n, SECTOR_ISO)
    edc = edc_batch(raw)
    for k in range(4):
        raw[:, 0x810 + k] = (edc >> (8 * k)) & 0xFF
    raw[:, 0x81C:0x8C8] = ecc_p(raw)
    raw[:, 0x8C8:SECTOR_RAW] = ecc_q(raw)
    return raw


# ---------------------------------------------------------------- conversion


def convert(iso_path, out_base=None):
    size = os.path.getsize(iso_path)
    if size == 0:
        sys.exit(f"error: {iso_path} is empty")
    pad = (-size) % SECTOR_ISO
    if pad:
        print(f"warning: size not a multiple of 2048 ({size} B): "
              f"last sector padded with {pad} zero bytes")
    total = (size + pad) // SECTOR_ISO

    if out_base is None:
        out_base = os.path.splitext(iso_path)[0]
    bin_path = out_base + ".bin"
    cue_path = out_base + ".cue"
    if os.path.abspath(bin_path) == os.path.abspath(iso_path):
        sys.exit("error: the output would overwrite the input")

    lba = 0
    with open(iso_path, "rb") as fin, open(bin_path, "wb") as fout:
        while True:
            chunk = fin.read(BATCH * SECTOR_ISO)
            if not chunk:
                break
            if len(chunk) % SECTOR_ISO:
                chunk += b"\x00" * ((-len(chunk)) % SECTOR_ISO)
            raw = build_batch(chunk, lba)
            fout.write(raw.tobytes())
            lba += raw.shape[0]
            print(f"\r  {lba}/{total} sectors", end="", flush=True)
    print()

    with open(cue_path, "w", newline="\r\n") as f:
        f.write(f'FILE "{os.path.basename(bin_path)}" BINARY\n')
        f.write("  TRACK 01 MODE1/2352\n")
        f.write("    INDEX 01 00:00:00\n")

    mm, ss = divmod(total // 75, 60)
    print(f"OK: {bin_path} ({total * SECTOR_RAW} B, {total} sectors, "
          f"{mm:02d}:{ss:02d}) + {cue_path}")


# ---------------------------------------------------------------- BIN check


def check(bin_path):
    size = os.path.getsize(bin_path)
    if size % SECTOR_RAW:
        print(f"warning: size not a multiple of 2352 ({size} B) -- not a raw BIN?")
    total = size // SECTOR_RAW
    bad_sync = bad_msf = bad_edc = bad_ecc = data_sectors = 0
    lba = 0
    with open(bin_path, "rb") as f:
        while True:
            chunk = f.read(BATCH * SECTOR_RAW)
            if len(chunk) < SECTOR_RAW:
                break
            n = len(chunk) // SECTOR_RAW
            raw = np.frombuffer(chunk[: n * SECTOR_RAW], dtype=np.uint8).reshape(
                n, SECTOR_RAW)
            # data sectors = those starting with sync (the rest = audio, skipped)
            is_data = np.all(raw[:, 0:12] == SYNC, axis=1) & (raw[:, 15] == 0x01)
            idx = np.nonzero(is_data)[0]
            data_sectors += len(idx)
            bad_sync += int(np.count_nonzero(~is_data & (raw[:, 15] == 0x01)))
            if len(idx):
                d = raw[idx]
                exp_msf = msf_bcd(lba, n)[idx]
                mm = ~np.all(d[:, 12:15] == exp_msf, axis=1)
                bad_msf += int(np.count_nonzero(mm))
                edc = edc_batch(d)
                stored = (d[:, 0x810].astype(np.uint32)
                          | d[:, 0x811].astype(np.uint32) << 8
                          | d[:, 0x812].astype(np.uint32) << 16
                          | d[:, 0x813].astype(np.uint32) << 24)
                ee = edc != stored
                bad_edc += int(np.count_nonzero(ee))
                # Q is checked on the sector as is (stored P included)
                ce = (~np.all(d[:, 0x81C:0x8C8] == ecc_p(d), axis=1)) | \
                     (~np.all(d[:, 0x8C8:] == ecc_q(d), axis=1))
                bad_ecc += int(np.count_nonzero(ce))
                for i in idx[np.nonzero(ee | ce)[0]][:10]:
                    print(f"  sector LBA {lba + int(i)}: "
                          f"{'EDC ' if ee[list(idx).index(i)] else ''}"
                          f"{'ECC' if ce[list(idx).index(i)] else ''} bad")
            lba += n
            print(f"\r  {lba}/{total} sectors", end="", flush=True)
    print()
    audio = total - data_sectors
    print(f"sectors: {total} total, {data_sectors} Mode1 data, {audio} non-data/audio")
    print(f"bad MSF: {bad_msf} · bad EDC: {bad_edc} · bad ECC: {bad_ecc}")
    if bad_msf == bad_edc == bad_ecc == 0 and data_sectors:
        print("VERDICT: all data sectors are intact")
        return 0
    return 1


def main():
    args = sys.argv[1:]
    if not args or args[0] in ("-h", "--help"):
        print(__doc__)
        return 0
    if args[0] == "--check":
        if len(args) != 2:
            sys.exit("usage: iso2bincue.py --check image.bin")
        return check(args[1])
    convert(args[0], args[1] if len(args) > 1 else None)
    return 0


if __name__ == "__main__":
    sys.exit(main())
