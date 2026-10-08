#!/usr/bin/env python3
# =============================================================================
# MatshitaPico -- Solid-state CD-ROM drive replacement for the Commodore CDTV
# Copyright (c) 2026 Nicola Avanzi (na103)
# Licensed under Creative Commons Attribution 4.0 International (CC BY 4.0).
# See LICENSE.
# =============================================================================
"""mkbootcd.py -- builds a BOOTABLE CDTV CD for mpdisk.device (MODE1/2352 BIN/CUE).

Two boot routes:
  Route B (default, --boot trademark): like the original CDTV discs -- the 'TM'
      tag in the PVD Application-Use points to CDTV.TM, taken from the user's
      genuine CDTV disc. LBA 0 stays zero.
  Route A (--bootblock): DOS\0 bootblock in the System Area (LBA 0), no
      Commodore trademark. The ROM's cdstrap (f0f656) reads 1024 B from LBA 0,
      checks the DOS\0/DOS\1 signature + standard bootblock checksum, and
      boots immediately if valid.
Once the disc is accepted, the CDTV mounts CD0: and runs S/startup-sequence.

The CD contains an ISO9660 filesystem (PVD @16) with:
  S/STARTUP-SEQUENCE     (--startup, default amiga/boot/startup-sequence)
  DEVS/MPDISK.DEVICE     (--device, default amiga/mpdisk/mpdisk.device)
  C/MPMOUNT              (--mount-tool, dynamic self-describing mount MPD0..MPD3)
  C/<AmigaDOS commands>  (MOUNT, ASSIGN, EXECUTE, ... see C_COMMANDS)

FREE OVERLAY (--extra, default amiga/boot/cd-extra): the whole tree of that
folder is merged into the CD. Names become UPPERCASE (ISO9660); files starting
with '.' are ignored; a file with the same name as a default one REPLACES it.

The Commodore AmigaDOS commands are NOT in the repository (copyright): they are
extracted at build time from the user's own genuine CDTV disc (--cdisc
<disc.bin> MODE1/2352) or from a user's Workbench 1.3 C: directory (--cdir <dir>).
Personal use only.

Usage:
  python3 tools/mkbootcd.py <out_base> --cdisc "<genuine CDTV .bin>"
  python3 tools/mkbootcd.py <out_base> --cdir <C_dir> --tm <CDTV.TM>
  python3 tools/mkbootcd.py --extract-cache <dir> --cdisc "<genuine CDTV .bin>"
  options: [--startup f] [--device f] [--mount-tool f] [--extra dir]
           [--volid MPDISK] [--iso only.iso] [--bootblock]
    -> <out_base>.bin + <out_base>.cue (MODE1/2352 raw, for the SD card)
Needs iso2bincue.py (EDC/ECC engine, numpy) in the same folder.
"""
import os
import struct
import sys

USR = 2048  # user bytes per logical sector (ISO9660 block size)

# AmigaDOS commands needed by the startup-sequence (+ handy for debugging).
C_COMMANDS = ["MOUNT", "INFO", "ECHO", "ASSIGN", "WAIT", "FAILAT", "DIR", "LIST",
              "RESIDENT", "EXECUTE", "COPY", "PATH", "RMTM", "LOADWB", "NEWCLI"]
# RMTM = "Remove TradeMark": it MUST be in C: and be the FIRST command of the
# startup-sequence, otherwise the CDTV stays on the splash screen (white logo).
# RMTM/LoadWB/NewCLI are not in the /C directory of a genuine CDTV disc: copy
# them into the C: cache by hand (RMTM from the root of the CDTV Demo Disc,
# LoadWB/NewCLI from a Workbench 1.3 disk).
C_REQUIRED = {"MOUNT", "ASSIGN", "EXECUTE"}

# ------------------------------------------------------------------ bootblock

