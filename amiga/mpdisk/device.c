/* =============================================================================
 * MatshitaPico -- Solid-state CD-ROM drive replacement for the Commodore CDTV
 * Copyright (c) 2026 Nicola Avanzi (na103)
 * Licensed under Creative Commons Attribution 4.0 International (CC BY 4.0).
 * See LICENSE.
 * =============================================================================
 * device.c -- mpdisk.device: exec framework (romtag/init/vectors) + dispatch of
 *             trackdisk-like commands onto the vendor pipe (pipe.c).
 * I/O is SYNCHRONOUS inside BeginIO, under Forbid(). Up to 4 units (MPD0..MPD3),
 * each with its own geometry read from the firmware. Kickstart 1.3 compatible.
 * ============================================================================= */
#include <exec/types.h>
#include <exec/resident.h>
#include <exec/nodes.h>
#include <exec/devices.h>
#include <exec/io.h>
#include <exec/errors.h>
#include <exec/memory.h>
#include <devices/trackdisk.h>
#include <proto/exec.h>

#include "mpdisk_dev.h"
#include "pipe.h"

struct ExecBase *SysBase;      /* global for the proto/exec.h inlines */

/* --- vector prototypes (register args, Amiga convention) --- */
static struct MpDiskBase *initDevice(struct MpDiskBase *dev  __asm("d0"),
                                     BPTR seglist            __asm("a0"),
                                     struct ExecBase *sysb   __asm("a6"));
static void  devOpen (struct MpDiskBase *dev __asm("a6"), struct IOStdReq *io __asm("a1"),
                      ULONG unit __asm("d0"), ULONG flags __asm("d1"));
static BPTR  devClose(struct MpDiskBase *dev __asm("a6"), struct IOStdReq *io __asm("a1"));
static BPTR  devExpunge(struct MpDiskBase *dev __asm("a6"));
static ULONG devNull(void);
static void  devBeginIO(struct MpDiskBase *dev __asm("a6"), struct IOStdReq *io __asm("a1"));
static ULONG devAbortIO(struct MpDiskBase *dev __asm("a6"), struct IORequest *io __asm("a1"));

static const char devName[]  = MPDISK_NAME;
static const char devIdStr[] = MPDISK_NAME " 1.0 (23.7.2026)\r\n";

/* function table: LVO -6 Open, -12 Close, -18 Expunge, -24 Null, -30 BeginIO, -36 AbortIO */
static const APTR funcTable[] = {
    (APTR)devOpen, (APTR)devClose, (APTR)devExpunge, (APTR)devNull,
    (APTR)devBeginIO, (APTR)devAbortIO,
    (APTR)-1
};

/* InitTable: base size, function table, dataTable (0), init */
static const ULONG initTable[4] = {
    sizeof(struct MpDiskBase),
    (ULONG)funcTable,
    0,
    (ULONG)initDevice
};

/* RomTag (Resident): AUTOINIT -> exec/MakeLibrary when the device is loaded */
const struct Resident romtag = {
    RTC_MATCHWORD,
    (struct Resident *)&romtag,
    (APTR)((const struct Resident *)&romtag + 1),   /* EndSkip: right after the romtag */
    RTF_AUTOINIT,
    MPDISK_VERSION,
    NT_DEVICE,
    0,
    (char *)devName,
    (char *)devIdStr,
    (APTR)initTable
};

/* --- init: called by MakeLibrary (d0=base, a0=seglist, a6=execbase) --- */
static struct MpDiskBase *initDevice(struct MpDiskBase *dev  __asm("d0"),
                                     BPTR seglist            __asm("a0"),
                                     struct ExecBase *sysb   __asm("a6")) {
    SysBase = sysb;
    dev->md_SysBase = sysb;
    dev->md_SegList = seglist;
    dev->md_Lib.lib_Node.ln_Type = NT_DEVICE;
    dev->md_Lib.lib_Node.ln_Name = (char *)devName;
    dev->md_Lib.lib_Version  = MPDISK_VERSION;
    dev->md_Lib.lib_Revision = MPDISK_REVISION;
    dev->md_Lib.lib_IdString = (APTR)devIdStr;

    /* find the DMAC + DETECT the mpdisk firmware (FALSE on a real drive) */
    dev->md_Present = pipe_init();
    if (dev->md_Present) {
        UBYTE u;
        dev->md_Units = pipe_units();
        if (dev->md_Units > MPDISK_MAXUNITS) dev->md_Units = MPDISK_MAXUNITS;
        for (u = 0; u < dev->md_Units; u++)
            dev->md_Blocks[u] = pipe_blocks(u);   /* per-unit geometry from the firmware */
    } else {
        dev->md_Units = 0;
    }
    return dev;   /* device always created: Open fails gracefully if !md_Present */
}

/* --- Open: validate unit + firmware presence, bump OpenCnt --- */
static void devOpen(struct MpDiskBase *dev __asm("a6"), struct IOStdReq *io __asm("a1"),
                    ULONG unit __asm("d0"), ULONG flags __asm("d1")) {
    (void)flags;
    if (!dev->md_Present || unit >= dev->md_Units) {
        io->io_Error = IOERR_OPENFAIL;
        io->io_Device = 0;
        return;
    }
    dev->md_Lib.lib_OpenCnt++;
    dev->md_Lib.lib_Flags &= ~LIBF_DELEXP;
    io->io_Unit  = (struct Unit *)(ULONG)unit;   /* "light" unit: just the unit number */
    io->io_Error = 0;
}

