/* =============================================================================
 * MatshitaPico -- Solid-state CD-ROM drive replacement for the Commodore CDTV
 * Copyright (c) 2026 Nicola Avanzi (na103)
 * Licensed under Creative Commons Attribution 4.0 International (CC BY 4.0).
 * See LICENSE.
 * =============================================================================
 * pipe.c -- mpdisk vendor pipe implementation (see pipe.h)
 *
 * The DMAC/6525 is SHARED with the ROM-resident cdtv.device, which stays alive
 * (SCOR interrupts at 75 Hz, CD reads by cdfs). Everything here is written to
 * coexist with it: STEN replies are caught by our own high-priority INT2 server,
 * and the READ DMA is arbitrated against cdtv.device's "CD DMA in flight" flag.
 * ============================================================================= */
#include "pipe.h"
#include <exec/types.h>
#include <exec/memory.h>
#include <exec/interrupts.h>
#include <exec/execbase.h>
#include <hardware/intbits.h>
#include <libraries/configvars.h>
#include <proto/exec.h>
#include <proto/expansion.h>

extern struct ExecBase *SysBase;      /* defined in device.c (for FindName/Disable) */

/* --- registers (offsets from the DMAC base) -- map/sequence from the ROM's
 *     cdtv.device: DMA setup = f057f4, polled handshake = f05d8a. --- */
#define R_ISTR   0x41      /* interrupt status (R)                            */
#define R_CNTR   0x43      /* control (R/W)                                   */
#define R_WTC    0x80      /* word transfer count, 32 bit big-endian 0x80..83 */
#define R_ACR    0x84      /* DMA address, 32 bit big-endian 0x84..87         */
#define R_DAWR   0x8F      /* DMA Address Width Register (config byte)        */
#define R_CMD    0xA1      /* command (W) / response (R)                      */
#define R_PB     0xB3      /* 6525 port B: bit0-1 = read handshake (f05a7c)   */
#define R_PRC    0xB5      /* 6525 port C: bit3=STEN, bit2=STCH               */
#define R_DDRC   0xBB      /* 6525 DDRC = IRQ MASK (in interrupt mode)        */
#define R_AIR    0xBF      /* 6525 Active Interrupt Register (read = ack)     */
#define R_STDMA  0xE0      /* strobe: START DMA (any write)                   */
#define R_SPDMA  0xE2      /* strobe: STOP DMA                                */
#define R_CINT   0xE4      /* strobe: clear ISTR                              */
#define R_FLUSH  0xE8      /* strobe: FLUSH the DMAC's internal buffer        */

/* CNTR bits */
#define CNTR_TCEN  (1<<7)  /* terminal count enable (arms the WTC countdown)  */
#define CNTR_PREST (1<<6)
#define CNTR_PDMD  (1<<5)  /* 1 = SCSI DMA; 0 = CD DMA (our case)             */
#define CNTR_INTEN (1<<4)  /* IRQ line enable: NOT used (we poll)             */
#define CNTR_DDIR  (1<<3)  /* 0 = device->memory (read); 1 = memory->device   */
/* ISTR bits */
#define ISTR_E_INT (1<<5)  /* end-of-process (EOP) = DMA complete             */

/* AIR/PRC bit3 = STEN: set = reply byte ready on 0xA1 (as the ROM's f05d8a). */
#define STEN_BIT   0x08
#define TIMEOUT    200000UL   /* busy-wait iterations */

static volatile UBYTE *g_base = 0;      /* DMAC base (cd_BoardAddr) */
static UBYTE g_units = 0;

/* --- DMAC ARBITRATION with the CD (reverse of cdtv.device, ext-ROM v1.0) ------
 * cdtv.device keeps a "CD DMA active" flag at 272(a6) (offset 0x110 from its
 * device base): set to (len+300) right after START (f057e8), cleared by its INT2
 * handler on completion (f05862) and by teardown/timeout (f05840).
 * != 0  =>  a CD DMA is in flight OR its completion is not yet serviced. Then we
 * must NOT touch the DMAC: (a) in flight -> we would corrupt the CD transfer;
 * (b) completion pending -> our CINT (clear ISTR) would eat the EOP that
 * cdtv.device's INT2 handler still has to see -> cdfs hangs.
 * PASSIVE read (a plain load, no command -> the CD DMA is not disturbed).
 * g_cdtv_dma==0 (no cdtv.device) -> no contention possible. */
