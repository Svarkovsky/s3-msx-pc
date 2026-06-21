*Note: The most up-to-date version of the emulator is always available directly in the source code on the `main` branch. While these features and fixes are fully complete and tested, a new pre-compiled binary release (firmware dump) has not been published yet.*

## [Unreleased]

[-]

## [1.0.3] — 2026-06-21

1. DS-LZ v2.5a — Zero-Heap Compression Engine

   - Hash Table: Migrated from PSRAM (32 KB heap) to DRAM via VGA buffer
     (4 KB, 512 entries, zero allocations).
   - Hash Function: Replaced triple-op XOR with Knuth multiplicative hash
     (`val * 0x1E35A7BD >> 23`) for better distribution.
   - Signature Filter: Added 16-bit signature (`val * 0x85EBCA77 >> 16`)
     eliminating 99.99% false PSRAM reads.
   - Position Packing: Compressed 32-bit `pos` to `pos16 | sig16<<16` — single
     cache line per bucket, no PSRAM access on miss.
   - I/O Buffer: Consolidated 2×4096 B (8 KB DRAM) → 1×256 B unified buffer
     (saves 7.7 KB DRAM).
   - State Array: Reduced from 256 entries (1024 B) to 80 entries (320 B,
     saves 704 B).
   - Memory: Zero heap allocations, zero PSRAM for hash, ~0.6 KB total DRAM.
   - Compression: ~6% smaller output (152 KB vs 162 KB) due to improved match
     accuracy from signature filtering.
   - Stability: No OOM crashes (no dynamic allocations), VGA sync before
     buffer reuse.
   - Compatibility: Backward compatible V3 format.
   - Fix: Underflow bug in block_type==2 stride match (len=3 + offset>255).

2. GZip (.gz) Compression Support — Zero zlib Dependencies

   - Added: Transparent `.gz` decompression for ROMs, disk images, and cassette
     tapes without using the real zlib library.
   - Architecture: Implemented a lightweight zlib API adapter (`zlib.h` +
     `tinf_zlib.c`) using Joergen Ibsen's tinf inflate library (~2 KB code)
     under the zlib/libpng license.
   - Memory Strategy: All large buffers (compressed ~400 KB, uncompressed
     720 KB) are allocated exclusively in PSRAM via `heap_caps_malloc` with
     `MALLOC_CAP_SPIRAM`, preserving critical internal DRAM.
   - DRAM Optimization: Working structures (`tinf_data` ~1.3 KB, Huffman
     tables ~240 bytes, CRC32 lookup table ~1 KB) are placed in the existing
     VGA render buffer (`sram_render_buffer[]`) at zero additional memory cost.
   - Speed Optimizations:
     * Huffman tables cached in fast DRAM instead of Flash (~1.5× speedup)
     * CRC32 lookup table in DRAM (~20× faster than bit-by-bit calculation)
     * `memcpy` for non-overlapping LZ77 matches (~3× copy speedup)
     * `IRAM_ATTR` on the main inflate function (~1.3× speedup, +1.5 KB IRAM)
     * Hardware Watchdog resets during SD read and decompression
   - Launcher Integration: Added `gz` extension filter; dual-extension names
     (`.dsk.gz`, `.cas.gz`) are displayed cleanly by stripping both suffixes.

   Compression Efficiency (720 KB MSX disk image):

   | gzip level | Compressed size | Ratio | Decompression time |
   |------------|-----------------|-------|--------------------|
   | 1 (fast)   | ~300 KB         | 58%   | ~0.7 s             |
   | 6 (default)| ~250 KB         | 65%   | ~0.9 s             |
   | 9 (best)   | ~230 KB         | 68%   | ~1.1 s             |

   *Measured on ESP32-S3 @ 240 MHz, PSRAM @ 80 MHz QSPI.*

3. Cassette (.cas) Tape Support Restored

   - Added: Full `.cas` file support via `-tape` command-line argument.
   - Detection: `strstr()` based extension matching for `.cas` and `.cas.gz`.
   - Launcher: Added `cas` and `gz` to the MSX file extension filter.
   - Dual-extension support: `.cas.gz` files are decompressed transparently
     and loaded as standard cassette images.
     
     
## [1.0.2] — 2026-06-18

