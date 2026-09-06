<!--
  MatshitaPico -- Solid-state CD-ROM drive replacement for the Commodore CDTV
  Copyright (c) 2026 Nicola Avanzi (na103)
  Licensed under Creative Commons Attribution 4.0 International (CC BY 4.0).
  See LICENSE.
-->

# CDTV CD-ROM drive protocol specification

Extracted from WinUAE's `cdtv.cpp` (WinUAE emulation by Toni Wilen, technical info by
Mark Knibbs) and `include/blkdev.h`. This is the behavior the replacement device must
reproduce on the drive side, exposed on connector **CN9**.

> Convention: "host" = the CDTV (68000 CPU + DMAC + 6525). "drive" = the device we are
> building. Numbers are hexadecimal when prefixed with `0x`.

---

## 1. General model

The CDTV never talks directly to the optics: it talks to a **drive microcontroller**
through the **LC8951** decoder, which acts as a mailbox (command FIFO + status register)
and as a data-transfer engine. The replacement device must emulate **both** the LC8951
host-interface hardware **and** the drive µC logic.

Five logical channels coexist on CN9:

| Channel | Direction | CN9 signals | Function |
|---|---|---|---|
| Command | host→drive | `DB0-7`, `HWR`, `CMD` | command bytes into the FIFO |
| Status | drive→host | `DB0-7`, `HRD`, `STEN` | command reply bytes |
| Notify | drive→host | `STCH` | asynchronous status change (IRQ) |
| Data | drive→host | `DB0-7`, `DRQ`+`HRD` (measured: `DTEN`/`XAEN` never asserted) | sectors over DMA |
| Subcode | drive→host | `SCOR`, `SBCP`, `SCCK` | subchannel frames (Q channel etc.) |
| Audio | drive→host | `C16M`, `DATA`, `LRCK`, `BCLK` | digital I²S audio + clocks |
| Control | host→drive | (serial via a 6525 port) | volume / DAC format |

On the CDTV side these signals are driven/read by the **6525 (TPI)** and the **DMAC**;
for us the semantics matter, not those chips (they live on the motherboard).

---

## 2. Command sequence (command/status handshake)

1. The host writes the command bytes one at a time with `CMD` asserted (`HWR`).
   The first byte is the **opcode**; the total length depends on the opcode (see §3).
2. When the drive has received all the expected bytes, it executes the command and
   produces 0 to N bytes of **output** (command status).
3. The drive signals the availability of the output: `STEN` active while there are bytes
   to read. (In `cdtv.cpp`: at command completion `sten=1, stch=0`.)
4. The host reads the output bytes one at a time (`HRD`, register at `0xa1`). After the
   last byte `STEN` goes inactive (`sten=0`).
5. Asynchronous events (end of seek, end of audio, media change, error, motor) are
   notified by raising **`STCH`** (`do_stch`), which generates an IRQ (INT2) on the CDTV.

Important rules:
- A command with no output (`size=0`) still completes the cycle: the host understands
  this because `STEN` does not activate (or deactivates immediately).
- `STCH` is raised only if enabled by the 6525 configuration (`tp_cr & 1` and the mask,
  see §9). In the initial reset `stch=1`.

---

## 3. Command set

Legend: **Len in** = number of command bytes (opcode included). **Out** = reply bytes.
`s[n]` = the n-th command byte.

