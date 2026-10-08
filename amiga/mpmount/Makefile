# =============================================================================
# MatshitaPico -- Solid-state CD-ROM drive replacement for the Commodore CDTV
# Copyright (c) 2026 Nicola Avanzi (na103)
# Licensed under Creative Commons Attribution 4.0 International (CC BY 4.0).
# See LICENSE.
# =============================================================================
# Makefile -- mpmount: dynamic mount of MPD0..MPD3 (CDTV, Kickstart 1.3)
#
# AmigaDOS executable with a MINIMAL ENTRY (-nostartfiles + our own `_start`):
# Bebbo's standard crt0 auto-opens utility.library, ABSENT on KS 1.3 -> crash
# before main on the CDTV. Our _start opens only dos.library. -lamiga provides
# CreateExtIO/CreatePort/DeleteExtIO/DeletePort. Goes into C: of the boot CD,
# run by the startup-sequence.
# =============================================================================
CC     = m68k-amigaos-gcc
CFLAGS = -Os -m68000 -noixemul -fomit-frame-pointer -Wall -Wno-pointer-sign
TARGET = mpmount

all: $(TARGET)

$(TARGET): crt0.S mpmount.c
	$(CC) $(CFLAGS) -nostartfiles -o $@ crt0.S mpmount.c -lamiga

clean:
	rm -f $(TARGET)

.PHONY: all clean
