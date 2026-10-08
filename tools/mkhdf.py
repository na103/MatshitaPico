#!/usr/bin/env python3
# =============================================================================
# MatshitaPico -- Solid-state CD-ROM drive replacement for the Commodore CDTV
# Copyright (c) 2026 Nicola Avanzi (na103)
# Licensed under Creative Commons Attribution 4.0 International (CC BY 4.0).
# See LICENSE.
# =============================================================================
# mkhdf.py -- creates an EMPTY .hdf hardfile (WinUAE style) for mpdisk.device.
#
# Flat geometry matching the device (TD_GETGEOMETRY: heads=1, sectors=32,
# 512 B blocks): block N of the file = byte N*512. cyls = blocks/32.
#
# Usage:
#   python3 mkhdf.py <output.hdf> <size_MB> [--fill]
#     --fill : write real zeros (to create it DIRECTLY on the SD card; without it
#              a sparse local file is created, materialised when copied to FAT32).
#
# The file goes in the ROOT of the SD card as disk0.hdf..disk3.hdf (/games is for
# CDs only). Then FORMAT it as OFS (DOS\0, built into the Kickstart 1.3 ROM -> no
# external binaries): in WinUAE (mounted as a hardfile) or on the CDTV (Mount
# MPD0: + Format). FFS (DOS\1) would need L:FastFileSystem, absent on CDTV discs
# -> default = OFS. Prints a Mountlist entry for the chosen size.
# =============================================================================
import sys, os

BSIZE = 512      # bytes per block
SURF  = 1        # heads
BPT   = 32       # blocks per track (sectors)

def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    fill = "--fill" in sys.argv
    if len(args) != 2:
        print("usage: mkhdf.py <output.hdf> <size_MB> [--fill]", file=sys.stderr)
        return 2
    out, mb = args[0], int(args[1])

    total_bytes = mb * 1024 * 1024
    blocks = total_bytes // BSIZE
    blocks -= blocks % (SURF * BPT)                # whole number of tracks
    if blocks <= 0:
        print("size too small", file=sys.stderr); return 2
    cyls  = blocks // (SURF * BPT)
    size  = blocks * BSIZE

    with open(out, "wb") as f:
        if fill:
            chunk = b"\x00" * (1024 * 1024)
            written = 0
            while written < size:
                n = min(len(chunk), size - written)
                f.write(chunk[:n]); written += n
        else:
            f.truncate(size)                       # local sparse file

    name = os.path.basename(out)
    print(f"created {out}: {size} bytes = {blocks} blocks of {BSIZE} B "
          f"({size//(1024*1024)} MiB)")
    print(f"geometry: cyls={cyls} surfaces={SURF} blockspertrack={BPT} blocksize={BSIZE}")
    print()
    print("--- Mountlist entry (HighCyl matches the size) ---")
    print(f"MPD0:")
    print(f"    Device       = mpdisk.device")
    print(f"    Unit         = 0")
    print(f"    Flags        = 0")
    print(f"    Surfaces     = {SURF}")
    print(f"    BlocksPerTrack = {BPT}")
    print(f"    Reserved     = 2")
    print(f"    Interleave   = 0")
    print(f"    LowCyl       = 0")
    print(f"    HighCyl      = {cyls - 1}")
    print(f"    Buffers      = 30")
    print(f"    BufMemType   = 1")
    print(f"    Mount        = 1")
    print(f"    DosType      = 0x444F5300")        # DOS\0 = OFS (in the 1.3 ROM)
    print(f"    StackSize    = 4000")
    print(f"    Priority     = 5")
    print(f"    GlobVec      = 0")
    print(f"#")
    print("copy the .hdf to the ROOT of the SD card; on FAT32 use a defragmented card "
          "(contiguous cluster chain -> writes never touch the FAT).")
    return 0

if __name__ == "__main__":
    sys.exit(main())