| Op | Name | Len in | Output | Notes |
|----|------|:------:|--------|------|
| `0x00` / `0x80` | Test / ping | 2 | `0xAA 0x55` | presence handshake |
| `0x01` | **Seek** | 7 | — | LBA in `s[1..3]`; raises `STCH` at end of seek; `cd_finished=1` |
| `0x02` | **Read (data)** | 7 | — | start LSN=`s[1..3]`, sector count=`s[4..5]`; starts the DMA transfer (§5) |
| `0x04` | Motor on | 7 | — | `cd_motor=1`, `cd_finished=1` |
| `0x05` | Motor off | 7 | — | `cd_motor=0`, `cd_finished=1` |
| `0x09` | **Play audio (LSN)** | 7 | — | start=`s[1..3]`, *length*=`s[4..6]` (end=start+len) |
| `0x0a` | **Play audio (MSF)** | 7 | — | start=`s[1..3]`, end=`s[4..6]` in MSF |
| `0x0b` | Play track range | 7 | — | track_start=`s[1]`, idx_start=`s[2]`, track_end=`s[3]`, idx_end=`s[4]` |
| `0x81` | **Status byte** | 1 | 1 | status bitmask (see §4) |
| `0x82` | Read/clear error | 7 | 6 | if `cd_error` sets bit4 of out[2]; clears `cd_error`, `cd_isready` |
| `0x83` | **Inquiry** (model) | 7 | 12 | string **`MATSHITA0.96`** |
| `0x84` | **Mode set** | 7 | — | sector size = `s[2..3]` (see §5) |
| `0x85` | Mode sense | 1 | 2 | current sector size (16 bit, MSB first) |
| `0x86` | **Capacity** | 1 | 5 | lastaddress-1 (3B) + sector size (2B); `cd_error` if no media |
| `0x87` | **SubQ** (position) | 7 | 13 | `s[1]&2` ⇒ MSF, else LSN (see §6) |
| `0x88` | (disc info?) | 1 | 14 | 14 zero bytes (placeholder) |
| `0x89` | Disc info | 7 | 5 | first_track, last_track, size MSF (3B) |
| `0x8a` | **Read TOC** | 7 | 8 | track=`s[2]`, `s[1]&2`⇒MSF (see §7) |
| `0x8b` | Pause/Resume | 7 | — | `s[1]==0x00` ⇒ pause, else resume |
| `0xa2` | (?) | 1 | 4 | 4 zero bytes |
| `0xa3` | Front panel enable | 7 | — | enable/disable the front panel (`s[1]`) |
| others | unknown | — | 0 | sets `cd_error` |

> The 7-byte commands have 6 payload bytes after the opcode; often only some are used.
> The length is the discriminant by which the drive knows the command is complete.

---

## 4. Status byte (command `0x81`)

A single byte, active bits = condition true:

| Bit | Value | Meaning |
|-----|--------|-------------|
| 0 | `0x01` | NOT ready (`!cd_isready`) |
| 2 | `0x04` | audio playing (`cd_playing`) |
| 3 | `0x08` | operation finished (`cd_finished`) |
| 4 | `0x10` | error (`cd_error`) |
| 5 | `0x20` | motor on (`cd_motor`) |
| 6 | `0x40` | media present (`cd_media`) |

Reading command `0x81` clears `cd_finished`.

### Audio states (`cd_audio_status`, from `blkdev.h`)
Used in SubQ and in notifications:

| Constant | Value |
|---|---|
| NOT_SUPPORTED (data) | `0x00` |
| IN_PROGRESS (play) | `0x11` |
| PAUSED | `0x12` |
| PLAY_COMPLETE | `0x13` |
| PLAY_ERROR | `0x14` |
| NO_STATUS | `0x15` |

---

## 5. Data transfer (sector read)

1. The host sets the **sector size** with `0x84` (mode set). Valid values:
   `512, 1024, 2048, 2052, 2336, 2340`. Typical data CD-ROM = **2048**.
2. The host sends `0x02` (read) with the start LSN and sector count.
3. On the CDTV side the DMAC is programmed (destination address ACR, word count WTC) and
   started; the drive delivers the sector bytes on the data bus with the
   `DTEN`/`DRQ`/`HRD` handshake.
4. The transfer is in **16-bit words**: the DMAC fetches 2 bytes per cycle
   (`dma_put_byte` ×2). Total bytes to supply = `n_sectors × sector_size`.
5. At the end of the transfer: `cd_finished=1`; on the host side the end-of-DMA IRQ fires.

Size→sector-source mapping (from `dma_do_thread`):
- `2048` → standard data sector (Mode 1 / Mode 2 Form 1 user data).
- `2336/2340/2352/2052` → raw read (needs header/subheader/EDC/ECC from the file).
- `<2048` → a fraction of the sector.

