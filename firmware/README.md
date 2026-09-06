<!--
  MatshitaPico — Solid-state CD-ROM drive replacement for the Commodore CDTV
  Copyright (c) 2026 Nicola Avanzi (na103)
  Licensed under Creative Commons Attribution 4.0 International (CC BY 4.0).
  See LICENSE.
-->

# MatshitaPico — RP2350 firmware

This directory holds the **application source** of the RP2350 / Raspberry Pi Pico 2
firmware: the drive command protocol, BIN/CUE image handling, I2S audio, the subcode
Q / CD+G channel, and the UI (NEXT button + status LED). The CPLD design that pairs
with it (the real-time 5 V front-end on CN9) is in [`../hdl`](../hdl); the protocol
itself is documented in [`../docs`](../docs).

> This is a **read-only reference copy** of the core sources. The build system
> (`CMakeLists.txt`), the SD-card driver glue (SPI-over-PIO transport and
> `hw_config`), and the third-party FatFs library are maintained with the full
> firmware tree and are **not** included here — see *Dependencies* below.

## What runs where

```
   CDTV (CN9, 5 V TTL)
        |
        v
   CPLD  ATF1508AS   real-time front-end: host-interface FSM (DRQ mode),
        |            audio clock division (C16M -> BCLK/LRCK), subcode burst
        v            engine (SBCP/SCOR/EFFK), /INAC activity LED
   MCU  RP2350       THIS firmware: drive command protocol, SD (BIN/CUE),
                     I2S audio samples, subcode Q stream, NEXT + LED
```

The RP2350 is **not** 5 V-tolerant, so it never touches CN9 directly: the CPLD is the
only chip in the 5 V domain, and a bus switch level-shifts the CPLD↔Pico signals. The
firmware talks to the CPLD through the register/handshake contract in
[`../hdl/host_if.v`](../hdl/host_if.v) (see `src/host_link.h`).

## SD card layout

The firmware reads a FAT-formatted microSD with this layout:

```
/                 <- SD root
├── games/        <- CD-ROM images
│   ├── Game A.cue
│   ├── Game A.bin
│   ├── Game A.sub     (optional: CloneCD subchannel, enables CD+G)
│   ├── Game A.subok   (optional: empty marker, forces subcode ON for a mixed disc)
│   └── ...
├── work.hdf      <- hardfiles in the root (ENABLE_MPDISK builds only)
└── extra.hdf
```

* **CD images — `/games/*.cue`.** Discs live in the `/games/` folder as **BIN/CUE, 2352-byte
  raw** sectors (not `.iso`: an ISO is 2048-byte data only and carries no audio, sync or EDC).
  The `.cue` and its referenced `.bin` sit side by side; the NEXT button cycles through them in
  name order. A companion `.sub` (CloneCD subchannel) next to a `.cue` enables **CD+G** playback,
  and an empty `.subok` marker forces the subcode stream on for a mixed disc known to be safe.
* **Hardfiles — `*.hdf` in the root** (only when built with **`ENABLE_MPDISK`**). Amiga hard-disk
  images placed in the SD root are exposed as block-device units **MPD0..MPD3** (first four found,
  in name order). These are ordinary **`.hdf` files, interchangeable with WinUAE** — mount the same
  image in WinUAE to prepare or inspect it, then drop it in the SD root. See *Optional feature:
  mpdisk hardfiles* below.

## The golden reference

The command protocol is a port of `cdtv.cpp` from **WinUAE** (the executable
specification of the DMAC + 6525 + LC8951 + command set). The WinUAE sources are not
redistributed here. See [`../docs/drive-commands.md`](../docs/drive-commands.md) and
[`../docs/protocol-cdtv.md`](../docs/protocol-cdtv.md).

## Source layout

| File | Role |
|------|------|
| `main.cpp` | main loop: command intake, status/data replies, STCH announce, auto-insert |
| `cdtv_drive.{h,cpp}` | drive state machine + command dispatch (ported from `cdtv.cpp`) |
| `host_link.{h,cpp}` | link layer to the CPLD (IRQ capture, bus gather/scatter, handshakes) |
| `cd_image.{h,cpp}` | CD image (BIN/CUE 2352 raw): TOC, LSN→offset, prewarm, subcode gate |
| `cue_parser.{h,cpp}` | SDK-free CUE sheet parser (unit-testable on the PC) |
| `storage.{h,cpp}` | SD/FatFs glue: image scan, cached reads (data / audio / subcode) |
| `audio.{h,cpp}` | I2S-slave transmitter on A_DATA, ping-pong DMA |
| `subcode.{h,cpp}` | subcode Q + CD+G R-W stream on A_SUBQ |
| `subq_packet.h` / `subpw_block.h` | Red Book Q packet + P..W wire block builders |
| `cdrom_util.h` | LSN/MSF/BCD conversions |
| `ui.{h,cpp}` / `ui_logic.h` | NEXT button + status LED (pure logic split out for tests) |
| `pinmap.h` | RP2350 GPIO assignment (matches the routed PCB) |
| `ram_func.h` | portable macro to place critical-path code in RAM (removes XIP stalls) |
| `pio/*.pio` | PIO programs: I2S slave, subcode serialiser, SD SPI master |

Files that are compiled for both the device and the PC unit tests (`cd_image`,
`cue_parser`, `storage`, `subq_packet`, `subpw_block`, `ui_logic`) avoid the Pico SDK
in their hot paths; `ram_func.h`'s `RAM_FUNC` macro is a no-op off ARM so the shared
sources compile identically on the host.

## Dependencies (not included here)

* **Pico SDK** — the RP2350 platform (`pico/stdlib.h`, `hardware/*`, PIO tooling).
* **FatFs** (ChaN's ff15, e.g. via `no-OS-FatFS-SD-SPI-RPi-Pico`) — the FAT filesystem
  behind `storage.cpp`. Only the standard `f_*` API is used, so any FatFs port works.
* **SD driver glue** — the SPI-over-PIO transport (`sd_spi_pio`) and `hw_config` that
  bind FatFs to the rev.0 PCB pins (`pinmap.h`). The rev.0 SD pins do not map to any
  RP2350 hardware SPI, so the bus is bit-banged in PIO on `pio2`.

## Optional feature: mpdisk hardfiles

Read/write `.hdf` hard-disk images (an Amiga block device, `mpdisk.device`) are gated
behind the compile-time flag **`ENABLE_MPDISK`** (default **off**). With the flag off
the vendor pipe (commands `0x60`–`0x64`) compiles out entirely and the firmware is the
plain CD-ROM replacement; with it on, `.hdf` files in the SD root become units
MPD0..MPD3. See the `#if ENABLE_MPDISK` blocks in `cdtv_drive.cpp` / `storage.cpp` and
[`../docs/drive-commands.md`](../docs/drive-commands.md).
