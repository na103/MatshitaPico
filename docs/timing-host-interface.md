<!--
  MatshitaPico -- Solid-state CD-ROM drive replacement for the Commodore CDTV
  Copyright (c) 2026 Nicola Avanzi (na103)
  Licensed under Creative Commons Attribution 4.0 International (CC BY 4.0).
  See LICENSE.
-->

# LC8950/LC8951 host-interface timing (datasheet Appendix F)

Derived from the Sanyo LC8950/LC8951 datasheet, Appendix F / TABLE 12 (AC Electrical
Characteristics) and Figures 38–42. These are the timing constraints that the **replacement
device's front-end must meet on CN9** toward the CDTV (the host side of the LC8951). The
LC8950 and LC8951 have **identical** values for these parameters.

> **Fundamental constant: `T` = the XTALCK clock period.**
> In the CDTV, XTALCK ≈ **16.9344 MHz** ⇒ **T ≈ 59.05 ns**.
> Many times are expressed as multiples of T.
>
> Handy values: 2T≈118 ns · 4T≈236 ns · 6T≈354 ns · 8T≈472 ns · 10T≈590 ns.

All values in **ns** unless noted. "—" = not specified.

---

## 1. Command write — host→drive (Figure 38)

Sequence: the host asserts `ENABLE`, sets `CMD` (0 = command/status), places the byte on
`HD0-7`, pulses `HWR`. The drive takes it in and updates `DTEN`/`STEN`.

| # | Parameter | min | max |
|---|-----------|----:|----:|
| 1 | `ENABLE` active → `HWR` (setup) | 30 | — |
| 2 | `CMD` → `HWR` (setup) | 15 | — |
| 3 | `HWR` low (pulse width) | 50 | — |
| 4 | `HWR` → `DTEN` change | — | 105 |
| 5 | `HWR` → `STEN` change | — | 105 |
| 6 | data `HD0-7` → `HWR` (setup) | 50 | — |
| 7 | `HWR` inactive → data end (data hold) | 20 | — |
| 8 | `HWR` inactive → `ENABLE` inactive | 0 | — |
| 9 | `HWR` inactive → `CMD` inactive | 5 | — |
| 10 | data `HD0-7` → `HWR` inactive (hold) | 0 | — |

→ **Our side (drive)**: we must *sample* `HD0-7` on the `HWR` edge (data stable from ≥50 ns
before, hold ~0 ns) and produce the `DTEN`/`STEN` transition within **105 ns**. All times
easily handled by a CPLD.

---

## 2. Data read — drive→host (Figure 39 = WAIT, Figure 40 = DRQ)

`SELDRQ` selects the mode:
- **DRQ** (Figure 40): `WAIT`/`DRQ` acts as a *data request*. Flow:
  `ENABLE`↓ → `CMD`=1 (data read) → `DTEN`↓ (data ready) → `DRQ`↓ (request) →
  the host pulses `HRD` for each byte, the drive drives `HD0-7` → `EOP`↓ on the last byte.
- **WAIT** (Figure 39): software transfer; the drive raises `WAIT` if the host reads too
  fast, the host waits for `WAIT` to go high again.

### Parameters (Figure 39/40)

| # | Parameter | min | max |
|---|-----------|----:|----:|
| 31 | `HRD` active → `DTEN` inactive | — | 120 |
| 32 | `HRD` → `EOP` (setup) | — | 120 |
| 33 | `HRD` inactive → `ENABLE` inactive | 0 | — |
| 34 | `HRD` inactive → `CMD` active | 5 | — |
| 35 | `HRD` inactive → `EOP` inactive | — | 120 |
| 36 | `CMD` active → `HRD` active | 15 | — |
| 37 | `DTEN` active → `WAIT` inactive | 5 | 25 |
| 38 | `WAIT` period | 6T−20 (≈334) | — |
| 39 | `WAIT` inactive → `HRD` active | 0 | — |
| 40 | `HRD` → `WAIT` (setup) | — | 80 |
| 41 | `HRD` active period | 2T (≈118) | — |
| 42 | `HRD` → data valid (setup) | — | **120** |
| 43 | `HRD` inactive → `WAIT` inactive | 2T+10 (≈128) | 4T−75 (≈161) |
| 44 | data valid → data end (hold) | 0 | — |
| 44b| `HRD` active → `DTEN` inactive | 6T−… | — |