> Implication for the replacement: from the file on the SD card (ISO 2048 or BIN/CUE 2352)
> the requested size must be reconstructed. Having **2352 raw** images is the safest choice
> because it covers all raw requests and the subset can always be extracted.

---

## 6. SubQ — current position (command `0x87`, 13-byte output)

`s[1] & 2` selects the time format: **MSF** if set, **LSN** otherwise.

| Byte | Contents |
|------|-----------|
| 0 | `cd_audio_status` (§4) |
| 1 | CONTROL/ADR (swapped nibbles: `(x>>4)|(x<<4)`) |
| 2 | track number (BCD→bin) |
| 3 | index (BCD→bin) |
| 4 | 0 |
| 5..7 | **absolute disc** position (MSF or LSN), MSB first |
| 8 | 0 |
| 9..11 | **track-relative** position (MSF or LSN), MSB first |
| 12 | 0 |

The source data comes from `cdrom_qcode` (the subcode Q channel, in BCD). The device must
keep the current position consistent with the audio being played.

---

## 7. TOC, capacity, disc info

**Read TOC** (`0x8a`, out 8 bytes) for the requested track/point:
| Byte | Contents |
|---|---|
| 0 | 0 |
| 1 | `(ADR<<4) | CONTROL` |
| 2 | point (track number) |
| 3 | total number of tracks |
| 4 | 0 |
| 5..7 | start address (MSF or LSN) |

**Capacity** (`0x86`, out 5 bytes): `lastaddress-1` (3B) + sector size (2B).

**Disc info** (`0x89`, out 5 bytes): first_track, last_track, total size in MSF (3B).

The device must build a TOC from the files on SD (from CUE, or from an implicit TOC for a
single ISO). `datatrack` is set if the first track is data (control & 0x0c == 0x04).

---

## 8. CD audio and volume control

- The drive generates `C16M` (16.9344 MHz) and the digital audio stream (`DATA`/`LRCK`/
  `BCLK`, I²S-like format 44.1 kHz 16-bit stereo) that feeds the LC7883 DAC on the
  motherboard.
- During play (`0x09/0x0a/0x0b`) the device plays the audio tracks from the file (BIN/CUE),
  synchronizing position and subcode.
- **Volume**: the host sends it **serially** through port B of the 6525:
  - bit 6 = shift clock; on each edge bit 5 is shifted into `dac_control_data_format`
    (12 bits total). bit 7 = latch strobe: at the latch `cd_volume =
    dac_control_data_format & 0x3FF` (10 bits).
  - The device must capture this serial word and apply the attenuation to the audio output.
    (`xaen`, `enable`, `cmd`, `dten` come from the other port B bits — see the direction
    note in §11.)

---

## 9. Serial subcode (SCOR / SBCP / SCCK)

The drive sends the subchannel frames serially; the CDTV shifts them in through port A of
the 6525.
- `SCOR` = start of subchannel frame (sync). `SBCP` = subchannel byte ready.
- A frame = **`SUB_CHANNEL_SIZE` = 96 bytes** (channels P–W de-interlaced, 12×8).
- The bytes read from port A are **bit-reversed** (see `tp_bget` case 0: each bit is
  mirrored) → the device must emit the bits in the expected order.
- Rate: in `cdtv.cpp` one frame every ~`maxvpos*vblank/75` (≈ 75 frames/s, the CD rate),
  with micro-timing `SUBCODE_CYCLES=460` between bytes. When the motor runs but no playback
  is in progress, periodic `SCOR` pulses are still generated ("frame interrupt").

The status lines toward the CDTV (port C of the 6525, **active low**):
`SBCP` (bit0), `SCOR` (bit1), `STCH` (bit2), `STEN` (bit3/4).

---

## 10. Address conversions

- **LSN/LBA ↔ MSF**: `msf2lsn`, `lsn2msf`. MSF = Minute:Second:Frame, 75 frames/s, with an
  offset of 150 frames (2 s) for the pre-gap. BCD is used in the Q channel (`frombcd`,
  `fromlongbcd`).
- `last_cd_position` = `toc.lastaddress` (end of disc in LSN).

---

## 11. Signal-direction notes — RECONCILED ON REAL HARDWARE

