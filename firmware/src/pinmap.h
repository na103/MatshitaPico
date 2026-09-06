// =============================================================================
// MatshitaPico -- Solid-state CD-ROM drive replacement for the Commodore CDTV
// Copyright (c) 2026 Nicola Avanzi (na103)
// Licensed under Creative Commons Attribution 4.0 International (CC BY 4.0).
// See LICENSE.
// =============================================================================
// pinmap.h -- RP2350 / Pico 2 (A1) GPIO assignment.
// This is extracted from the KiCad schematic and is the hardware truth: the PCB
// is already routed on these pins. Do NOT change them without re-routing the PCB.
// Budget: 26/26 GPIO used (GP0-GP22 + GP26/27/28); GP23/24/25 are module-internal,
// GP25 = onboard status LED.
//
// CPLD contract (host_if.v / MatshitaPico_top.v):
//   CPLD -> Pico (via SN74CBTD16211, 5V->3.3V): CMD_STB, BYTE_REQ, CHAN, BCLK, LRCK, FRAME
//   Pico -> CPLD (direct, 3.3V->TTL):           BYTE_VLD, ST_AVAIL, DAT_AVAIL, STCH_REQ,
//                                               A_DATA, A_SUBQ, A_EMPH
//   bidirectional data bus (via shifter):       SD0..SD7
// =============================================================================
#pragma once
#include "pico/stdlib.h"

// ---- Host data bus SD[7:0] (bidirectional, via SN74CBTD16211) ----------------
// NOT contiguous: no PIO on the bus, use gather/scatter (see host_link.cpp). At
// CDTV rates (~6.5 us/byte at 1x) software gather/scatter is more than enough.
static constexpr uint PIN_SD0 = 16;
static constexpr uint PIN_SD1 = 17;
static constexpr uint PIN_SD2 = 18;
static constexpr uint PIN_SD3 = 12;
static constexpr uint PIN_SD4 = 19;
static constexpr uint PIN_SD5 = 11;
static constexpr uint PIN_SD6 = 20;
static constexpr uint PIN_SD7 = 10;
static constexpr uint PIN_SD[8] = {
    PIN_SD0, PIN_SD1, PIN_SD2, PIN_SD3, PIN_SD4, PIN_SD5, PIN_SD6, PIN_SD7
};
// mask of all data-bus bits (for gpio_set_dir_masked / get_all)
static constexpr uint32_t SD_BUS_MASK =
    (1u<<PIN_SD0)|(1u<<PIN_SD1)|(1u<<PIN_SD2)|(1u<<PIN_SD3)|
    (1u<<PIN_SD4)|(1u<<PIN_SD5)|(1u<<PIN_SD6)|(1u<<PIN_SD7);

// ---- Handshake CPLD -> Pico (inputs) -----------------------------------------
static constexpr uint PIN_CMD_STB  = 21;  // pulse: command byte on SD[] from the CPLD
static constexpr uint PIN_BYTE_REQ = 9;   // pulse: "give me the next byte" (status/data)
static constexpr uint PIN_CHAN     = 8;   // 0 = status, 1 = data (which next byte)

// ---- Handshake Pico -> CPLD (outputs) ----------------------------------------
static constexpr uint PIN_BYTE_VLD  = 13;  // valid byte placed on SD[] by the Pico
static constexpr uint PIN_ST_AVAIL  = 14;  // status bytes available -> STEN
static constexpr uint PIN_DAT_AVAIL = 27;  // data transfer ready -> DRQ
static constexpr uint PIN_STCH_REQ  = 28;  // async notification request -> STCH

// ---- Audio clocks from the CPLD (inputs; I2S slave) --------------------------
static constexpr uint PIN_BCLK  = 22;  // bit clock (C16M/8 = 48 fs)
static constexpr uint PIN_LRCK  = 7;   // L/R clock (fs = 44.1 kHz, high = L)
static constexpr uint PIN_FRAME = 6;   // KiCad net "FRAME", reused as SCOR: subcode
                                       // block sync (75 Hz) for the subq_tx PIO
                                       // (I2S aligns on LRCK, no longer needs it)

// ---- Audio / subcode Pico -> CPLD (outputs) ----------------------------------
static constexpr uint PIN_A_DATA = 2;  // serial audio sample (I2S, MSB-first)
static constexpr uint PIN_A_SUBQ = 1;  // subcode Q data stream (the CPLD makes the clocks)
static constexpr uint PIN_A_EMPH = 0;  // de-emphasis flag (per track)

// ---- SD card (3.3V, direct - does NOT pass through the shifter) --------------
// These 4 pins do NOT form any RP2350 hardware SPI; on rev.0 the SD is driven by
// SPI over PIO (pio2 free). See hw_config.c / sd_spi_pio.
static constexpr uint PIN_SD_SCK  = 5;   // CLK   (SD-05)
static constexpr uint PIN_SD_MOSI = 26;  // CMD   (SD-02)
static constexpr uint PIN_SD_MISO = 4;   // DAT0  (SD-07)
static constexpr uint PIN_SD_CS   = 15;  // DAT3  (SD-01)  external 10k pull-up to 3V3

// ---- UI ----------------------------------------------------------------------
static constexpr uint PIN_NEXT_BTN = 3;            // NEXT button -> GND, internal pull-up
static constexpr uint PIN_LED      = PICO_DEFAULT_LED_PIN; // GP25 onboard (status)