Datasheet notes:
1. `WAIT`=LOW appears only if the second `HRD` falling edge does not exceed the indicated
   times.
2. The minimum time for which `WAIT`=LOW does *not* occur.
3. `HRD` LOW and HIGH are assumed ≥ 50 ns.
4. If the `HRD` LOW pulse is 50 < t < 120 ns, the host does **not** read valid data on
   `HD0-7` → the data is guaranteed only after **120 ns** from `HRD`.

→ **Our side (drive)**: the critical constraint is **#42**: present the data on `HD0-7`
**within 120 ns** of `HRD` going active. Also `DTEN`/`EOP` must react to `HRD` within
**120 ns** (#31, #32, #35).

### Rate and bandwidth
- Minimum read period ≈ **6T ≈ 354 ns/byte** ⇒ the chip's peak bandwidth ≈ **2.8 MB/s**
  (consistent with the stated "2.3 MB/s").
- The **CDTV is 1×**: 75 sectors/s × 2048 = **153.6 KB/s ≈ 6.5 µs/byte**, ~18× slower than
  the chip's limit. ⇒ On *rate* we have huge margin; only the **setup/hold** and the
  **≤120 ns response** of the data after `HRD` matter.

---

## 3. Reset timing (Figure 41)

| Parameter | min | max |
|-----------|----:|----:|
| `RESET` pulse width | 100 | — |
| Controller data hold | — | 50 |
| Controller data delay | — | 50 |
| Host data hold | — | 50 |
| Host data delay | — | 50 |
| RAM data hold/delay | — | 50 |
| `DOUT` hold / delay | — | 100 |
| `STEN`, `DTEN` hold | — | 100 |
| `EOP` hold | — | 100 |
| `INT` hold | — | 100 |

→ `RESET` minimum **100 ns**; all outputs settle/release within 50–100 ns.

---

## 4. Controller side (Figure 42) — internal to our device

These are the bus timings between the drive µC and the LC8951 (`AS`, `CS`, `RD`, `WR`,
`D0-7`): AS setup/hold, CS setup/hold, RD/WR pulse width, access time, data-out delay,
tri-state delay, write data setup/hold. **They are not exposed on CN9**: in the replacement
this logic is implemented internally (firmware + CPLD), so these times are not a constraint
toward the CDTV — we realize them as needed. Not detailed here (unreliable diagram OCR;
consult Figure 42 in the datasheet if a faithful two-bus model needs replicating).

---

## 5. Design implications

1. **Ample throughput margin**: the bottleneck is the CDTV (1×, 6.5 µs/byte), not our
   logic. No exotic hardware is needed for bandwidth.
2. **Tight constraints only on response delays** (≈ tens–120 ns):
   - sample `HD0-7` on the `HWR` edge (command in);
   - present valid data within **120 ns** of `HRD` (data out, #42);
   - update `DTEN`/`STEN`/`EOP` within **105–120 ns** of their respective triggers.
3. **Those 120 ns are perfect for an ATF1508AS CPLD** (typical pin-to-pin delay 7–10 ns):
   the host-interface state machine runs comfortably. Even an RP2350 PIO could hold the
   data-after-HRD, but the 5 V CPLD gives margin and handles the 5 V bus natively.
4. **Suggested partitioning**:
   - *CPLD*: decode `ENABLE`/`CMD`/`HWR`/`HRD`/`SELDRQ`, latch the command byte, drive
     `DTEN`/`STEN`/`DRQ`/`EOP`, mux the data/status byte, meet the #1–#44 timings. It keeps
     a small handoff buffer/register toward the MCU.
   - *RP2350*: prepares the bytes (status, command output, sectors from SD) and makes them
     available to the CPLD before the CDTV requests them (the 6.5 µs/byte margin allows this
     with DMA + double buffering).
5. **`C16M` (= XTALCK, 16.9344 MHz)**: a dedicated crystal oscillator; it is the reference
   `T` of all the times above and is also the audio master clock.

