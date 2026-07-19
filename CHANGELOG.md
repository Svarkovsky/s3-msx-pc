

## [1.0.5] — 2026-07-19

### System & Memory Optimization
- **Asymmetric Build Architecture:** Re-engineered the Retro-Go component build scripts. The `fmsx` emulator app is now built strictly offline, completely stripping out the heavy Wi-Fi, HTTP, and TCP/IP (LwIP) network stacks while keeping them active in the launcher.
- **Memory Recovery:** Reclaiming space from the network stack freed up **~141 KB of static PSRAM (.bss)** and **~115 KB of executable code (.text)** in the emulator, reducing overall external memory footprint.
- **Optimized Partition Table:** Redesigned the 16MB flash layout with contiguous, 64KB-aligned boundaries. This allowed expanding the user `storage` (FATFS) partition by **+192 KB** to maximize physical flash utilization.

### Audio
- Added **YM2151** (OPM/SFG-05) chip emulation (lazy init, 16 kHz with linear interpolation)
- Ported and updated to a newer version of emu2212 (SCC/SCC+)
- Major refactoring and optimization of emu2413 (OPLL/YM2413)
- Optimized **I2S PDM stereo driver:** DMA buffer in internal SRAM, flat loop for GCC auto-vectorization, division-free fixed-point volume scaling, removed redundant clamping
- Disabled audio in launcher (no BGM, removed idle noise floor)

### Web

- **Web interface:** fixed file download (Content-Disposition header), added Download button, improved long filename display.
- **Screenshots:** STLZ screenshot support.
    
### STLZ Full Integration — lodepng Removed
- **Removed:** lodepng library completely eliminated from the project.
- **Replaced:** All built-in images (`background_msx.png`, `logo_msx.png`) converted to STLZ format via `tools/gen_images_stlz_all.py` converter.

### Screensaver
- **Added:** "Through the Universe" starfield screensaver in `screensaver.h`.
- **Effect:** 3D rotating starfield with perspective projection, variable star speeds (2-65), 5 size levels scaling smoothly on approach.
- **Settings:** Configurable in Options menu — On/Off and timeout (0-999 seconds, step 60s).

---

## [1.0.4] — 2026-07-03

### Several minor fixes

### Changed stereo panning to constant power: SCC and OPLL levels increased in both channels, better balance with PSG.

### Added SCC+ enable via 0xB000 (bit 7 check) for bare SCC+ cartridges in MSX-DOS2. Minimal change: no extra RAM allocation, no stability impact.

### tinf_zlib.c
- Optional CRC32 via `TINF_CRC32_ENABLE` define, small but important optimizations.

### VGA Pixel Clock Jitter Elimination
- **Fixed:** Visual "waves" or "shimmering" along the edges of the screen and raster boundaries.
- **Architecture:** Changed the target VGA pixel clock (`VGA_PCLK`) from the standard 25175000 Hz to 24000000 Hz.
- **Clock Source:** Hardcoded the LCD_CAM clock source to `LCD_CLK_SRC_PLL240M` in `esp_lcd_rgb_panel_config_t`.
- **Optimization:** This forces the ESP32-S3 to use a strict integer division (240 MHz / 10 = 24 MHz) instead of a fractional divider. This completely eliminates Phase-Locked Loop (PLL) jitter, resulting in a rock-solid, jitter-free analog video signal comparable to dedicated APLL implementations, while fully retaining the ultra-fast Zero-Copy LCD_CAM pipeline.

### Pixel-Perfect GUI Rendering (1:1 Scaling)
- **Fixed:** "Staircase" (aliasing) artifacts on the launcher menu background and gradient contours.
- **Assets:** Replaced the legacy low-resolution background array in `images.c` with a native 640x480 resolution asset.
- **Mechanism:** Retro-Go uses Nearest-Neighbor scaling for GUI elements. By matching the image resolution exactly to the display resolution (640x480), the software scaler is bypassed entirely, ensuring crisp, artifact-free 1:1 pixel rendering.
- **Memory Optimization:** The new image is heavily optimized (PNG-8 indexed color palette) to maintain a minimal footprint (~100 KB) in the Flash `.rodata` partition. During runtime, `lodepng` decompresses it directly into the 8MB Octal PSRAM via the OS allocator intercept (`CONFIG_SPIRAM_USE_MALLOC=y`), consuming zero internal DRAM.

---

## [1.0.3] — 2026-06-21

