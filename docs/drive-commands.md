<!--
  MatshitaPico -- Solid-state CD-ROM drive replacement for the Commodore CDTV
  Copyright (c) 2026 Nicola Avanzi (na103)
  Licensed under Creative Commons Attribution 4.0 International (CC BY 4.0).
  See LICENSE.
-->

# Command set handled by the firmware — reference

> Reference for the commands that the RP2350 firmware recognizes and serves on CN9. It is
> the **specification of what is implemented on real hardware**, complementary to
> [`protocol-cdtv.md`](protocol-cdtv.md) (which derives from WinUAE's `cdtv.cpp`).

## Framing and reply channels

- **Command** = bytes written by the host to `base+0xA1` (the DMAC command register),
  captured by the CPLD as an asynchronous latch on the **rising edge of `_HWR`** and passed
  to the firmware.
- **Command length** (`cmd_length()`): depends on the opcode. Most are **7 bytes** (opcode
  + 6 parameters); some status/polls are **1 byte**; the `0x80` ping is **2 bytes**; the
  `0x00` filler is **1 byte**. An incomplete command older than 10 ms is discarded (parser
  resync).
- **Reply** over three distinct channels:
  - **STEN** (mailbox): short replies read by the host via `0xA1`, **1 `_STEN` pulse per
    byte** (long pulse ~118 µs). E.g. status, capacity, subq, TOC, vendor DETECT/GEOMETRY/
    STATUS.
  - **DMA / DRQ**: the data stream (CD sectors or hardfile blocks) transferred via hardware
    DMA (`DRQ`↑ = byte ready). E.g. `0x02` read CD, `0x62` read hardfile.
  - **STCH** (asynchronous notification): events (seek/play done, media change) — not a
    byte reply but a pulse that makes the host CD task react.
- A command with no reply (`0x00`, seek, motor…) does **not** assert `_STEN`.

Byte-order/handshake details are in [`timing-host-interface.md`](timing-host-interface.md)
and [`protocol-cdtv.md`](protocol-cdtv.md).

---

## CD protocol commands (port of WinUAE's `cdtv.cpp`)

| Op | Name | Len | Payload | Reply | Description |
|----|------|-----|---------|----------|-------------|
| `0x00` | NOP / filler | 1 | — | none | Trailing byte of the "armed poll" `f058ea` (buffer `81 00`). Does not assert `_STEN`, does not misalign the parser. If treated as a 2-byte command it "ate" the next opcode. |
| `0x80` | test / ping | 2 | — | STEN 2B `AA 55` | Test command. |
| `0x01` | seek | 7 | LSN | STCH | Seek to the position. Marks `finished` and pulses STCH (no data). |
| `0x02` | read data | 7 | LSN(3) + n_sectors(2) | DMA n×sector_size | Starts the DMA transfer of the data sectors. Guard: out of range / no media → error, no phantom DMA. `sector_size` from the `0x84` mode-set. |
| `0x04` | motor on | 7 | — | — | Turns the motor on (`finished`). |
| `0x05` | motor off | 7 | — | — | Turns the motor off (`finished`). |
| `0x09` | play (LSN) | 7 | start(3) + len(3) | STCH | Starts audio playback from `start` for `len` frames. `00…00 / 00…00` = **stop**. STCH at start (statusfunc). |
| `0x0a` | play (MSF) | 7 | start(3) + end(3) | STCH | Like `0x09` but parameters in MSF (converted to LSN). |
| `0x0b` | play track range | 7 | track_start(1) + track_end(1) | STCH | Plays from track `start` to `end`. `00 00` = play from the current position to the end of disc (a ROM quirk of the CD+G screen). |
| `0x81` | status | 1 | — | STEN 1B | Status byte: b0=1 (always), b2=playing, b3=finished, b4=error, b5=motor, b6=media. This is the host's main poll. |
| `0x82` | read/clear error | 7 | — | STEN 6B | Returns the error state (b4 of `r[2]` if error) and **clears** `error`/`ready`. |
| `0x83` | inquiry | 7 | — | STEN 12B | Identification string `"MATSHITA0.96"`. Not a gate (the ROM does not validate it). |
| `0x84` | mode set | 7 | page(1) + size(2) + … | — | Sets the **sector size** (512/1024/2048/2052/2336/2340); an invalid size → error. Typical init `84 02 08 00 00 0F 00` (2048). |
| `0x85` | mode sense | 1 | — | STEN 2B | Returns the current sector size. |
| `0x86` | capacity | 1 | — | STEN 5B | `last_lsn-1` (24 bit) + sector size (16 bit). |
| `0x87` | subq | 7 | MSF/LSN flag in bit1 | STEN 13B | Q position channel: audio_status, ADR/control, track, index, absolute position (disc) and relative (track). MSF or LSN per `cmd[1]&2`. Used by the audio player and (on real hardware) by Sherlock. |
| `0x88` | — | 1 | — | STEN 14B (zeros) | Placeholder (14 zero bytes); a command rarely used by the ROM. |
| `0x89` | disc info | 7 | — | STEN 5B | `first_track`, `last_track`, MSF of `last_lsn`. |
| `0x8a` | read TOC | 7 | MSF/LSN flag + point(1) | STEN 8B | TOC record for track `point`. **Marks "mounted"** (`toc_read`) → the boot-time STCH announce stops. |
| `0x8b` | pause / resume | 7 | 0x00=pause / else=resume | — | Pauses/resumes playback (`audio::pause`). While paused `playing` stays set (like the reference). |
| `0xa2` | front panel | 1 | — | STEN 4B (zeros) | Front panel (placeholder). |
| `0xa3` | front panel enable | 7 | — | — | Enables the front panel (`finished`). Part of the CD+G sequence (`04, 0B, A3`). |
| *default* | unknown | 7 | — | — | Any other opcode → `error`. |

**Status byte (`0x81`)** — bits: `0x01`=ready-inverted (always 1), `0x04`=playing,
`0x08`=finished, `0x10`=error, `0x20`=motor, `0x40`=media. Ejected = `0x01`, with media =
`0x41`, at end of spin-up = `0x49`.

---

## Vendor mpdisk commands — `.hdf` hardfile (`0x60`–`0x64`)

Vendor page for the **hardfile** subsystem (`mpdisk.device` ↔ firmware). All 7 bytes:
`op, unit, LBA[4 MSB-first], count`. `unit` = 0..3 (MPD0..MPD3 = disk0.hdf..disk3.hdf);
block = 512 B. Exclusive with the CD in v1 (the Amiga device uses `Forbid()`).

| Op | Name | Payload | Reply | Description |
|----|------|---------|----------|-------------|
| `0x60` | DETECT | — | STEN 8B | Signature `"MPDK"` + version (`0x01`) + unit count + block size (512). Re-runs `scan_hardfiles()` of the SD root before replying (robust against boot-time ready delays). It is the gate that tells the device "the mpdisk firmware is present". |
| `0x61` | WRITE | unit, LBA, count | bulk-rx | Enters **bulk-rx**: after the command, `count×512` data bytes follow via `_HWR`, accumulated per block and written to the `.hdf`. The device writes **1 block per command** (waiting for STATUS acts as the handshake → no overflow of the CPLD's 2-byte FIFO). Outcome via `0x63`. |
| `0x62` | READ | unit, LBA, count | DMA count×512B | Reads `count` blocks from the `.hdf` and transfers them via DMA (DRQ). Range-checked (missing unit / LBA past capacity → error, no phantom DMA). |
| `0x63` | STATUS | — | STEN 2B | Flags: b0=ready (pipe active), b1=last error (`error` or a write error), b2=bulk-rx write in progress. Second byte = 0. Used after WRITE to confirm the outcome. |
| `0x64` | GEOMETRY | unit | STEN 6B | Geometry of the unit: total_blocks (4 bytes) + block size (2 bytes, 512). Missing `unit` → total_blocks=0 + error. The device derives HighCyl = TotalSectors/32-1 (self-describing: changing the size of a `.hdf` does not require rebuilding the CD). |

### Free / reserved opcodes
- `0x65`, `0x66`–`0x6F` are **free** (the dispatch treats them as "unknown" → `error`).