In `cdtv.cpp`, `cmd`, `enable`, `xaen`, `dten` are derived from **port B** written by the
CPU (host→drive), while the LC8951 datasheet gives `DTEN`/`STEN`/`WAIT`(`DRQ`)/`EOP` as
chip **outputs**. Bus captures closed the question:
- Measured directions (continuity + captures): `_CMD`/`_ENABLE`/`_XAEN`/`_DTEN` = drive
  **inputs** (from the TPI U32, port B); `_STEN`/`DRQ`/`_STCH` = drive **outputs**.
- **Polarity**: `_HWR`/`_HRD`/`_CMD`/`_ENABLE`/`_STEN` active-low; **`DRQ` ACTIVE HIGH**;
  `RESET` idle-high.
- **`_DTEN` and `_XAEN` are NEVER asserted** by the CDTV, not even during continuous data
  transfer: the data path uses **only `DRQ`+`_HRD`**, the data phase is distinguished by
  `_CMD` high alone. `EOP` is not on CN9 (the DMAC counts it).
- `cdtv.cpp` models the *register/protocol-level semantics* (correct); the real electrical
  behavior is the measured one, applied to `hdl/host_if.v`.

> Note: further reverse-engineering of the original drive PCB showed that `_XAEN` and
> `_DTEN` are actually drive **outputs** (read by the 6525), and `SCCK` is a drive
> **input** generated by the motherboard. This does not change the measured "never
> asserted / DRQ-only" data path above; see the pinout notes and the HDL comments.

---

## 12. Appendix — CDTV-side register map (reference)

These are the registers **of the CDTV motherboard** (DMAC + 6525), as seen by the 68000.
We do not implement them (they live on the CDTV), but they clarify the semantics.

DMAC (offset from the Zorro II autoconfig base):
| Offset | R/W | Function |
|---|---|---|
| `0x00-0x3F` | R | autoconfig ROM (`dmacmemory`) |
| `0x41` | R | ISTR (interrupt status) |
| `0x43` | R/W | CNTR (control); bits `PREST`=reset, `PDMD`=scsi-dma, `INTEN`, `TCEN`, `DDIR` |
| `0x80-0x83` | W | WTC — word transfer count (32 bit) |
| `0x84-0x87` | W | ACR — address count register (DMA destination) |
| `0x8E-0x8F` | W | DAWR |
| `0xA1` | R/W | **W = command byte to the drive; R = status byte from the drive** |
| `0xB0-0xBF` | R/W | **6525 (TPI)**, register = `(offset-0xB0)/2` (ports A,B,C,dir,etc.) |
| `0xE0/E1` | W | start DMA |
| `0xE2/E3` | W | stop DMA |
| `0xE4/E5` | W | clear ISTR |

6525 (TPI), registers 0–7: 0=port A (subcode in), 1=port B (control→drive + serial
volume), 2=port C (status lines SBCP/SCOR/STCH/STEN), 3/4=dir A/B, 5=dir C / interrupt
mask, 6=control (bit0 = MC, enables interrupt mode), 7=AIR.

ISTR bits: `INT_P`(0x10), `E_INT`(0x20) used for the end-of-DMA IRQ when
`CNTR_INTEN|CNTR_TCEN` are active.

---

## 13. Firmware checklist (what the device must do)

- [ ] Receive/assemble commands (variable length per opcode) — §2/§3.
- [ ] Reply with the output bytes handling `STEN` — §2.
- [ ] Maintain the status flags (ready/playing/finished/error/motor/media) — §4.
- [ ] Generate `STCH` on asynchronous events — §2/§9.
- [ ] Mode set/sense and sector-size reconstruction from the file — §5.
- [ ] DMA sector transfer (data handshake, 16-bit) — §5.
- [ ] TOC / capacity / disc info from CUE/ISO — §7.
- [ ] SubQ with a consistent position in BCD/MSF/LSN — §6.
- [ ] I²S audio 44.1 kHz + `C16M` + serial volume capture — §8.
- [ ] 96-byte/frame subcode generation with bit-reversal and timing — §9.
- [ ] Reply `MATSHITA0.96` to the `0x83` inquiry — §3.
