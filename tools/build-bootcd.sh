#!/usr/bin/env bash
# =============================================================================
# MatshitaPico -- Solid-state CD-ROM drive replacement for the Commodore CDTV
# Copyright (c) 2026 Nicola Avanzi (na103)
# Licensed under Creative Commons Attribution 4.0 International (CC BY 4.0).
# See LICENSE.
# =============================================================================
# build-bootcd.sh -- STANDALONE build of the mpdisk boot CD (no genuine disc
# needed after the first run).
#
# What it does:
#   1. rebuilds amiga/mpdisk/mpdisk.device and amiga/mpmount/mpmount (make)
#   2. if the Commodore binaries cache is missing, extracts it ONCE from the
#      genuine disc (C: MOUNT/ASSIGN/EXECUTE... + CDTV.TM) into tools/.bootcd-cache/
#   3. builds <out>.bin + <out>.cue from the cache + the repo's fresh
#      device/mpmount/startup-sequence + the amiga/boot/cd-extra overlay.
#
# The cache (tools/.bootcd-cache/) and the conf (tools/.bootcd.conf) are
# GIT-IGNORED: the Commodore binaries (copyright) never enter the repo.
# RMTM, LOADWB and NEWCLI are not on the genuine disc's /C: copy them by hand
# into tools/.bootcd-cache/C/ (RMTM from the CDTV Demo Disc root, LoadWB/NewCLI
# from Workbench 1.3). Personal use only.
#
# Requirements: m68k-amigaos-gcc (Bebbo) in PATH, python3 + numpy.
#
# Usage:
#   # first time (records the genuine disc in the conf and extracts the cache):
#   tools/build-bootcd.sh --genuine "/path/to/CDTV_genuine.bin"
#   # from then on (standalone: rebuilds the device + the CD):
#   tools/build-bootcd.sh
#   # options:
#   tools/build-bootcd.sh [--out <base_without_ext>] [--genuine <disc.bin>]
#                         [--refresh-cache]   # re-extract the cache from the disc
#
# Persistent config (tools/.bootcd.conf, written on the first run):
#   GENUINE=...   path of the genuine CDTV disc (MODE1/2352)
#   OUT=...       output base (default: build/mpdisk-bootcd under the repo)
# =============================================================================
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
CONF="$REPO/tools/.bootcd.conf"
CACHE="$REPO/tools/.bootcd-cache"

# --- defaults + persistent conf ---------------------------------------------
GENUINE=""
OUT="$REPO/build/mpdisk-bootcd"
# shellcheck disable=SC1090
[ -f "$CONF" ] && source "$CONF"

# --- CLI (overrides the conf) -----------------------------------------------
REFRESH=0
while [ $# -gt 0 ]; do
    case "$1" in
        --genuine) GENUINE="$2"; shift 2 ;;
        --out)     OUT="$2";     shift 2 ;;
        --refresh-cache) REFRESH=1; shift ;;
        -h|--help) sed -n '8,37p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) echo "unknown option: $1" >&2; exit 2 ;;
    esac
done

# --- 1. rebuild device + mount tool -----------------------------------------
echo ">> building mpdisk.device"
make -C "$REPO/amiga/mpdisk"
echo ">> building mpmount"
make -C "$REPO/amiga/mpmount"

# --- 2. Commodore binaries cache (one-shot) ---------------------------------
if [ "$REFRESH" = 1 ] || [ ! -f "$CACHE/CDTV.TM" ]; then
    if [ -z "$GENUINE" ]; then
        cat >&2 <<EOM
!! No cache and no known genuine disc.
   First run: pass the genuine CDTV disc (MODE1/2352 .bin):
     tools/build-bootcd.sh --genuine "/path/to/CDTV_genuine.bin"
   It is extracted ONCE into $CACHE/ (git-ignored) and remembered in $CONF.
EOM
        exit 1
    fi
    [ -f "$GENUINE" ] || { echo "!! genuine disc not found: $GENUINE" >&2; exit 1; }
    echo ">> extracting the Commodore cache from $(basename "$GENUINE")"
    python3 "$REPO/tools/mkbootcd.py" --extract-cache "$CACHE" --cdisc "$GENUINE"
fi

# --- save the conf (absolute paths, for the next standalone builds) ---------
mkdir -p "$(dirname "$CONF")"
{
    echo "# written by build-bootcd.sh -- paths for standalone builds"
    echo "GENUINE=\"${GENUINE:-}\""
    echo "OUT=\"$OUT\""
} > "$CONF"

# --- 3. build the CD from the cache (no genuine disc) -----------------------
mkdir -p "$(dirname "$OUT")"
echo ">> building the boot CD -> $OUT.bin/.cue"
python3 "$REPO/tools/mkbootcd.py" "$OUT" \
    --cdir "$CACHE/C" --tm "$CACHE/CDTV.TM" \
    --startup "$REPO/amiga/boot/startup-sequence" \
    --device "$REPO/amiga/mpdisk/mpdisk.device" \
    --mount-tool "$REPO/amiga/mpmount/mpmount" \
    --extra "$REPO/amiga/boot/cd-extra"

echo ">> done. Copy $OUT.bin + $OUT.cue into the CD folder (/games) of the SD card."
