<!--
  MatshitaPico — Solid-state CD-ROM drive replacement for the Commodore CDTV
  Copyright (c) 2026 Nicola Avanzi (na103)
  Licensed under Creative Commons Attribution 4.0 International (CC BY 4.0).
  See LICENSE.
-->

# MatshitaPico — a solid-state CD-ROM drive for the Commodore CDTV

**MatshitaPico** is a solid-state replacement for the optical CD-ROM drive of the
**Commodore CDTV**: BIN/CUE disc images stored on a microSD
card take the place of the optical mechanism.

**Why:** many CDTV drives have a worn-out laser and cannot be repaired, and no drop-in
replacement has ever existed. This project keeps these machines alive.

> This repository contains the **CPLD design (HDL)**, **technical documentation**, **RP2350 firmware** and **KiCad hardware project of beta board**.

---

## Community beta

Because of the limited turnout in the project's closed beta testing, I have decided to open
MatshitaPico testing up to the whole Amiga community.

The final version should not differ much from this first one, apart from an ATF1508 CPLD in a
TQFP-100 package, programmable in-circuit through a JTAG connector. The key point for the final
version is the **PCB layout**: it must be designed together with a 3D-printed mounting bracket.
The original eject button could also be reused as a control button.

This first version **is** installable — as you can see from the photos [here](https://github.com/na103/MatshitaPico/tree/main/img/beta_assembly) —
but several modifications are needed to fit it. When I laid out the PCB I did not worry about
its dimensions.

## The key insight

The CDTV service manual contains **no schematic for the CD-ROM subsystem**: all of the
CD-ROM electronics (CD-DSP, the Sanyo **LC8951** decoder, the drive's own microcontroller)
live on a PCB **inside the drive mechanism**, not on the motherboard. The motherboard only
carries the *host side* — the **DMAC (U36)**, the **6525/TPI (U32)** and glue logic — which
talks to the drive through connector **CN9** (42 pins, sheet 10 of schematic #252605).

→ **CN9 is the emulation point.** The device plugs into CN9 and pretends to be the entire
drive. The LC8951 does **not** interpret commands (it is a host↔µC FIFO mailbox); the
replacement emulates both the LC8951 host interface and the drive µC command protocol
(the proprietary CDTV/Sony command set).

## Hardware architecture

```
   CDTV (CN9, 5 V TTL)
        |  DB0-7, _HWR/_HRD/_CMD/_STEN/_DTEN/DRQ/_ENABLE/_STCH + RESET
        v
   CPLD  ATF1508AS  (5 V, 128 macrocells, PLCC84, JTAG; socketed, programmed out of circuit)
        |  - real-time 5 V front-end; host-interface FSM (DRQ mode)
        |  - clock division C16M -> BCLK/LRCK; SCOR / subcode generation
        |  <- level shifting 5 V <-> 3.3 V (SN74CBTD16211 bus switch, no DIR, 5 V clamp)
        v
   MCU  RP2350 / Raspberry Pi Pico 2  (3.3 V)
        - drive command protocol - BIN/CUE from SD
        - I2S audio 44.1 kHz (PIO) - subcode Q - UI (NEXT + LED)
   + 16.9344 MHz crystal oscillator (C16M, DIP14 metal-can 5 V TTL)
```

**Key constraint:** the CN9 bus is 5 V TTL and the RP2350 is **not** 5 V-tolerant, so a
native 5 V front-end is required (**ATF1508AS**, the `AS` variant, not `ASV`). The CPLD
performs four things in parallel — master clock, I2S audio, subcode, host DMA — plus command
glue logic; the heavy protocol logic runs on the RP2350.

## What the CPLD does

The CPLD is the only chip that faces CN9 (a pure 5 V domain). It contains:

- **`clkgen`** — derives the audio clocks (`BCLK`, `LRCK`, frame sync) from the `C16M`
  master clock (16.9344 MHz = 384 x 44.1 kHz), so the audio stays phase-coherent with the
  motherboard DAC (Sanyo LC7883M).
- **`host_if`** — the LC8951-style host-interface FSM: it captures host commands (async latch
  on the rising edge of `_HWR`), drives status replies over `_HRD`/`_STEN`, and runs the DMA
  data path in **DRQ mode** (`DRQ` high = byte ready). `EOP` is not on CN9 — the DMAC counts
  it — so ending a sector is just lowering `DRQ`.
- **top level** — subcode generation (`SBCP`/`SCOR`/`EFFK`; `SCCK` is a host **input**) and
  buffering of `DATA`/`EMPHASIS` from the RP2350.

## The golden reference

`cdtv.cpp` from WinUAE (plus `cdtvcr.cpp` for the CDTV-CR variant) is a complete emulation of
the DMAC + 6525 + LC8951 + command protocol. It is the **executable specification** the
firmware is ported from. (The WinUAE sources are not redistributed here — see the WinUAE
project.) The CDTV extended ROM is the real host-side protocol engine.

## Drive command set (summary)

`0x01` seek · `0x02` read · `0x04`/`0x05` motor on/off · `0x09`/`0x0a` play LSN/MSF ·
`0x0b` play track range · `0x82` clear-error · `0x84` mode set · `0x85` mode sense ·
`0x86` capacity · `0x87` subq · `0x8a` read TOC · `0xa2`/`0xa3` front panel.

The full, byte-level command reference — including the vendor `0x6x` hardfile page — is in
[`docs/drive-commands.md`](docs/drive-commands.md).

## Status

The device is functionally complete on real hardware: data boot, persistent disc change,
CD audio (play / pause / skip / stop), CD+G graphics, and the activity LED all work on a real
CDTV. 

## Thanks
Thankyou to all the people who have support this project.<br>
Andrea Quagliarini, Mirco Gaggiottini, Carlo Santagostino, Federico Di Dato.

## License

This work is licensed under the **Creative Commons Attribution 4.0 International License
(CC BY 4.0)**. See [`LICENSE`](LICENSE). You are free to share and adapt the material for any
purpose, provided you give appropriate credit.

<br><br>
If you found this my work useful, please consider buying me a cup of coffee if you want:<br>
<a href='https://ko-fi.com/na103' target='_blank'><img height='36' style='border:0px;height:36px;' src='https://storage.ko-fi.com/cdn/cup-border.png' border='0' alt='Buy Me a Coffee at ko-fi.com' /></a>
