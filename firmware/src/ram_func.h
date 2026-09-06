// =============================================================================
// MatshitaPico -- Solid-state CD-ROM drive replacement for the Commodore CDTV
// Copyright (c) 2026 Nicola Avanzi (na103)
// Licensed under Creative Commons Attribution 4.0 International (CC BY 4.0).
// See LICENSE.
// =============================================================================
// ram_func.h -- relocate critical-path code into RAM.
// Functions on the synchronous audio path (audio::service -> refill -> read_one
// -> cd_image::read_raw* -> storage::read_*_at) must run in DETERMINISTIC time:
// if one runs from flash and hits a cold XIP cache line it stalls on the fetch,
// the main loop blocks longer, and the CDTV host can lose a command byte ->
// timeout/reset. Marking them RAM_FUNC removes XIP access from their path.
// NB: f_read and the SD-PIO driver (FatFs, submodule) stay in flash -> NOT
// covered. On device (ARM) this uses the Pico SDK macro (__not_in_flash_func);
// in PC tests (x86, no SDK) it is a no-op, so shared sources compile identically.
// =============================================================================
#pragma once
#if defined(__ARM_ARCH)
#include "pico.h"                 // __not_in_flash_func (via pico/platform)
#endif

#ifndef __not_in_flash_func
#define __not_in_flash_func(func) func
#endif

#define RAM_FUNC(func) __not_in_flash_func(func)