def _dos_bootcode():
    """Standard DOS\\0 bootblock (1024 B): signature + checksum + canonical code.

    cdstrap does NOT run this code (it only uses signature+checksum as a gate),
    but it is kept correct in case a later Kickstart boot runs it:
    FindResident("dos.library") -> jmp rt_Init. Entry: a6 = ExecBase.
    """
    bb = bytearray(1024)
    bb[0:4] = b"DOS\x00"                # 0x444F5300
    # checksum (bb[4:8]) stays 0 until computed
    struct.pack_into(">I", bb, 8, 0)   # rootblock: 0 (irrelevant on CD)
    code = bytes([
        0x2C, 0x78, 0x00, 0x04,        # 0C movea.l 4.w,a6      ; a6 = ExecBase
        0x43, 0xFA, 0x00, 0x12,        # 10 lea    (0x12,pc),a1 ; -> "dos.library" @0x24
        0x4E, 0xAE, 0xFF, 0xA0,        # 14 jsr    -96(a6)      ; FindResident
        0x20, 0x40,                    # 18 movea.l d0,a0
        0x20, 0x68, 0x00, 0x16,        # 1A movea.l 22(a0),a0   ; rt_Init
        0x4E, 0xD0,                    # 1E jmp    (a0)
        0x00, 0x00, 0x00, 0x00,        # 20 pad -> string aligned at 0x24
    ])
    bb[0x0C:0x0C + len(code)] = code
    name = b"dos.library\x00"
    bb[0x24:0x24 + len(name)] = name
    _set_bootcksum(bb)
    return bytes(bb)


def _set_bootcksum(bb):
    """Set bb[4:8] to the standard bootblock checksum (ROM algorithm f0fcd8:
    sum of the 256 longs with end-around carry, then NOT; valid if == 0)."""
    bb[4:8] = b"\x00\x00\x00\x00"
    s = _sum_carry(bb)
    chk = (~s) & 0xFFFFFFFF
    struct.pack_into(">I", bb, 4, chk)
    assert _bootcheck_ok(bb), "invalid bootblock checksum"


def _sum_carry(bb):
    s = 0
    for i in range(0, 1024, 4):
        s += struct.unpack_from(">I", bb, i)[0]
        if s > 0xFFFFFFFF:
            s = (s & 0xFFFFFFFF) + 1
    return s & 0xFFFFFFFF


def _bootcheck_ok(bb):
    """Exact replica of f0fcd8: d0=0; for 256 longs d0+=long (carry->+1); not.l;
    valid if the result == 0."""
    return ((~_sum_carry(bb)) & 0xFFFFFFFF) == 0


# ------------------------------------------------------------------ ISO9660

def _both16(v):
    return struct.pack("<H", v) + struct.pack(">H", v)

def _both32(v):
    return struct.pack("<I", v) + struct.pack(">I", v)

def _drec_datetime():
    """Directory record date/time (7 byte): 1985-01-01 00:00:00 GMT."""
    return bytes([85, 1, 1, 0, 0, 0, 0])

def _pvd_datetime():
    """PVD date/time (17 byte ASCII 'YYYYMMDDHHMMSShh' + offset GMT)."""
    return b"1991010100000000" + bytes([0])


class Node:
    def __init__(self, name, is_dir, data=b""):
        self.name = name          # ISO identifier (no ;1 for dirs)
        self.is_dir = is_dir
        self.data = data          # contents (file)
        self.children = []        # (dir)
        self.lba = 0
        self.length = 0           # bytes of contents / dir extent
        self.dir_num = 0          # path-table number (dir)
        self.parent = None

    def add(self, node):
        node.parent = self
        self.children.append(node)
        return node


def _dir_record(name_bytes, lba, length, is_dir):
    """Build an ISO9660 directory record (even length)."""
    fi = name_bytes
    rec = bytearray(33 + len(fi))
    rec[1] = 0                                   # ext attr len
    rec[2:10] = _both32(lba)                     # extent
    rec[10:18] = _both32(length)                 # data length
    rec[18:25] = _drec_datetime()
    rec[25] = 0x02 if is_dir else 0x00           # flags
    rec[26] = 0                                   # file unit size
    rec[27] = 0                                   # interleave gap
    rec[28:32] = _both16(1)                      # volume seq number
    rec[32] = len(fi)
    rec[33:] = fi
    if len(rec) % 2:                              # pad to even length
        rec.append(0)
    rec[0] = len(rec)
    return bytes(rec)