#define CDTV_DMA_ACTIVE_OFF 0x110
static volatile ULONG *g_cdtv_dma = 0;

static void find_cdtv_dma_flag(void) {
    struct Node *dev;
    g_cdtv_dma = 0;
    Forbid();
    dev = FindName(&SysBase->DeviceList, (STRPTR)"cdtv.device");
    Permit();
    if (dev)
        g_cdtv_dma = (volatile ULONG *)((UBYTE *)dev + CDTV_DMA_ACTIVE_OFF);
}

#define REG8(off)   (*(volatile UBYTE  *)(g_base + (off)))
#define REG16(off)  (*(volatile UWORD  *)(g_base + (off)))
#define REG32(off)  (*(volatile ULONG  *)(g_base + (off)))

/* --- STEN reply capture via INTERRUPT (not polling) -------------------------
 * The CPLD asserts STEN as a ~118 us pulse per byte and drives the bus only in
 * that window: reading 0xA1 OUTSIDE it gives 0xFF and no byte_req. Polling for
 * it is a race (the AIR bit is short-lived, PRC is dead in interrupt mode). The
 * ROM catches it with the INTERRUPT (edge latched in hardware = reliable), and so
 * do we: our own INT2 (PORTS) server, at HIGH priority, sniffs STEN and reads
 * 0xA1 into our buffer BEFORE cdtv.device's server (which, with its count at 0,
 * would discard the byte). Each read of 0xA1 = byte_req -> the firmware serves
 * the next byte -> new STEN -> new INT2: the reply bytes chain by themselves.
 * g_rx.arm gates the server to the transaction window only. */
static struct {
    UBYTE       *buf;
    volatile int idx;
    int          exp;
    volatile int arm;
    volatile int done;
} g_rx;

static struct Interrupt g_is;
static int g_is_added = 0;

static void send_cmd(UBYTE op, UBYTE unit, ULONG lba, UBYTE cnt);   /* fwd */

static int mpd_int_server(void) {
    volatile UBYTE *base = g_base;
    if (base && g_rx.arm && (base[R_AIR] & STEN_BIT)) {
        UBYTE b;
        /* handshake as the ROM handler f05a7c: assert _ENABLE (PB bit0-1=0) so the
         * CPLD drives the reply bus during _HRD, read, deassert. Without it 0xA1
         * reads 0xFF and no byte_req is generated. */
        base[R_PB] &= ~3;
        b = base[R_CMD];                   /* 0xA1 = byte + STEN ack + advance */
        base[R_PB] |= 3;
        if (g_rx.idx < g_rx.exp) {
            g_rx.buf[g_rx.idx++] = b;
            if (g_rx.idx >= g_rx.exp) g_rx.done = 1;
        }
        /* byte CONSUMED -> return "handled" (d0!=0) = STOP the INT2 chain, so
         * cdtv.device's server does not run on this INT2 and cannot steal the byte.
         * On SCOR INT2s (bit3=0, below) we return 0 -> the chain goes on and the ROM
         * handles SCOR normally. */
        return 1;
    }
    return 0;   /* not ours: continue the chain (cdtv.device handles SCOR) */
}

/* EXCLUSIVE session on the DMAC/6525 around a firmware transaction.
 *
 * pipe_lock():   Forbid() (no task switch, the CD stays idle) + install our INT2
 *                server at priority 127, ahead of cdtv.device. NOT Disable(): our
 *                server MUST run to catch STEN. The 6525 registers are not
 *                touched (masking its IRQs via DDRC rebooted the CDTV).
 * pipe_unlock(): remove the server, Permit(). */
static void pipe_lock(void) {
    Forbid();
    if (g_base && !g_is_added) {
        g_is.is_Node.ln_Type = NT_INTERRUPT;
        g_is.is_Node.ln_Pri  = 127;                 /* MAX: ahead of cdtv.device's server */
        g_is.is_Node.ln_Name = (char *)"mpdisk";
        g_is.is_Data         = 0;                   /* uses global g_rx, not is_Data */
        g_is.is_Code         = (void (*)())mpd_int_server;
        g_rx.arm = 0; g_rx.done = 0; g_rx.idx = 0;
        AddIntServer(INTB_PORTS, &g_is);
        g_is_added = 1;
    }
}
static void pipe_unlock(void) {
    g_rx.arm = 0;
    if (g_is_added) {
        RemIntServer(INTB_PORTS, &g_is);
        g_is_added = 0;
    }
    Permit();
}

