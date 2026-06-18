
*Note: The most up-to-date version of the emulator is always available directly in the source code on the `main` branch. While these features and fixes are fully complete and tested, a new pre-compiled binary release (firmware dump) has not been published yet.*

## [1.0.1] — 2026-06-14

### Added
- Autofire option in launcher (toggle in Options menu)
- Warning: some games may freeze on boot with Autofire enabled; disable if needed
- DS-LZ compression engine by Ivan Svarkovsky (CC BY-NC-SA 4.0)
- Standalone library: [ds-lz.h](https://github.com/Svarkovsky/dslz)

### Changed
- DS-LZ: buffered I/O (4KB) — 2× faster save/load on SD card
- DS-LZ: 32-bit match extension — faster compression
- Internal functions renamed to `buf_*` prefix for ESP-IDF compatibility

### Fixed
- `fast_putc` macro conflict with ESP-IDF `<stdio.h>`
- Audio desynchronization and lag upon loading save states by flushing emulated sound buffers

## [1.0.0] — 2026-06-10

### Added
- Initial commit: deeply optimized bare-metal MSX2+ emulator for ESP32-S3
