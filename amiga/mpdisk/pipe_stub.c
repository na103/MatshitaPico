/* =============================================================================
 * MatshitaPico -- Solid-state CD-ROM drive replacement for the Commodore CDTV
 * Copyright (c) 2026 Nicola Avanzi (na103)
 * Licensed under Creative Commons Attribution 4.0 International (CC BY 4.0).
 * See LICENSE.
 * =============================================================================
 * pipe_stub.c -- STUB backend of mpdisk for TESTING IN WinUAE (no hardware).
 *
 * Implements the same interface as pipe.h, but instead of talking to the DMAC/
 * firmware it reads/writes `work.hdf` as a normal AmigaDOS FILE. device.c
 * (romtag, do_rw, geometry, IORequest handling) is IDENTICAL to the real build,
 * so the device framework can be validated in WinUAE with the same MountList.
 *
 * WinUAE setup (Kickstart 1.3):
 *   - make `work.hdf` reachable as a FILE (see STUB_PATHS below; do NOT also
 *     mount it as a WinUAE hardfile, or it stays locked);
 *   - copy this binary to DEVS:mpdisk.device (SAME name as the romtag);
 *   - copy the MountList (amiga/mount/MPDisk) to DEVS: and run: Mount MPD0:
 *   - then: Info MPD0:  and  Dir MPD0:
 * The init outcome is written to RAM:mpdisk-init.log (read it with Type).
 *
 * Build:  make stub   ->  mpdisk_stub.device
 * ============================================================================= */
#include "pipe.h"
#include <exec/types.h>
#include <exec/memory.h>
#include <dos/dos.h>
#include <dos/dosextens.h>
#include <proto/exec.h>
#include <proto/dos.h>

/* paths tried in order for work.hdf (first one that opens wins): covers the most
 * common WinUAE layouts. Add/change entries if your work.hdf lives elsewhere. */
static const char *STUB_PATHS[] = {
    "work.hdf", "SYS:work.hdf", "DH0:work.hdf", "DH1:work.hdf", "Work:work.hdf", 0
};

extern struct ExecBase *SysBase;      /* defined in device.c, for the exec inlines */
struct DosLibrary *DOSBase = 0;       /* proto/dos.h declares it extern: defined here */
static BPTR   g_fh      = 0;          /* work.hdf handle kept open (per-block I/O) */
static ULONG  g_bytes   = 0;
static ULONG  g_blocks  = 0;
static const char *g_path = 0;

/* --- VISIBLE log of the pipe_init outcome in RAM:mpdisk-init.log.
 *     Runs at device init (process context, dos.library open, NOT under Forbid). */
static ULONG slen(const char *s){ ULONG n=0; while (s[n]) n++; return n; }
static void  wr(BPTR f, const char *s){ if (s) Write(f, (APTR)s, (LONG)slen(s)); }
static void  wrhex(BPTR f, ULONG v){
    char b[11]; const char *h="0123456789ABCDEF"; int i;
    b[0]='0'; b[1]='x'; for (i=0;i<8;i++) b[2+i]=h[(v>>((7-i)*4))&0xF]; b[10]=0; wr(f,b);
}
static void stublog(const char *msg, const char *path, ULONG blocks){
    BPTR f = Open((STRPTR)"RAM:mpdisk-init.log", MODE_NEWFILE);
    if (!f) return;
    wr(f, "mpdisk STUB pipe_init: "); wr(f, msg);
    if (path)   { wr(f, " path="); wr(f, path); }
    wr(f, " blocks="); wrhex(f, blocks); wr(f, "\n");
    Close(f);
}

BOOL pipe_init(void) {
    BPTR lock;
    struct FileInfoBlock *fib;
    int i;

    DOSBase = (struct DosLibrary *)OpenLibrary((STRPTR)"dos.library", 0);
    if (!DOSBase) return FALSE;

    /* find work.hdf: try the known paths, take the first lockable one */
    lock = 0;
    for (i = 0; STUB_PATHS[i]; i++) {
        lock = Lock((STRPTR)STUB_PATHS[i], ACCESS_READ);
        if (lock) { g_path = STUB_PATHS[i]; break; }
    }
    if (!lock) {
        stublog("FAIL: work.hdf not found or LOCKED (remove it from the hardfile list!)", 0, 0);
        goto fail_lib;
    }
    fib = (struct FileInfoBlock *)AllocMem(sizeof(struct FileInfoBlock),
                                           MEMF_PUBLIC | MEMF_CLEAR);
    if (!fib) { UnLock(lock); stublog("FAIL: AllocMem FIB", g_path, 0); goto fail_lib; }
    g_bytes = Examine(lock, fib) ? (ULONG)fib->fib_Size : 0;
    FreeMem(fib, sizeof(struct FileInfoBlock));
    UnLock(lock);
    if (g_bytes < MPDISK_BLOCK) { stublog("FAIL: Examine/size", g_path, g_bytes); goto fail_lib; }
    g_blocks = g_bytes / MPDISK_BLOCK;

    /* per-block I/O: keep the file open (no 8 MB in RAM -> runs on WB 1.3).
     * MODE_READWRITE to support writes too; fall back to read-only. */
    g_fh = Open((STRPTR)g_path, MODE_READWRITE);
    if (!g_fh) g_fh = Open((STRPTR)g_path, MODE_OLDFILE);
    if (!g_fh) { stublog("FAIL: Open", g_path, g_blocks); goto fail_lib; }
    stublog("OK", g_path, g_blocks);
    return TRUE;

fail_lib:
    CloseLibrary((struct Library *)DOSBase);
    DOSBase = 0;
    g_blocks = 0;
    return FALSE;
}

UBYTE pipe_units(void)          { return g_fh ? 1 : 0; }
ULONG pipe_blocks(UBYTE unit)   { (void)unit; return g_blocks; }
UBYTE pipe_status(void)         { return (UBYTE)VST_READY; }

/* Per-block I/O via Seek+Read/Write. Under do_rw's Forbid() the Read does a Wait
 * (temporarily breaking the Forbid): harmless in the stub (no DMAC to protect;
 * work.hdf lives on a different handler than MPD0: -> no deadlock). */
BOOL pipe_read(UBYTE unit, ULONG lba, UBYTE cnt, APTR buf) {
    LONG n = (LONG)cnt * MPDISK_BLOCK;
    (void)unit;
    if (!g_fh || lba + cnt > g_blocks) return FALSE;
    if (Seek(g_fh, (LONG)(lba * MPDISK_BLOCK), OFFSET_BEGINNING) < 0) return FALSE;
    return (Read(g_fh, buf, n) == n) ? TRUE : FALSE;
}

BOOL pipe_write(UBYTE unit, ULONG lba, UBYTE cnt, CONST_APTR buf) {
    LONG n = (LONG)cnt * MPDISK_BLOCK;
    (void)unit;
    if (!g_fh || lba + cnt > g_blocks) return FALSE;
    if (Seek(g_fh, (LONG)(lba * MPDISK_BLOCK), OFFSET_BEGINNING) < 0) return FALSE;
    return (Write(g_fh, (APTR)buf, n) == n) ? TRUE : FALSE;
}