/* Arm the server, send the command and wait for the interrupt to collect n reply
 * bytes (or timeout).
 *
 * RETRY: cdtv.device's INT2 server is also in the PORTS chain and now and then
 * still steals ONE STEN byte (the STEN edge is transient) -> incomplete/shifted
 * reply. If we did not collect all n bytes, RESEND the command (the command bytes
 * flush the firmware's old state -> fresh reply), up to MAX_TRY times.
 * DETECT/GEOMETRY/STATUS are idempotent reads; WRITE sends its payload BEFORE this
 * (the final STATUS is idempotent). */
#define MAX_TRY 8
static BOOL cmd_resp(UBYTE op, UBYTE unit, ULONG lba, UBYTE cnt, UBYTE *dst, int n) {
    int try;
    for (try = 0; try < MAX_TRY; try++) {
        ULONG to;
        g_rx.buf = dst; g_rx.exp = n; g_rx.idx = 0; g_rx.done = 0;
        g_rx.arm = 1;                                /* from here the server sniffs STEN */
        send_cmd(op, unit, lba, cnt);
        to = TIMEOUT;
        while (!g_rx.done) { if (!--to) break; }
        g_rx.arm = 0;
        if (g_rx.idx >= n) return TRUE;              /* all bytes are ours */
        /* race lost (1 byte to the ROM server): retry with a fresh reply */
    }
    return FALSE;
}

/* Write a 32-bit value to a DMAC register (WTC/ACR) as ONE 32-bit store.
 * CRITICAL: the DMAC latches WTC/ACR only on word/long cycles of the 68000 bus.
 * Four byte writes (0x80..83) are single-lane UDS/LDS cycles -> the word count is
 * NOT latched and the DMA never clocks. The ROM (f057f4) uses `move.l d0,128(a5)`. */
static void reg32(int off, ULONG v) {
    REG32(off) = v;
}

/* Pacing between command bytes. Written back-to-back (~1 us each) the firmware
 * loses the TAIL of the command. The CDTV ROM paces its bytes naturally (6 bytes,
 * then a ~ms gap before the last); we replicate it with a busy delay per byte.
 * TUNABLE (perf): shorter = less per-command overhead; if mount/reads FAIL (lost
 * command bytes -> "not a DOS disk"/read error) -> RAISE it. */
#define CMD_GAP_ITERS 100
static void cmd_gap(void) {
    volatile ULONG i;
    for (i = 0; i < CMD_GAP_ITERS; i++) { }
}

/* Per-byte pacing of the bulk-rx PAYLOAD (write). The firmware drains the payload
 * in a tight loop and keeps up with the host at ~1 us/byte, with the CPLD's 2-byte
 * FIFO as margin -> a MINIMAL gap is enough to absorb jitter.
 * TUNABLE: if writes CORRUPT -> RAISE; if clean and you want speed -> LOWER. */
#define BULK_GAP_ITERS 8
static void bulk_gap(void) {
    volatile ULONG i;
    for (i = 0; i < BULK_GAP_ITERS; i++) { }
}

/* Send a 7-byte vendor command (op, unit, LBA[4], count) by writing them to R_CMD;
 * each write = one HWR strobe = one byte in the firmware's feed_command_byte().
 * Gap AFTER bytes 1..6 only: on the 7th byte the firmware executes and asserts STEN,
 * which our INT2 server catches whenever it happens. */
static void send_cmd(UBYTE op, UBYTE unit, ULONG lba, UBYTE cnt) {
    REG8(R_CMD) = op;                 cmd_gap();
    REG8(R_CMD) = unit;               cmd_gap();
    REG8(R_CMD) = (UBYTE)(lba >> 24); cmd_gap();
    REG8(R_CMD) = (UBYTE)(lba >> 16); cmd_gap();
    REG8(R_CMD) = (UBYTE)(lba >> 8);  cmd_gap();
    REG8(R_CMD) = (UBYTE)(lba);       cmd_gap();
    REG8(R_CMD) = cnt;   /* no gap after the last: STEN is edge-latched -> timing irrelevant */
}