static BPTR devClose(struct MpDiskBase *dev __asm("a6"), struct IOStdReq *io __asm("a1")) {
    (void)io;
    if (dev->md_Lib.lib_OpenCnt > 0) dev->md_Lib.lib_OpenCnt--;
    return 0;   /* no delayed expunge (stays resident) */
}

static BPTR devExpunge(struct MpDiskBase *dev __asm("a6")) {
    (void)dev;
    return 0;   /* never expunged */
}

static ULONG devNull(void) { return 0; }

/* --- synchronous I/O --- */
static void do_rw(struct MpDiskBase *dev, struct IOStdReq *io, BOOL write) {
    ULONG off = io->io_Offset, len = io->io_Length;
    UBYTE *buf = (UBYTE *)io->io_Data;
    ULONG unit = (ULONG)io->io_Unit;            /* MPD0..MPD3 -> unit 0..3 */
    ULONG blocks = (unit < MPDISK_MAXUNITS) ? dev->md_Blocks[unit] : 0;
    ULONG lba, nblk;
    BOOL ok = TRUE;

    if ((off | len) & (MPDISK_BLOCK - 1)) { io->io_Error = IOERR_BADLENGTH; return; }
    lba  = off / MPDISK_BLOCK;
    nblk = len / MPDISK_BLOCK;
    if (lba + nblk > blocks) { io->io_Error = IOERR_BADADDRESS; return; }

    Forbid();                                   /* cdfs cannot start new CD reads meanwhile */
    while (nblk && ok) {
        /* WRITE: ONE block per command. Waiting for STATUS in pipe_write is a
         * per-block handshake -> the host does not send the next block until the
         * firmware has written to SD and answered, so no bytes are lost while
         * hardfile_write runs (with cnt>1 the CPLD's 2-byte FIFO would overflow
         * between blocks). READ: up to 255 blocks (split further in pipe_read). */
        UBYTE c = write ? 1 : ((nblk > 255) ? 255 : (UBYTE)nblk);
        ok = write ? pipe_write((UBYTE)unit, lba, c, buf)
                   : pipe_read ((UBYTE)unit, lba, c, buf);
        lba  += c;
        buf  += (ULONG)c * MPDISK_BLOCK;
        nblk -= c;
    }
    Permit();

    if (ok) io->io_Actual = len;
    else    io->io_Error  = TDERR_NotSpecified;
}

static void do_geometry(struct MpDiskBase *dev, struct IOStdReq *io) {
    struct DriveGeometry *dg = (struct DriveGeometry *)io->io_Data;
    ULONG unit   = (ULONG)io->io_Unit;
    ULONG blocks = (unit < MPDISK_MAXUNITS) ? dev->md_Blocks[unit] : 0;
    dg->dg_SectorSize   = MPDISK_BLOCK;
    dg->dg_TotalSectors = blocks;
    dg->dg_Heads        = 1;
    dg->dg_TrackSectors = 32;
    dg->dg_CylSectors   = 32;
    dg->dg_Cylinders    = blocks / 32;
    dg->dg_BufMemType   = MEMF_PUBLIC;
    dg->dg_DeviceType   = DG_DIRECT_ACCESS;
    dg->dg_Flags        = 0;
    io->io_Actual = sizeof(struct DriveGeometry);
}

static void devBeginIO(struct MpDiskBase *dev __asm("a6"), struct IOStdReq *io __asm("a1")) {
    io->io_Error = 0;
    switch (io->io_Command) {
        case CMD_READ:       do_rw(dev, io, FALSE); break;
        case CMD_WRITE:      do_rw(dev, io, TRUE);  break;
        case TD_GETGEOMETRY: do_geometry(dev, io);  break;
        case CMD_UPDATE:                            /* firmware syncs on every write */
        case CMD_CLEAR:      io->io_Actual = 0;      break;
        case TD_CHANGENUM:   io->io_Actual = 0;      break;  /* never changed      */
        case TD_CHANGESTATE: io->io_Actual = 0;      break;  /* 0 = disk present   */
        case TD_PROTSTATUS:  io->io_Actual = 0;      break;  /* not protected      */
        case TD_MOTOR:       io->io_Actual = 1;      break;  /* always on          */
        case TD_REMOVE:
        case TD_ADDCHANGEINT:
        case TD_REMCHANGEINT:
        case CMD_RESET: case CMD_STOP: case CMD_START: case CMD_FLUSH:
            break;                                          /* harmless no-op     */
        default:             io->io_Error = IOERR_NOCMD;    break;
    }
    if (!(io->io_Flags & IOF_QUICK))
        ReplyMsg(&io->io_Message);
}

static ULONG devAbortIO(struct MpDiskBase *dev __asm("a6"), struct IORequest *io __asm("a1")) {
    (void)dev; (void)io;
    return 0;   /* synchronous I/O: nothing to abort */
}
