<!--
  MatshitaPico — Solid-state CD-ROM drive replacement for the Commodore CDTV
  Copyright (c) 2026 Nicola Avanzi (na103)
  Licensed under Creative Commons Attribution 4.0 International (CC BY 4.0).
  See LICENSE.
-->

# MatshitaPico — Amiga side: `mpdisk.device` and the boot CD

This directory holds the **Amiga-side sources** of the optional *mpdisk* feature:
read/write **`.hdf` hardfiles** stored on the MatshitaPico microSD and exposed to the
CDTV as block-device units **MPD0..MPD3**. The CDTV can boot from a small CD, mount
the hardfile and hand the system over to it: a CDTV with a writable hard disk, and no
extra hardware beyond the drive replacement.

The firmware half of the feature (vendor commands `0x60`–`0x64`) is compiled in only
with **`ENABLE_MPDISK`** — see [`../firmware/README.md`](../firmware/README.md) and the
byte-level reference in [`../docs/drive-commands.md`](../docs/drive-commands.md).

## How it works

```
   CDTV  (68000, Kickstart 1.3)
     |   mpdisk.device  -- trackdisk-like exec device, units MPD0..MPD3
     |       |  vendor commands on the CD command port (DMAC 0xA1)
     |       |  READ  = DMA (DRQ), like a CD sector read
     |       |  WRITE = bulk bytes on 0xA1, one block per command
     v       v
   CN9 -> CPLD -> RP2350 firmware -> FatFs -> /*.hdf on the SD card
```

There is no extra bus or connector: the device reuses the **CD controller itself**
(DMAC U36 + 6525 U32), found through `expansion.library/FindConfigDev`. The DMAC is
**shared** with the ROM-resident `cdtv.device`, which stays alive, so the pipe:

- catches the per-byte `STEN` replies with its **own INT2 server** at priority 127,
  ahead of `cdtv.device`'s (with a retry if a byte is still stolen);
- **arbitrates the DMA**: before each read chunk it checks, under `Disable()`,
  `cdtv.device`'s "CD DMA in flight" flag (`272(a6)`) and backs off if a CD transfer
  is running; the DMA itself is programmed exactly like the ROM does (`f057f4`),
  with long-word writes to `WTC`/`ACR` (byte writes are not latched by the DMAC);
- paces the 7 command bytes like the ROM does, and writes **one block per WRITE
  command**, using `STATUS` as a per-block handshake (the CPLD FIFO is 2 bytes).

## Layout

```
amiga/
├── mpdisk/                 mpdisk.device
│   ├── device.c            exec framework: romtag, init, vectors, trackdisk commands
│   ├── pipe.c / pipe.h     transport: DMAC/6525 access, STEN capture, arbitrated DMA
│   ├── pipe_stub.c         WinUAE test backend (reads work.hdf as a file, no hardware)
│   ├── mpdisk_dev.h        device base structure
│   └── Makefile            `make` -> mpdisk.device, `make stub` -> mpdisk_stub.device
├── mpmount/                mpmount: mounts MPD0..MPD3 with the geometry from the firmware
│   ├── mpmount.c           TD_GETGEOMETRY -> MakeDosNode/AddDosNode (OFS, KS 1.3 ROM)
│   ├── crt0.S              minimal entry (no utility.library on KS 1.3) + soft 32-bit div
│   └── Makefile
├── boot/
│   ├── startup-sequence    boot CD script: RMTM, mpmount, assigns to MPD0:, run its S/Startup-sequence
│   └── cd-extra/           free overlay: anything here is merged into the boot CD
└── mount/
    └── MPDisk              static Mountlist (reference / WinUAE / manual `Mount MPD0:`)
```

The PC-side tools are in [`../tools`](../tools):

| Tool | Purpose |
|---|---|
| `build-bootcd.sh` | Builds `mpdisk.device` + `mpmount`, then the boot CD (`.bin` + `.cue`). |
| `mkbootcd.py` | The CD builder: ISO9660 + CDTV trademark (or `DOS\0` bootblock) → MODE1/2352 BIN/CUE. |
| `iso2bincue.py` | EDC/ECC engine used by `mkbootcd.py`; also a standalone ISO → BIN/CUE converter. |
| `mkhdf.py` | Creates an empty `.hdf` of a given size and prints the matching Mountlist entry. |

## Build

Requirements: the **m68k-amigaos-gcc** cross compiler (Bebbo's toolchain), and
`python3` with `numpy` for the CD tools.

```sh
make -C amiga/mpdisk          # -> amiga/mpdisk/mpdisk.device
make -C amiga/mpmount         # -> amiga/mpmount/mpmount
```

### Boot CD

The boot CD needs a few **Commodore binaries** (AmigaDOS `C:` commands and the CDTV
trademark file `CDTV.TM`) which are copyrighted and **not** included. They are extracted
once from **your own genuine CDTV disc** (a MODE1/2352 `.bin`) into a git-ignored cache:

```sh
tools/build-bootcd.sh --genuine "/path/to/CDTV_genuine.bin"   # first run: extracts the cache
tools/build-bootcd.sh                                         # later runs: standalone
```

`RMTM`, `LoadWB` and `NewCLI` are not in the `/C` directory of a genuine disc: copy them
by hand into `tools/.bootcd-cache/C/` (`RMTM` from the root of the CDTV Demo Disc,
`LoadWB`/`NewCLI` from Workbench 1.3). `RMTM` must run first, or the CDTV stays on the
splash screen.

The result is `build/mpdisk-bootcd.bin` + `.cue` (or `--out <base>`): copy it into `/games`
on the SD card like any other disc.

## Preparing a hardfile

```sh
python3 tools/mkhdf.py disk0.hdf 64       # empty 64 MB hardfile
```

Copy it into the **root** of the SD card. Units are assigned to the first four `*.hdf` in
name order (`disk0.hdf`..`disk3.hdf` keeps it obvious). Format it as **OFS** (`DOS\0`,
built into the Kickstart 1.3 ROM, so no `L:FastFileSystem` is needed) — in WinUAE, mounted
as a normal hardfile, or on the CDTV. For the boot CD flow, MPD0: must contain a bootable
system (`C`, `L`, `LIBS`, `DEVS`, `S`, ...): it becomes `SYS:` and its
`S/Startup-sequence` is executed.

Use a defragmented card: a contiguous `.hdf` keeps writes away from the FAT.

## Testing in WinUAE

`make -C amiga/mpdisk stub` builds `mpdisk_stub.device`: the same `device.c` with a backend
that reads `work.hdf` as a plain file. Copy it to `DEVS:mpdisk.device`, the Mountlist to
`DEVS:`, then `Mount MPD0:`. The init result is written to `RAM:mpdisk-init.log`. This
validates the device framework without any hardware.

## Limits

- The DMAC is shared with the CD: hardfile and CD access are serialised (`Forbid()` around
  each request), and throughput is about **140 KB/s**.
- Ejecting/changing the CD still reboots the CDTV (that is `cdtv.device`'s behaviour on a
  media change), so the hardfile session ends with it.