BOOL pipe_init(void) {
    struct Library *ExpansionBase;
    struct ConfigDev *cd;

    g_base = 0; g_units = 0;
    ExpansionBase = OpenLibrary("expansion.library", 0);
    if (!ExpansionBase) return FALSE;
    cd = FindConfigDev(0, MPDISK_MANUF, MPDISK_PROD);
    CloseLibrary(ExpansionBase);
    if (!cd) return FALSE;                       /* no CDTV DMAC = not a CDTV */
    g_base = (volatile UBYTE *)cd->cd_BoardAddr;
    find_cdtv_dma_flag();                        /* for DMAC arbitration (pipe_read) */

    /* DETECT: "MPDK" signature + version + n_units + block size */
    {
        UBYTE r[8];
        BOOL ok;
        pipe_lock();
        ok = cmd_resp(VOP_DETECT, 0, 0, 0, r, 8);   /* STEN caught via interrupt */
        pipe_unlock();
        if (!ok) return FALSE;
        if (r[0] != 'M' || r[1] != 'P' || r[2] != 'D' || r[3] != 'K') return FALSE;
        g_units = r[5];
        /* r[6..7] = block size (512): the device assumes MPDISK_BLOCK */
    }
    return TRUE;
}

UBYTE pipe_units(void) { return g_units; }

ULONG pipe_blocks(UBYTE unit) {
    UBYTE r[6];
    BOOL ok;
    if (!g_base) return 0;
    pipe_lock();
    ok = cmd_resp(VOP_GEOMETRY, unit, 0, 0, r, 6);
    pipe_unlock();
    if (!ok) return 0;
    return ((ULONG)r[0] << 24) | ((ULONG)r[1] << 16) | ((ULONG)r[2] << 8) | r[3];
}

UBYTE pipe_status(void) {
    UBYTE r[2];
    BOOL ok;
    if (!g_base) return 0;
    pipe_lock();
    ok = cmd_resp(VOP_STATUS, 0, 0, 0, r, 2);
    pipe_unlock();
    if (!ok) return 0;
    return r[0];
}

/* READ via DMA, ARBITRATED with the CD's use of the DMAC (cdtv.device f057f4/f05832).
 *
 * Safe sequence for each chunk:
 *   Disable()  -> cdtv.device FROZEN: all its DMAC accesses happen in task or INT2
 *      context, both masked by Disable(); the only remaining actor is the DMAC
 *      hardware finishing a transfer ALREADY started.
 *   if 272(a6)!=0 (CD DMA in flight/to be serviced): Enable() + backoff and retry
 *      (let the pending EOP fire -> cdtv's INT2 handler completes and clears it).
 *      do_rw calls us under Forbid() -> cdfs cannot start NEW CD reads meanwhile ->
 *      at most ONE CD DMA in flight on entry, so the wait is bounded.
 *   else: program WTC/ACR/DAWR/CNTR(no INTEN)/STDMA, send 0x62 (the firmware pushes
 *      via DRQ), poll ISTR&E_INT, teardown (FLUSH/SPDMA/WTC=0/CINT/PB), Enable().
 *
 * ARB_CHUNK blocks per window keep the Disable() window short (~ms), well below
 * cdtv.device's DMA watchdog (~400 ms) and harmless for audio (I2S/Paula, not the
 * 68000). The buffer must be reachable by the DMAC (stock CDTV = chip RAM). */
#define ARB_CHUNK    64          /* blocks (32 KB) per window: ~19 ms of DMA under
                                  * Disable. TUNABLE: if the system gets unstable, lower it. */
#define ARB_MAX_TRY  4000UL      /* arbitration retry cap (~1 s before giving up) */

