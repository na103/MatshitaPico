/* =============================================================================
 * MatshitaPico -- Solid-state CD-ROM drive replacement for the Commodore CDTV
 * Copyright (c) 2026 Nicola Avanzi (na103)
 * Licensed under Creative Commons Attribution 4.0 International (CC BY 4.0).
 * See LICENSE.
 * =============================================================================
 * pipe.h -- mpdisk vendor pipe (Amiga <-> RP2350 firmware) over the CDTV DMAC/6525
 *
 * The CDTV CD controller (DMAC U36 + 6525 U32) is a Zorro II AUTOCONFIG device
 * (manufacturer 0x0202 = Commodore/514, product 0x03): its base is found with
 * expansion.library/FindConfigDev, NOT hardcoded. Offsets from that base:
 *
 *   0x41  ISTR  (interrupt status, R)        0x43  CNTR  (control, R/W)
 *   0x80  WTC   (word transfer count, 32b)   0x84  ACR   (DMA address, 32b)
 *   0xA1  command (W) / response (R)         0xB0.. 6525 TPI (odd addresses)
 *   0xB5  PRC (port C: bit3=STEN, bit2=STCH, bit1=SCOR)
 *   0xE0/0xE2/0xE4  START/STOP/clear-ISTR DMA strobes
 * The DMA sequence and the response handshake follow the CDTV extended ROM's own
 * cdtv.device (DMA setup at f057f4, response handshake at f05d8a/f05a7c).
 *
 * Vendor protocol (contract with firmware cdtv_drive.cpp, opcodes 0x60-0x64):
 *   command = 7 bytes: op, unit, LBA[4 MSB-first], count (1..255 512-byte blocks)
 *   0x60 DETECT   -> STEN reply 8B: "MPDK" ver nunits blk_hi blk_lo
 *   0x64 GEOMETRY -> STEN reply 6B: total_blocks(4) block_size(2)
 *   0x62 READ     -> DMA drive->host: count*512 B (DRQ)
 *   0x61 WRITE    -> bulk-rx: count*512 B follow, written to 0xA1 (1 byte/HWR)
 *   0x63 STATUS   -> STEN reply 2B: [flags: b0=ready b1=err b2=busy], 0
 * See docs/drive-commands.md.
 * ============================================================================= */
#ifndef MPDISK_PIPE_H
#define MPDISK_PIPE_H

#include <exec/types.h>

#define MPDISK_BLOCK      512      /* bytes per block (WinUAE-style hardfile)      */
#define MPDISK_MANUF      0x0202   /* Commodore West Chester (514)                 */
#define MPDISK_PROD       0x03     /* CDTV DMAC / CD controller                    */

/* vendor opcodes */
#define VOP_DETECT        0x60
#define VOP_WRITE         0x61
#define VOP_READ          0x62
#define VOP_STATUS        0x63
#define VOP_GEOMETRY      0x64

/* 0x63 STATUS flags */
#define VST_READY         0x01
#define VST_ERROR         0x02
#define VST_BUSY          0x04

/* Initialise the pipe: find the DMAC (FindConfigDev), check the "MPDK" signature
 * with DETECT. Returns TRUE if the mpdisk firmware answers (a real drive = FALSE ->
 * the device must degrade gracefully). */
BOOL  pipe_init(void);

/* Number of hardfiles (units) exposed by the firmware (from DETECT). */
UBYTE pipe_units(void);

/* Total 512 B blocks of the unit; 0 if unknown/out of range. */
ULONG pipe_blocks(UBYTE unit);

/* Read/write 'cnt' 512 B blocks (1..255) at the given LBA. TRUE on success.
 * For reads, buf must be reachable by the DMAC (chip RAM on a stock CDTV). */
BOOL  pipe_read (UBYTE unit, ULONG lba, UBYTE cnt, APTR buf);
BOOL  pipe_write(UBYTE unit, ULONG lba, UBYTE cnt, CONST_APTR buf);

/* Status byte (0x63). */
UBYTE pipe_status(void);

#endif /* MPDISK_PIPE_H */
