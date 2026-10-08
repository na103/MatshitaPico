/* =============================================================================
 * MatshitaPico -- Solid-state CD-ROM drive replacement for the Commodore CDTV
 * Copyright (c) 2026 Nicola Avanzi (na103)
 * Licensed under Creative Commons Attribution 4.0 International (CC BY 4.0).
 * See LICENSE.
 * =============================================================================
 * mpdisk_dev.h -- base structure of mpdisk.device
 * ============================================================================= */
#ifndef MPDISK_DEV_H
#define MPDISK_DEV_H

#include <exec/types.h>
#include <exec/devices.h>
#include <exec/libraries.h>

#define MPDISK_VERSION   1
#define MPDISK_REVISION  0
#define MPDISK_NAME      "mpdisk.device"
#define MPDISK_MAXUNITS  4           /* disk0.hdf..disk3.hdf -> MPD0..MPD3    */

struct MpDiskBase {
    struct Library    md_Lib;        /* standard library/device node         */
    BPTR              md_SegList;    /* seglist (for Expunge)                */
    struct ExecBase  *md_SysBase;    /* copy of ExecBase                     */
    UBYTE             md_Units;      /* hardfiles exposed by firmware (<=4)  */
    UBYTE             md_Present;    /* TRUE if the mpdisk firmware answers  */
    ULONG             md_Blocks[MPDISK_MAXUNITS];  /* 512 B blocks per unit  */
};

#endif /* MPDISK_DEV_H */