static BOOL dma_read_chunk(UBYTE unit, ULONG lba, UBYTE cnt, UBYTE *buf) {
    ULONG words = ((ULONG)cnt * MPDISK_BLOCK) >> 1;
    ULONG try;
    for (try = 0; try < ARB_MAX_TRY; try++) {
        ULONG to;
        BOOL ok = TRUE;
        Disable();                               /* cdtv.device frozen (task+INT2) */
        if (g_cdtv_dma && *g_cdtv_dma != 0) {    /* CD DMA in flight or to be serviced */
            volatile ULONG i;
            Enable();                            /* let the EOP fire -> cdtv completes */
            for (i = 0; i < 200; i++) { }        /* short backoff */
            continue;
        }
        /* --- ATOMIC DMAC SETUP under Disable (register writes only, ~us).
         * PB handshake (ROM f057ca/d0: bclr#1/bset#0 on 0xB3) = gate of the CPLD's
         * DMA data path. STDMA ARMS the DMAC (it waits for DRQ; 0x62 not sent yet). */
        REG8(R_PB) = (UBYTE)((REG8(R_PB) & ~0x02) | 0x01);
        reg32(R_WTC, words);                     /* 0x80 word count (move.l) */
        reg32(R_ACR, (ULONG)buf);                /* 0x84 destination address (move.l) */
        REG8(R_DAWR)  = 0x00;                     /* 0x8F DAWR (ROM value 325(a6)=0) */
        REG8(R_CNTR)  = CNTR_TCEN;                /* 0x43=0x80: TCEN, CD, read, INTEN off */
        REG16(R_STDMA) = (UWORD)words;            /* 0xE0: ARM the DMA (move.w of word count) */
        Enable();                                 /* <-- interrupts back ON for the slow part */

        /* --- slow SEND with interrupts ON. send_cmd takes ~28 ms (cmd_gap in
         * contended chip RAM): under Disable we would miss cdtv.device's SCOR INT2s
         * (system freeze). Forbid() in do_rw still keeps cdfs from starting a CD read;
         * with 272==0 cdtv's INT2 does not touch the DMAC. The DMAC stays ARMED idle
         * until the 7th byte arrives -> the firmware serves DRQ. */
        send_cmd(VOP_READ, unit, lba, cnt);

        /* --- POLL EOP + teardown under Disable again (~1 ms, the DMA itself): HIDES
         * our E_INT from cdtv.device's INT2 (seeing ISTR&0x30==0x30 it would clear
         * ISTR via its CINT and we would lose the EOP). The DMA only starts NOW
         * (after the last byte of send_cmd), so the whole E_INT window is covered. */
        Disable();
        to = TIMEOUT;
        while (!(REG8(R_ISTR) & ISTR_E_INT)) {   /* wait for OUR EOP */
            if (!--to) { ok = FALSE; break; }
        }
        /* Teardown as cdtv.device (f05832): FLUSH writes the last buffered word to
         * RAM; then STOP, WTC=0, clear ISTR, restore PB. */
        REG16(R_FLUSH) = 0;                       /* 0xE8: FLUSH */
        REG16(R_SPDMA) = 0;                       /* 0xE2: STOP DMA */
        reg32(R_WTC, 0);                         /* WTC = 0 */
        REG16(R_CINT)  = 0;                       /* 0xE4: clear ISTR (OUR EOP) */
        REG8(R_PB)    |= 0x03;                     /* 0xB3: restore PB handshake */
        Enable();
        return ok;
    }
    return FALSE;   /* DMAC never free (extreme contention) -> error, cdfs/OFS retries */
}

BOOL pipe_read(UBYTE unit, ULONG lba, UBYTE cnt, APTR buf) {
    UBYTE *dst = (UBYTE *)buf;
    if (!g_base || cnt == 0) return FALSE;
    while (cnt) {                                 /* split into short Disable() windows */
        UBYTE c = (cnt > ARB_CHUNK) ? ARB_CHUNK : cnt;
        if (!dma_read_chunk(unit, lba, c, dst)) return FALSE;
        lba += c;
        dst += (ULONG)c * MPDISK_BLOCK;
        cnt -= c;
    }
    return TRUE;
}

/* WRITE via bulk-rx: send the command, then stream cnt*512 bytes to R_CMD (one HWR
 * strobe per byte = one bulk-rx byte in the firmware). No DMA (the data channel is
 * one-way, device->host). */
BOOL pipe_write(UBYTE unit, ULONG lba, UBYTE cnt, CONST_APTR buf) {
    const UBYTE *p = (const UBYTE *)buf;
    ULONG i, bytes;
    UBYTE st[2];
    BOOL ok;
    if (!g_base || cnt == 0) return FALSE;
    bytes = (ULONG)cnt * MPDISK_BLOCK;

    pipe_lock();
    send_cmd(VOP_WRITE, unit, lba, cnt);
    for (i = 0; i < bytes; i++) {
        REG8(R_CMD) = p[i];                       /* one payload byte into bulk-rx */
        bulk_gap();                               /* minimal pacing vs the CPLD 2-byte FIFO */
    }
    /* Before STATUS: give the firmware time to write the block to SD (~ms).
     * Otherwise the first 0x63 bytes arrive while it is busy and get lost (cmd_resp's
     * retry would recover them; this avoids the extra round). */
    cmd_gap(); cmd_gap();
    /* outcome via inline STATUS (not pipe_status: pipe_lock is not re-entrant) */
    ok = cmd_resp(VOP_STATUS, 0, 0, 0, st, 2);
    pipe_unlock();
    return (ok && !(st[0] & VST_ERROR)) ? TRUE : FALSE;
}