def _sort_key(node):
    # ISO9660: sort by identifier, space padded; '.' and '..' handled apart.
    ident = node.name if node.is_dir else node.name + ";1"
    return ident.encode("ascii")


def _build_dir_extent(node, sectors_used):
    """Serialize a directory extent (records . .. children), never letting a
    record cross a sector boundary (zero pad)."""
    parent = node.parent or node
    # The "." and ".." records MUST carry the data length of the dir (own /
    # parent): the CDTV cdfs navigates using the length in the "." record -- with
    # 0 it treats the dir as empty and finds no files (red error screen).
    recs = [
        _dir_record(b"\x00", node.lba, node.length, True),      # .
        _dir_record(b"\x01", parent.lba, parent.length, True),  # ..
    ]
    for ch in sorted(node.children, key=_sort_key):
        ident = (ch.name if ch.is_dir else ch.name + ";1").encode("ascii")
        recs.append(_dir_record(ident, ch.lba, ch.length, ch.is_dir))

    out = bytearray()
    for r in recs:
        if (len(out) % USR) + len(r) > USR:               # do not cross 2048
            out += b"\x00" * (USR - (len(out) % USR))
        out += r
    if len(out) % USR:
        out += b"\x00" * (USR - (len(out) % USR))
    return bytes(out)


def _path_table(dirs, big):
    """Path table (L if big=False, M if big=True) from the ordered dir nodes."""
    out = bytearray()
    for d in dirs:
        ident = b"\x00" if d.parent is None else d.name.encode("ascii")
        rec = bytearray(8 + len(ident))
        rec[0] = len(ident)
        rec[1] = 0
        if big:
            struct.pack_into(">I", rec, 2, d.lba)
            struct.pack_into(">H", rec, 6, d.dir_num_parent)
        else:
            struct.pack_into("<I", rec, 2, d.lba)
            struct.pack_into("<H", rec, 6, d.dir_num_parent)
        rec[8:] = ident
        if len(rec) % 2:
            rec.append(0)
        out += rec
    return bytes(out)