### DS-LZ v2.5a — Zero-Heap Compression Engine
- **Hash Table:** Migrated from PSRAM (32 KB heap) to DRAM via VGA buffer (4 KB, 512 entries, zero allocations).
- **Hash Function:** Replaced triple-op XOR with Knuth multiplicative hash (`val * 0x1E35A7BD >> 23`) for better distribution.
- **Signature Filter:** Added 16-bit signature (`val * 0x85EBCA77 >> 16`) eliminating 99.99% false PSRAM reads.
- **Position Packing:** Compressed 32-bit pos to `pos16 | sig16<<16` — single cache line per bucket, no PSRAM access on miss.
- **I/O Buffer:** Consolidated 2×4096 B (8 KB DRAM) → 1×256 B unified buffer (saves 7.7 KB DRAM).
- **State Array:** Reduced from 256 entries (1024 B) to 80 entries (320 B, saves 704 B).
- **Memory:** Zero heap allocations, zero PSRAM for hash, ~0.6 KB total DRAM.
- **Compression:** ~6% smaller output (152 KB vs 162 KB) due to improved match accuracy from signature filtering.
- **Stability:** No OOM crashes (no dynamic allocations), VGA sync before buffer reuse.
- **Compatibility:** Backward compatible V3 format.
- **Fix:** Underflow bug in `block_type==2` stride match (`len=3 + offset>255`).

### GZip (.gz) Compression Support — Zero zlib Dependencies
- **Added:** Transparent `.gz` decompression for ROMs, disk images, and cassette tapes without using the real zlib library.
- **Architecture:** Implemented a lightweight zlib API adapter (`zlib.h` + `tinf_zlib.c`) using Joergen Ibsen's tinf inflate library (~2 KB code) under the zlib/libpng license.
- **Memory Strategy:** All large buffers (compressed ~400 KB, uncompressed 720 KB) are allocated exclusively in PSRAM via `heap_caps_malloc` with `MALLOC_CAP_SPIRAM`, preserving critical internal DRAM.
- **DRAM Optimization:** Working structures (`tinf_data` ~1.3 KB, Huffman tables ~240 bytes, CRC32 lookup table ~1 KB) are placed in the existing VGA render buffer (`sram_render_buffer[]`) at zero additional memory cost.
- **Speed Optimizations:**
  - Huffman tables cached in fast DRAM instead of Flash (~1.5× speedup)
  - CRC32 lookup table in DRAM (~20× faster than bit-by-bit calculation)
  - `memcpy` for non-overlapping LZ77 matches (~3× copy speedup)
  - `IRAM_ATTR` on the main inflate function (~1.3× speedup, +1.5 KB IRAM)
  - Hardware Watchdog resets during SD read and decompression
- **Launcher Integration:** Added `gz` extension filter; dual-extension names (`.dsk.gz`, `.cas.gz`) are displayed cleanly by stripping both suffixes.

**Compression Efficiency (720 KB MSX disk image):**

| gzip level | Compressed size | Ratio | Decompression time |
|------------|-----------------|-------|--------------------|
| 1 (fast)   | ~300 KB         | 58%   | ~0.7 s             |
| 6 (default)| ~250 KB         | 65%   | ~0.9 s             |
| 9 (best)   | ~230 KB         | 68%   | ~1.1 s             |

*Measured on ESP32-S3 @ 240 MHz, PSRAM @ 80 MHz QSPI.*

### Cassette (.cas) Tape Support Restored
- **Added:** Full `.cas` file support via `-tape` command-line argument.
- **Detection:** `strstr()` based extension matching for `.cas` and `.cas.gz`.
- **Launcher:** Added `cas` and `gz` to the MSX file extension filter.
- **Dual-extension support:** `.cas.gz` files are decompressed transparently and loaded as standard cassette images.

---

## [1.0.2] — 2026-06-18

### STLZ (Striped Delta-Stride LZ) Image Codec
- **PNG Encoder Replacement:** Replaced the resource-intensive LodePNG encoder with the lightweight STLZ codec for save-state screenshots, completely eliminating the high peak-memory heap allocation (~500 KB to 1 MB) during emulation.
- **Compression Memory Reduction:** Optimized the compression memory footprint by reducing the hash table size (`STLZ_HASH_SIZE`) from 4096 to 1024 entries. This reduced heap allocation from 32 KB to 8 KB without affecting the compression ratio on localized 4 KB stripe blocks.
- **Separate Compilation Unit:** Decoupled the codec codebase into `ds-stlz.h` (declarations) and `ds-stlz.c` (definitions) to prevent unnecessary full project rebuilds during minor adjustments.

