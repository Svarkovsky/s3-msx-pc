# Changelog

> **Note:** The most up-to-date version of the emulator is always available
> directly in the source code on the `main` branch. While these features and
> fixes are fully complete and tested, a new pre-compiled binary release
> (firmware dump) has not been published yet.

## [1.0.2] — 2026-06-18

### Added

- **Dual-Mapper Configuration (Slot 3-3):** A secondary expandable memory mapper
  cartridge (128 KB / 8 pages) now operates alongside the primary 4 MB mapper
  (Slot 3-2). The 128 KB workspace is fully absorbed by the Nextor/MSX-DOS 2
  kernel for its internal buffers and command interpreter, leaving the primary
  4 MB mapper completely untouched and fully dedicated to heavy user
  applications and large RAM-disk setup (up to ~3.5 MB) with maximum stability.

### Changed

- **STLZ Image Codec — PNG Encoder Replacement:** Replaced the resource-intensive
  LodePNG encoder with the lightweight STLZ codec for save-state screenshots,
  completely eliminating high peak-memory heap allocations (~500 KB to 1 MB)
  during emulation.
- **STLZ — Compression Memory Reduction:** Reduced the hash table size
  (`STLZ_HASH_SIZE`) from 4096 to 1024 entries, shrinking heap allocation from
  32 KB to 8 KB with no impact on compression ratio for localized 4 KB stripe
  blocks.
- **STLZ — Separate Compilation Unit:** Decoupled the codec codebase into
  `ds-stlz.h` (declarations) and `ds-stlz.c` (definitions) to prevent
  unnecessary full project rebuilds during minor adjustments.
- **Low-Memory GUI — Zero-Heap Upscaling:** Implemented `rg_gui_draw_image_fit`,
  a lightweight line-by-line Nearest-Neighbor renderer. Scaling and optional
  Byte-Swapping (Big-Endian to Little-Endian) now occur on the fly, bypassing
  the heavy `rg_surface_convert` allocation (~530 KB) for legacy PNG
  screenshots.
- **Low-Memory GUI — Unified Fallbacks:** Integrated conditional checks into
  `slot_select_cb` and `gui_load_preview` to automatically handle fast STLZ
  decoding or legacy PNG line-by-line fitting.
- **POSIX I/O Migration for State.h:** Migrated the fMSX SaveState/LoadState
  serialization framework from standard library `FILE*` streams to direct POSIX
  calls (`open`, `read`, `write`, `close`), preserving fast disk access speeds
  by routing through the existing 4 KB static arrays (`out_buf` and `in_buf`)
  aligned to FATFS block structures.

### Fixed

- **Low-Memory GUI — Stack Overflow Prevention:** Refactored
  `rg_gui_draw_stlz_image` to allocate temporary line and stripe buffers on the
  heap via `malloc`/`free` instead of the task stack, avoiding FreeRTOS stack
  overflows on S3-MSX-PC.
- **GCC 12 Address Warnings:** Fixed compilation errors triggered by
  `-Werror=address` by replacing static pointer evaluations (e.g.,
  `slot->preview`) with string occupancy checks (`slot->preview[0]`).
- **POSIX I/O — DRAM Preservation:** Eliminated dynamic `newlib` stdio buffer
  allocations, preserving ~4 KB of highly critical internal DRAM
  (`MALLOC_CAP_INTERNAL`) previously consumed by newlib buffering — crucial for
  S3-MSX-PC's tight memory limits.
- **Floppy Swap Peak Optimization:** Refactored `ChangeDisk` in `MSX.c` to eject
  the old floppy disk (`EjectFDI`) and free its 720 KB RAM buffer *before*
  allocating memory for the newly requested disk (`LoadFDI`). This caps the
  peak memory footprint during floppy swaps to exactly 720 KB instead of
  1.44 MB, resolving PSRAM fragmentation failures.
- **Save State Desynchronization Fix:** Corrected a hidden data corruption
  vulnerability in `State.h`'s `SkipSTRUCT` macro by replacing a direct
  `lseek` call with sequential `buf_getc` loops, ensuring the 4 KB read cache
  (`in_buf`) remains perfectly synchronized with the active stream pointer.

## [1.0.1] — 2026-06-14

### Added

- Autofire option in launcher (toggle in Options menu).  
  *Warning: some games may freeze on boot with Autofire enabled; disable if needed.*
- DS-LZ compression engine by Ivan Svarkovsky (CC BY-NC-SA 4.0).  
  Standalone library: [ds-lz.h](https://github.com/Svarkovsky/dslz)

### Changed

- DS-LZ: buffered I/O (4 KB) — 2× faster save/load on SD card.
- DS-LZ: 32-bit match extension — faster compression.
- Internal functions renamed to `buf_*` prefix for ESP-IDF compatibility.

### Fixed

- `fast_putc` macro conflict with ESP-IDF `<stdio.h>`.
- Audio desynchronization and lag upon loading save states by flushing emulated
  sound buffers.

## [1.0.0] — 2026-06-10

### Added

- Initial commit: deeply optimized bare-metal MSX2+ emulator for ESP32-S3.