def build_iso(tree, volid, bootblock=False, tm_node=None):
    """tree = root node (Node dir). Returns the bytes of the ISO9660 image.

    bootblock=True  -> puts a DOS\\0 bootblock at LBA0 (Route A).
    tm_node != None -> writes the 'TM' tag in the PVD App-Use pointing to that
                       file's extent (Route B, CDTV trademark; LBA0 stays zero
                       as on the original discs)."""
    # ---- directories in path-table order (BFS, sorted children) ----
    dirs = []
    queue = [tree]
    while queue:
        n = queue.pop(0)
        dirs.append(n)
        queue.extend(sorted([c for c in n.children if c.is_dir], key=_sort_key))
    for i, d in enumerate(dirs, 1):
        d.dir_num = i
    for d in dirs:
        d.dir_num_parent = (d.parent.dir_num if d.parent else 1)

    # ---- directory sizes (to assign the LBAs) ----
    def dir_size(node):
        total = 34 + 34                     # . and ..
        used = total
        for ch in sorted(node.children, key=_sort_key):
            ident = (ch.name if ch.is_dir else ch.name + ";1").encode("ascii")
            rlen = 33 + len(ident)
            if rlen % 2:
                rlen += 1
            if (used % USR) + rlen > USR:
                used += USR - (used % USR)
            used += rlen
        return ((used + USR - 1) // USR) * USR

    for d in dirs:
        d.length = dir_size(d)

    # ---- layout LBA ----
    # 0..15 System Area (bootblock at 0), 16+17 PVD, then VDST, PT-L, PT-M
    pt_l_size = len(_path_table(dirs, False))
    pt_m_size = len(_path_table(dirs, True))
    pt_l_sects = (pt_l_size + USR - 1) // USR
    pt_m_sects = (pt_m_size + USR - 1) // USR
    lba = 16
    pvd_lba = lba; lba += 1
    pvd2_lba = lba; lba += 1          # duplicated PVD (as ISOCD/genuine disc)
    vdst_lba = lba; lba += 1
    pt_l_lba = lba; lba += pt_l_sects
    pt_m_lba = lba; lba += pt_m_sects
    # directory extents
    for d in dirs:
        d.lba = lba
        lba += d.length // USR
    # file extents
    files = []
    def collect_files(n):
        for ch in sorted(n.children, key=_sort_key):
            if ch.is_dir:
                collect_files(ch)
            else:
                files.append(ch)
    collect_files(tree)
    for f in files:
        f.lba = lba
        f.length = len(f.data)
        lba += (len(f.data) + USR - 1) // USR
    total_sectors = lba

    # ---- tail padding (lead-out) ----
    # The CDTV cdfs reads files with READ-AHEAD (bursts of several sectors): on a
    # tiny CD, reading a file in the LAST sector overruns the image -> the
    # firmware does a SHORT read -> the host DMAC never completes -> cdfs errors,
    # resets (0x82) and re-reads = endless LOOP on the logo. (WinUAE hides it by
    # returning zeros past EOF.) Empty trailing sectors keep the read-ahead inside
    # the image. They are not part of the Volume Space (lead-out only).
    LEAD_PAD = 34                      # > largest read-ahead observed (17)

    # ---- build the image ----
    img = bytearray((total_sectors + LEAD_PAD) * USR)

    # bootblock (LBA 0) only in Route A; in Route B (trademark) LBA0 stays zero.
    if bootblock:
        img[0:1024] = _dos_bootcode()

    # PVD (LBA 16)
    pvd = bytearray(USR)
    pvd[0] = 1
    pvd[1:6] = b"CD001"
    pvd[6] = 1
    pvd[8:40] = b"CDTV".ljust(32)                              # system id: MARKER CDTV
    pvd[40:72] = volid.encode("ascii").ljust(32)[:32]          # volume id
    pvd[80:88] = _both32(total_sectors)                        # volume space size
    pvd[120:124] = _both16(1)                                  # volume set size
    pvd[124:128] = _both16(1)                                  # volume seq number
    pvd[128:132] = _both16(USR)                                # block size
    pvd[132:140] = _both32(pt_l_size)                          # path table size
    struct.pack_into("<I", pvd, 140, pt_l_lba)                 # PT-L loc
    struct.pack_into("<I", pvd, 144, pt_l_lba)                 # PT-L opt (= primary, as ISOCD)
    struct.pack_into(">I", pvd, 148, pt_m_lba)                 # PT-M loc
    struct.pack_into(">I", pvd, 152, pt_m_lba)                 # PT-M opt (= primary)
    pvd[156:190] = _dir_record(b"\x00", tree.lba, tree.length, True)
    for off in (190, 318, 446, 574):                           # misc ids (a-chars)
        pvd[off:off + 128] = b" " * 128
    for off in (702, 739, 776):                                # file id (37)
        pvd[off:off + 37] = b" " * 37
    pvd[813:830] = _pvd_datetime()                             # creation
    pvd[830:847] = _pvd_datetime()                             # modification
    pvd[847:864] = b"0" * 16 + bytes([0])                      # expiration (none)
    pvd[864:881] = b"0" * 16 + bytes([0])                      # effective (none)
    pvd[881] = 1                                               # file structure version
    # App-Use (883..1394): trademark 'TM' tag (Route B) or zero (Route A).
    # Format (from ROM f0b924 + the bytes of a genuine disc), big-endian:
    #   883: 0x00 (leading byte; the parser starts at 884)
    #   884..885: 'TM'    886..887: len = 0x0014 (20)
    #   888..891: size    892..895/896../900../904..: LBA0..LBA3 (identical)
    if tm_node is not None:
        tm = bytearray()
        tm += b"\x00"
        tm += b"TM"
        tm += struct.pack(">H", 20)
        tm += struct.pack(">I", len(tm_node.data))
        tm += struct.pack(">I", tm_node.lba) * 4
        pvd[883:883 + len(tm)] = tm
    # PVD written TWICE (16 and 17), as ISOCD and the genuine disc
    img[pvd_lba * USR:pvd_lba * USR + USR] = pvd
    img[pvd2_lba * USR:pvd2_lba * USR + USR] = pvd

    # VDST (LBA 18)
    vdst = bytearray(USR)
    vdst[0] = 255
    vdst[1:6] = b"CD001"
    vdst[6] = 1
    img[vdst_lba * USR:vdst_lba * USR + USR] = vdst

    # path tables
    ptl = _path_table(dirs, False)
    ptm = _path_table(dirs, True)
    img[pt_l_lba * USR:pt_l_lba * USR + len(ptl)] = ptl
    img[pt_m_lba * USR:pt_m_lba * USR + len(ptm)] = ptm

    # directory extents (now that all LBAs are known)
    for d in dirs:
        ext = _build_dir_extent(d, d.length // USR)
        assert len(ext) == d.length, f"dir {d.name}: {len(ext)} != {d.length}"
        img[d.lba * USR:d.lba * USR + len(ext)] = ext

    # file
    for f in files:
        img[f.lba * USR:f.lba * USR + len(f.data)] = f.data

    return bytes(img)


# ---------------------------------------------------- C: extraction from a BIN

def _extract_c_from_bin(bin_path):
    """Extract the AmigaDOS commands from the /C dir of a genuine CDTV disc
    (MODE1/2352). Returns {NAME: bytes}."""
    RAW, HDR = 2352, 16

    def rs(f, l):
        f.seek(l * RAW + HDR); return f.read(USR)

    def rext(f, l, n):
        cnt = (n + USR - 1) // USR
        return b"".join(rs(f, l + i) for i in range(cnt))[:n]

    def pdir(data):
        e = []; off = 0
        while off < len(data):
            rl = data[off]
            if rl == 0:
                nxt = ((off // USR) + 1) * USR
                if nxt >= len(data):
                    break
                off = nxt; continue
            lba = struct.unpack_from("<I", data, off + 2)[0]
            ln = struct.unpack_from("<I", data, off + 10)[0]
            fl = data[off + 25]; nl = data[off + 32]
            nm = data[off + 33:off + 33 + nl]
            e.append((nm, lba, ln, fl)); off += rl
        return e

    out = {}
    with open(bin_path, "rb") as f:
        pvd = rs(f, 16)
        if pvd[1:6] != b"CD001":
            sys.exit(f"{bin_path}: PVD not found (is it a MODE1/2352 BIN?)")
        root_rec = pvd[156:190]
        root_lba = struct.unpack_from("<I", root_rec, 2)[0]
        root_len = struct.unpack_from("<I", root_rec, 10)[0]
        root_entries = pdir(rext(f, root_lba, root_len))
        cdir = None
        for nm, lba, ln, fl in root_entries:
            if nm.upper() == b"C" and (fl & 2):
                cdir = (lba, ln); break
        if not cdir:
            sys.exit(f"{bin_path}: directory /C not found")
        want = set(C_COMMANDS)
        for nm, lba, ln, fl in pdir(rext(f, cdir[0], cdir[1])):
            base = nm.split(b";")[0].upper().decode("latin1")
            if base in want and not (fl & 2):
                out[base] = rext(f, lba, ln)
        # trademark CDTV.TM;1 in the root (Route B)
        for nm, lba, ln, fl in root_entries:
            if nm.split(b";")[0].upper() == b"CDTV.TM" and not (fl & 2):
                out["@TM"] = rext(f, lba, ln)
                break
    return out


def _extract_c_from_dir(cdir):
    out = {}
    for fn in os.listdir(cdir):
        up = fn.upper()
        if up in C_COMMANDS:
            with open(os.path.join(cdir, fn), "rb") as f:
                out[up] = f.read()
    return out


# ------------------------------------------------------------------ free overlay

def merge_overlay(root, extra_dir):
    """Recursively merge the file tree in extra_dir into the ISO (root Node).

    - names become UPPERCASE (ISO9660 identifiers);
    - files/dirs starting with '.' are ignored (.gitkeep, .DS_Store, ...);
    - an existing dir (C/, S/, DEVS/) is MERGED, not duplicated;
    - a file with the same name as an existing one is REPLACED (overlay wins).
    Returns the list of added paths (for the log)."""
    added = []

    def child(node, name, is_dir):
        for c in node.children:
            if c.name == name and c.is_dir == is_dir:
                return c
        return None

    def walk(fsdir, node, prefix):
        for fn in sorted(os.listdir(fsdir)):
            if fn.startswith("."):
                continue
            path = os.path.join(fsdir, fn)
            nm = fn.upper()
            if os.path.isdir(path):
                sub = child(node, nm, True) or node.add(Node(nm, True))
                walk(path, sub, prefix + nm + "/")
            else:
                with open(path, "rb") as f:
                    data = f.read()
                ex = child(node, nm, False)
                if ex is not None:
                    ex.data = data
                else:
                    node.add(Node(nm, False, data))
                added.append(f"{prefix}{nm} ({len(data)} B)")

    if extra_dir and os.path.isdir(extra_dir):
        walk(extra_dir, root, "/")
    return added


# ------------------------------------------------------------------ main

def main():
    argv = sys.argv[1:]
    opts = {"--startup": "amiga/boot/startup-sequence",
            "--device": "amiga/mpdisk/mpdisk.device",
            "--mount-tool": "amiga/mpmount/mpmount",
            "--volid": "MPDISK", "--cdisc": None, "--cdir": None, "--iso": None,
            "--tm": None, "--boot": "trademark", "--extract-cache": None,
            "--extra": "amiga/boot/cd-extra"}
    flags = {"--bootblock"}
    flagset = set()
    pos = []
    i = 0
    while i < len(argv):
        a = argv[i]
        if a in opts:
            opts[a] = argv[i + 1]; i += 2
        elif a in flags:
            flagset.add(a); i += 1
        elif a.startswith("--"):
            sys.exit(f"unknown option: {a}")
        else:
            pos.append(a); i += 1
    if "--bootblock" in flagset:
        opts["--boot"] = "bootblock"

    # --- CACHE EXTRACTION mode: dump the Commodore binaries (C: + CDTV.TM) from
    #     the genuine disc into a local dir ONCE; later builds are standalone
    #     (--cdir <cache>/C --tm <cache>/CDTV.TM). Keep the cache out of git (copyright).
    if opts["--extract-cache"]:
        if not opts["--cdisc"]:
            sys.exit("--extract-cache requires --cdisc <genuine disc.bin>")
        cache = opts["--extract-cache"]
        cmds = _extract_c_from_bin(opts["--cdisc"])
        missing = C_REQUIRED - (set(cmds) - {"@TM"})
        if missing:
            sys.exit(f"C: commands missing on the disc: {sorted(missing)}")
        os.makedirs(os.path.join(cache, "C"), exist_ok=True)
        n = 0
        for name, data in cmds.items():
            if name == "@TM":
                with open(os.path.join(cache, "CDTV.TM"), "wb") as f:
                    f.write(data)
            else:
                with open(os.path.join(cache, "C", name), "wb") as f:
                    f.write(data)
                n += 1
        tm = "CDTV.TM" if "@TM" in cmds else "(absent)"
        print(f"cache extracted to {cache}/: {n} C: commands + trademark {tm}")
        return 0

    if len(pos) != 1:
        print(__doc__); return 2
    out_base = pos[0]

    if not opts["--cdisc"] and not opts["--cdir"]:
        sys.exit("need --cdisc <genuine disc.bin> or --cdir <C: commands dir>")

    # C: commands
    if opts["--cdisc"]:
        cmds = _extract_c_from_bin(opts["--cdisc"])
    else:
        cmds = _extract_c_from_dir(opts["--cdir"])
    missing = C_REQUIRED - set(cmds)
    if missing:
        sys.exit(f"C: commands missing: {sorted(missing)}")

    # our own files
    with open(opts["--startup"], "rb") as f:
        startup = f.read()
    with open(opts["--device"], "rb") as f:
        device = f.read()
    # mpmount: mounts MPD0..MPD3 at runtime asking the firmware for the geometry
    # (self-describing) -> a .hdf can change size without rebuilding the CD. No
    # static Mountlist on the CD (it would be tied to one fixed size).
    with open(opts["--mount-tool"], "rb") as f:
        mount_tool = f.read()

    # trademark (Route B): from --tm <file> or extracted from --cdisc (CDTV.TM)
    mode = opts["--boot"]
    tm_data = None
    if mode == "trademark":
        if opts["--tm"]:
            with open(opts["--tm"], "rb") as f:
                tm_data = f.read()
        elif "@TM" in cmds:
            tm_data = cmds.pop("@TM")
        else:
            sys.exit("Route B (trademark): CDTV.TM needed -- use --cdisc (which holds it) "
                     "or --tm <file>. For a CD without trademark use --bootblock (Route A).")
    else:
        cmds.pop("@TM", None)

    # tree
    root = Node("", True)
    cdir = root.add(Node("C", True))
    for name in sorted(cmds):
        cdir.add(Node(name, False, cmds[name]))
    cdir.add(Node("MPMOUNT", False, mount_tool))   # our dynamic mount tool
    sdir = root.add(Node("S", True))
    sdir.add(Node("STARTUP-SEQUENCE", False, startup))
    ddir = root.add(Node("DEVS", True))
    ddir.add(Node("MPDISK.DEVICE", False, device))
    tm_node = None
    if tm_data is not None:
        tm_node = root.add(Node("CDTV.TM", False, tm_data))

    # free overlay: merge everything in --extra (default cd-extra)
    extra_added = merge_overlay(root, opts["--extra"])

    iso = build_iso(root, opts["--volid"],
                    bootblock=(mode == "bootblock"), tm_node=tm_node)
    if mode == "trademark":
        print(f"ISO9660: {len(iso)} B = {len(iso)//USR} sectors · trademark CDTV.TM "
              f"({len(tm_data)} B) @LBA{tm_node.lba}, 'TM' tag in the PVD")
    else:
        print(f"ISO9660: {len(iso)} B = {len(iso)//USR} sectors · bootblock DOS\\0 @LBA0")
    print(f"  /C: {', '.join(sorted(cmds))}")
    print(f"  /S/STARTUP-SEQUENCE ({len(startup)} B)")
    print(f"  /DEVS/MPDISK.DEVICE ({len(device)} B)")
    print(f"  /C/MPMOUNT ({len(mount_tool)} B)")
    if extra_added:
        print(f"  overlay ({opts['--extra']}): {', '.join(extra_added)}")

    if opts["--iso"]:
        with open(opts["--iso"], "wb") as f:
            f.write(iso)
        print(f"wrote {opts['--iso']} (for checks/WinUAE)")

    # ISO -> BIN/CUE MODE1/2352 (reuses the validated EDC/ECC engine)
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    import iso2bincue as ib
    bin_path = out_base + ".bin"
    cue_path = out_base + ".cue"
    total = len(iso) // USR
    with open(bin_path, "wb") as fout:
        for off in range(0, len(iso), ib.BATCH * USR):
            chunk = iso[off:off + ib.BATCH * USR]
            raw = ib.build_batch(chunk, off // USR)
            fout.write(raw.tobytes())
    with open(cue_path, "w", newline="\r\n") as f:
        f.write(f'FILE "{os.path.basename(bin_path)}" BINARY\n')
        f.write("  TRACK 01 MODE1/2352\n")
        f.write("    INDEX 01 00:00:00\n")
    mm, ss = divmod(total // 75, 60)
    print(f"OK: {bin_path} ({total} sectors, {mm:02d}:{ss:02d}) + {cue_path}")
    print("copy .bin+.cue into the CD folder on the SD card (like a game).")
    return 0


if __name__ == "__main__":
    sys.exit(main())
