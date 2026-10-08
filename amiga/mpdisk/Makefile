# =============================================================================
# MatshitaPico -- Solid-state CD-ROM drive replacement for the Commodore CDTV
# Copyright (c) 2026 Nicola Avanzi (na103)
# Licensed under Creative Commons Attribution 4.0 International (CC BY 4.0).
# See LICENSE.
# =============================================================================
# Makefile -- mpdisk.device (CDTV, Kickstart 1.3) with m68k-amigaos-gcc (Bebbo)
#
# Exec device loadable from DEVS:. "Bare" link (no C startup): the romtag and
# the vectors are provided by device.c.
# =============================================================================
CC      = m68k-amigaos-gcc
# -m68000 = CDTV CPU; -noixemul = no UNIX C library
CFLAGS  = -Os -m68000 -noixemul -fomit-frame-pointer -Wall -Wno-pointer-sign \
          -ffreestanding -fno-builtin $(EXTRA_CFLAGS)
# bare link: no C startup; -lgcc for the 68000 32-bit mul/div helpers
LDFLAGS = -nostdlib -nostartfiles
LIBS    = -lgcc

OBJ     = device.o pipe.o
TARGET  = mpdisk.device

# STUB for WinUAE testing: same device.c, backend that reads work.hdf as a file
# (pipe_stub.c) instead of the DMAC. -> mpdisk_stub.device (rename to DEVS:mpdisk.device).
STUB_OBJ = device_stub.o pipe_stub.o
STUB     = mpdisk_stub.device

all: $(TARGET)

$(TARGET): $(OBJ)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(OBJ) $(LIBS)

stub: $(STUB)

# device.c compiled for the stub (identical; separate object to avoid clashing with OBJ)
device_stub.o: device.c pipe.h mpdisk_dev.h
	$(CC) $(CFLAGS) -c -o $@ device.c

pipe_stub.o: pipe_stub.c pipe.h
	$(CC) $(CFLAGS) -c -o $@ pipe_stub.c

$(STUB): $(STUB_OBJ)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(STUB_OBJ) $(LIBS)

%.o: %.c pipe.h mpdisk_dev.h
	$(CC) $(CFLAGS) -c -o $@ $<

pipe.o: pipe.c pipe.h
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f $(OBJ) $(STUB_OBJ) $(TARGET) $(STUB)

.PHONY: all clean stub