### Low-Memory GUI Preview & Rendering
- **Stack Overflow Prevention:** Refactored `rg_gui_draw_stlz_image` to allocate its temporary line and stripe buffers on the heap via `malloc`/`free` instead of the task stack, avoiding FreeRTOS stack overflows on S3-MSX-PC.
- **Zero-Heap Upscaling:** Implemented `rg_gui_draw_image_fit`, a lightweight, line-by-line Nearest-Neighbor renderer. It handles scaling and optional Byte-Swapping (Big-Endian to Little-Endian) on the fly, bypassing the heavy `rg_surface_convert` allocation (~530 KB) for legacy PNG screenshots.
- **GCC 12 Address Warnings:** Fixed compilation errors triggered by `-Werror=address` by replacing static pointer evaluations (e.g., `slot->preview`) with string occupancy checks (`slot->preview[0]`).
- **Unified Fallbacks:** Integrated conditional checks into `slot_select_cb` and `gui_load_preview` to automatically handle fast STLZ decoding or legacy PNG line-by-line fitting.

### POSIX I/O Migration for State.h
- **Standard I/O Bypassed:** Migrated the fMSX SaveState/LoadState serialization framework from standard library `FILE*` streams to direct POSIX calls (`open`, `read`, `write`, `close`).
- **DRAM Preservation:** Eliminated dynamic newlib stdio buffer allocations, preserving ~4 KB of highly critical internal DRAM (`MALLOC_CAP_INTERNAL`) previously allocated dynamically by newlib buffering, which is crucial for S3-MSX-PC's tight memory limits.
- **Sector-Aligned Speed:** Preserved fast disk access speeds by routing POSIX calls directly through the existing 4 KB static arrays (`out_buf` and `in_buf`), aligning operations with FATFS block structures.

### Memory & Emulation Stability Fixes
- **Floppy Swap Peak Optimization:** Refactored `ChangeDisk` in `MSX.c` to eject the old floppy disk (`EjectFDI`) and free its 720 KB RAM buffer before allocating memory for the newly requested disk (`LoadFDI`). This capped the peak memory footprint during floppy swaps to exactly 720 KB instead of 1.44 MB, resolving PSRAM fragmentation failures.
- **Save State Desynchronization Fix:** Corrected a hidden data corruption vulnerability in `State.h`'s `SkipSTRUCT` macro. Replaced the direct `lseek` call with sequential `buf_getc` loops to ensure the 4 KB read cache (`in_buf`) remains perfectly synchronized with the active stream pointer.
- **Dual-Mapper Integration (Slot 3-3):** Configured an additional expandable memory mapper cartridge in Slot 3-3 at 128 KB (8 pages) to work alongside the primary 4 MB mapper (Slot 3-2).
  - **Optimal Memory Partitioning:** This 128 KB secondary mapper serves as the ideal system workspace, fully absorbed by the Nextor/MSX-DOS 2 kernel for its internal buffers and command interpreter. Consequently, the primary 4 MB mapper remains completely untouched, leaving it fully dedicated to the user for executing heavy applications and configuring a large RAM-disk (up to ~3.5 MB) with maximum stability.

---

## [1.0.1] — 2026-06-14

### Autofire & Input
- **Added:** Autofire option in launcher (toggle in Options menu).
- **Warning:** Some games may freeze on boot with Autofire enabled; disable if needed.

### DS-LZ Compression Engine
- **Added:** DS-LZ compression engine by Ivan Svarkovsky (CC BY-NC-SA 4.0).
- **Added:** Standalone library — `ds-lz.h` (https://github.com/Svarkovsky/dslz).
- **Changed:** Buffered I/O (4KB) — 2× faster save/load on SD card.
- **Changed:** 32-bit match extension — faster compression.
- **Changed:** Internal functions renamed to `buf_*` prefix for ESP-IDF compatibility.

### Audio & Build Fixes
- **Fixed:** `fast_putc` macro conflict with ESP-IDF `<stdio.h>`.
- **Fixed:** Audio desynchronization and lag upon loading save states by flushing emulated sound buffers.

---

## [1.0.0] — 2026-06-10

- **Initial Release**
- **Added:** Deeply optimized bare-metal MSX2+ emulator for ESP32-S3.