1. STLZ (Striped Delta-Stride LZ) Image Codec

  - PNG Encoder Replacement: Replaced the resource-intensive LodePNG encoder
    with the lightweight STLZ codec for save-state screenshots, completely
    eliminating the high peak-memory heap allocation (~500 KB to 1 MB) during
    emulation.
  - Compression Memory Reduction: Optimized the compression memory footprint by
    reducing the hash table size (STLZ_HASH_SIZE) from 4096 to 1024 entries.
    This reduced heap allocation from 32 KB to 8 KB without affecting the
    compression ratio on localized 4 KB stripe blocks.
  - Separate Compilation Unit: Decoupled the codec codebase into ds-stlz.h
    (declarations) and ds-stlz.c (definitions) to prevent unnecessary full
    project rebuilds during minor adjustments.

2. Low-Memory GUI Preview & Rendering

  - Stack Overflow Prevention: Refactored rg_gui_draw_stlz_image to allocate its
    temporary line and stripe buffers on the heap via malloc/free instead of the
    task stack, avoiding FreeRTOS stack overflows on S3-MSX-PC.
  - Zero-Heap Upscaling: Implemented rg_gui_draw_image_fit, a lightweight,
    line-by-line Nearest-Neighbor renderer. It handles scaling and optional
    Byte-Swapping (Big-Endian to Little-Endian) on the fly, bypassing the heavy
    rg_surface_convert allocation (~530 KB) for legacy PNG screenshots.
  - GCC 12 Address Warnings: Fixed compilation errors triggered by
    -Werror=address by replacing static pointer evaluations (e.g.,
    slot->preview) with string occupancy checks (slot->preview[0]).
  - Unified Fallbacks: Integrated conditional checks into slot_select_cb and
    gui_load_preview to automatically handle fast STLZ decoding or legacy PNG
    line-by-line fitting.

3. POSIX I/O Migration for State.h

  - Standard I/O Bypassed: Migrated the fMSX SaveState/LoadState serialization
    framework from standard library FILE* streams to direct POSIX calls (open,
    read, write, close).
  - DRAM Preservation: Eliminated dynamic newlib stdio buffer allocations,
    preserving ~4 KB of highly critical internal DRAM (MALLOC_CAP_INTERNAL)
    previously allocated dynamically by newlib buffering, which is crucial for
    S3-MSX-PC's tight memory limits.
  - Sector-Aligned Speed: Preserved fast disk access speeds by routing POSIX
    calls directly through the existing 4 KB static arrays (out_buf and in_buf),
    aligning operations with FATFS block structures.

4. Memory & Emulation Stability Fixes

  - Floppy Swap Peak Optimization: Refactored ChangeDisk in MSX.c to eject the
    old floppy disk (EjectFDI) and free its 720 KB RAM buffer before allocating
    memory for the newly requested disk (LoadFDI). This capped the peak memory
    footprint during floppy swaps to exactly 720 KB instead of 1.44 MB,
    resolving PSRAM fragmentation failures.
  - Save State Desynchronization Fix: Corrected a hidden data corruption
    vulnerability in State.h's SkipSTRUCT macro. Replaced the direct lseek call
    with sequential buf_getc loops to ensure the 4 KB read cache (in_buf)
    remains perfectly synchronized with the active stream pointer.
  - Dual-Mapper Integration (Slot 3-3): Configured an additional expandable
    memory mapper cartridge in Slot 3-3 at 128 KB (8 pages) to work alongside
    the primary 4 MB mapper (Slot 3-2). 
    
    *   **Optimal Memory Partitioning**: This 128 KB secondary mapper serves as
        the ideal system workspace, fully absorbed by the Nextor/MSX-DOS 2
        kernel for its internal buffers and command interpreter. Consequently,
        the primary 4 MB mapper remains completely untouched, leaving it fully
        dedicated to the user for executing heavy applications and configuring
        a large RAM-disk (up to ~3.5 MB) with maximum stability.

## [1.0.1] — 2026-06-14

1. Autofire & Input

  - Added: Autofire option in launcher (toggle in Options menu)
  - Warning: some games may freeze on boot with Autofire enabled; disable if needed

2. DS-LZ Compression Engine

  - Added: DS-LZ compression engine by Ivan Svarkovsky (CC BY-NC-SA 4.0)
  - Added: Standalone library — ds-lz.h (https://github.com/Svarkovsky/dslz)
  - Changed: Buffered I/O (4KB) — 2× faster save/load on SD card
  - Changed: 32-bit match extension — faster compression
  - Changed: Internal functions renamed to `buf_*` prefix for ESP-IDF compatibility

3. Audio & Build Fixes

  - Fixed: `fast_putc` macro conflict with ESP-IDF `<stdio.h>`
  - Fixed: Audio desynchronization and lag upon loading save states by flushing emulated sound buffers

## [1.0.0] — 2026-06-10

1. Initial Release

  - Added: Deeply optimized bare-metal MSX2+ emulator for ESP32-S3
