/* =============================================================================
 * MatshitaPico -- Solid-state CD-ROM drive replacement for the Commodore CDTV
 * Copyright (c) 2026 Nicola Avanzi (na103)
 * Licensed under Creative Commons Attribution 4.0 International (CC BY 4.0).
 * See LICENSE.
 * =============================================================================
 * mpmount.c -- mounts the mpdisk.device hardfiles as MPD0..MPD3, with the REAL
 *              geometry read from the firmware (self-describing: changing the
 *              size of a .hdf does NOT require rebuilding the boot CD).
 *
 * MINIMAL ENTRY (crt0.S, -nostartfiles): Bebbo's standard crt0 auto-opens
 * utility.library, ABSENT on KS 1.3 -> crash before main on the CDTV. Its 32/32
 * division also uses utility.library/UDivMod32 -> crt0.S provides SOFTWARE
 * __udivsi3/__umodsi3.
 *
 * No intermediate files (RAM: is not mounted on this minimal boot, and the only
 * writable storage is the very hardfile we are about to mount). So the DosNode is
 * built at runtime with MakeDosNode/AddDosNode (OFS DOS\0, GlobVec=0), like the
 * static Mountlist in amiga/mount/MPDisk.
 *
 * For each unit 0..3: OpenDevice (fails on missing units) -> TD_GETGEOMETRY ->
 * HighCyl = TotalSectors/32 - 1 -> MakeDosNode/AddDosNode.
 * ============================================================================= */
#include <exec/types.h>
#include <exec/execbase.h>
#include <exec/io.h>
#include <exec/memory.h>
#include <devices/trackdisk.h>
#include <libraries/filehandler.h>
#include <libraries/expansion.h>
#include <libraries/expansionbase.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/expansion.h>
#include <clib/alib_protos.h>

#define MPDISK_MAXUNITS 4                     /* disk0.hdf..disk3.hdf -> MPD0..MPD3   */
#define DOSTYPE_OFS     0x444F5300UL          /* 'DOS\0' = Old File System, KS 1.3 ROM */

struct ExecBase      *SysBase;                 /* set by crt0.S                        */
struct DosLibrary    *DOSBase;
struct ExpansionBase *ExpansionBase;           /* used by the proto/expansion inlines  */

/* MakeDosNode parmPacket: DOS name (C string!), device name (C string!), unit,
 * flags, then the DosEnvec (de_TableSize..de_DosType). MakeDosNode wants both
 * names as C STRINGS, not BPTRs: it builds the dn_Name/dn_Handler BSTRs itself. */
struct MountParm {
    ULONG dosName;
    ULONG execName;
    ULONG unit;
    ULONG flags;
    ULONG env[17];                             /* de_TableSize(0) .. de_DosType(16)   */
};

static void mount_unit(ULONG unit, ULONG total_blocks) {
    struct MountParm parm;
    struct DeviceNode *dn;
    char name[6];

    name[0] = 'M'; name[1] = 'P'; name[2] = 'D';
    name[3] = (char)('0' + unit); name[4] = 0;

    parm.dosName  = (ULONG)name;
    parm.execName = (ULONG)"mpdisk.device";
    parm.unit     = unit;
    parm.flags    = 0;
    parm.env[0]   = 16;                         /* de_TableSize = DE_DOSTYPE           */
    parm.env[1]   = 128;                        /* de_SizeBlock = 512/4 longword       */
    parm.env[2]   = 0;                          /* de_SecOrg                           */
    parm.env[3]   = 1;                          /* de_Surfaces                         */
    parm.env[4]   = 1;                          /* de_SectorPerBlock                   */
    parm.env[5]   = 32;                         /* de_BlocksPerTrack                   */
    parm.env[6]   = 2;                          /* de_Reserved                         */
    parm.env[7]   = 0;                          /* de_PreAlloc                         */
    parm.env[8]   = 0;                          /* de_Interleave                       */
    parm.env[9]   = 0;                          /* de_LowCyl                           */
    parm.env[10]  = total_blocks / 32 - 1;      /* de_HighCyl (firmware geometry)      */
    parm.env[11]  = 30;                         /* de_NumBuffers                       */
    parm.env[12]  = 1;                          /* de_BufMemType = MEMF_PUBLIC         */
    parm.env[13]  = 0x00ffffff;                 /* de_MaxTransfer                      */
    parm.env[14]  = 0x00fffffe;                 /* de_Mask (24-bit DMAC, even)         */
    parm.env[15]  = 0;                          /* de_BootPri                          */
    parm.env[16]  = DOSTYPE_OFS;                /* de_DosType = OFS                    */

    dn = MakeDosNode(&parm);
    if (dn) {
        dn->dn_GlobalVec = 0;                   /* ROM OFS (KS 1.3), as the Mountlist  */
        dn->dn_StackSize = 4000;
        dn->dn_Priority  = 5;
        /* ADNF_STARTPROC = start the filesystem NOW (not on first access): the
         * volume is mounted/validated immediately -> its icon shows up on the
         * Workbench at boot without a `dir MPDn:` to "wake it up". */
        AddDosNode(0, ADNF_STARTPROC, dn);
    }
}

/* called by __start (crt0.S) after opening dos.library */
int run(void) {
    struct MsgPort *port;
    struct IOExtTD *io;
    ULONG unit;
    int   n = 0;

    ExpansionBase = (struct ExpansionBase *)OpenLibrary((CONST_STRPTR)"expansion.library", 0);
    if (!ExpansionBase) return 20;

    port = CreatePort(0, 0);
    io   = (struct IOExtTD *)CreateExtIO(port, sizeof(struct IOExtTD));
    if (!port || !io) {
        if (io)   DeleteExtIO((struct IORequest *)io);
        if (port) DeletePort(port);
        CloseLibrary((struct Library *)ExpansionBase);
        return 20;
    }

    for (unit = 0; unit < MPDISK_MAXUNITS; unit++) {
        struct DriveGeometry dg;
        ULONG blocks = 0;
        if (OpenDevice((CONST_STRPTR)"mpdisk.device", unit, (struct IORequest *)io, 0) != 0)
            continue;                          /* missing unit -> skip it            */
        io->iotd_Req.io_Command = TD_GETGEOMETRY;
        io->iotd_Req.io_Data    = &dg;
        io->iotd_Req.io_Length  = sizeof(dg);
        if (DoIO((struct IORequest *)io) == 0 && dg.dg_TotalSectors >= 32)
            blocks = dg.dg_TotalSectors;
        CloseDevice((struct IORequest *)io);

        if (blocks >= 32) {
            mount_unit(unit, blocks);
            n++;
        }
    }

    DeleteExtIO((struct IORequest *)io);
    DeletePort(port);
    CloseLibrary((struct Library *)ExpansionBase);
    return n ? 0 : 5;
}
