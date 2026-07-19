
# Optimized I2S PDM Stereo Audio Driver for ESP32-S3 (Retro-Go)

High-efficiency, low-overhead stereo PDM (Pulse Density Modulation) audio driver for the ESP32-S3 microcontroller within the Retro-Go framework. Specifically adapted to meet the demanding timing constraints of cycle-accurate retro emulation (fMSX MSX emulator).

**Author:** Ivan Svarkovsky, 2026
**Contact:** ivansvarkovsky@gmail.com

---

## Hardware Configuration & Pinout

The driver uses the ESP32-S3's hardware I2S peripheral in PDM TX mode with `I2S_SLOT_MODE_STEREO` (TWO_LINE_DAC mode) for native stereo output over two GPIO lines:

| GPIO | Function |
|------|----------|
| 9    | PDM CLK (Bit Clock) |
| 10   | DOUT (Right Channel) |
| 11   | DOUT2 (Left Channel) |

---

## Physical & Mathematical Principles of PDM

Instead of an external multi-bit I2S DAC chip, this driver leverages the dedicated delta-sigma hardware modulator inside the ESP32-S3 silicon.

### 1. High Oversampling Rate (OSR)

Standard 16-bit PCM audio frames are upsampled by the I2S hardware at a high Oversampling Rate (typically 64× or 128×) into a megahertz-level bitstream (e.g., 2.82 MHz or 5.64 MHz). This spreads quantization noise power over a much wider frequency band, significantly lowering its power density in the audible spectrum (20 Hz – 20 kHz).

### 2. Delta-Sigma Noise Shaping

The ESP32-S3's hardware delta-sigma modulator acts as a high-pass filter for quantization noise. It actively suppresses noise in the audible low-frequency band and shifts (shapes) the noise energy into the high-frequency/ultrasonic band.

```
Quantization Noise Spectrum
       ^
       |                  / (HF Noise shifted by Noise Shaping)
       |                 /
       |                /
       |               / 
       |--------------/
       | (Low audible noise)
       +-----------------------> Frequency
       0      20kHz         MHz
```

### 3. Reconstruction Filtering

Because noise has been pushed into the high-frequency band, a simple external passive low-pass RC reconstruction filter (e.g., 270 Ω resistor in series with 33 nF capacitor to ground) on GPIO 10 and GPIO 11 is sufficient to attenuate high-frequency modulation noise. This recovers a clean, high-fidelity analog wave with near-zero CPU overhead.

---

## Technical Software Optimizations

### 1. Heap DMA Buffer Allocation vs. Stack VLA

**Problem:** Variable-length arrays (VLAs) on the task stack (`int16_t stereo_buf[count * 2]`) consume ~2.5 KB at 640 samples. FreeRTOS audio/emulation tasks typically have only 3–4 KB of stack, making VLA allocation prone to silent stack overflows and random system panics.

**Solution:** The temporary stereo buffer is pre-allocated once during `driver_init()` using `heap_caps_malloc` with `MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL` flags.

**Impact:** Eliminates stack overflow risks, ensures memory safety across FreeRTOS tasks, and places the buffer in fast on-chip SRAM instead of slow external PSRAM, maximizing DMA transfer bus throughput.

### 2. Loop Flattening for Auto-Vectorization (SIMD)

**Problem:** Looping over a 2D struct (`frames[i].left` / `frames[i].right`) forces the compiler to calculate memory addresses with offsets for every element independently, preventing efficient pipelining.

**Solution:** Since `rg_audio_frame_t` is contiguous in memory, pointers are cast to a flat 1D array of `const int16_t *`. The loop processes `count * 2` samples sequentially, decorated with `__restrict__` and compiled with `__attribute__((optimize("O3")))`.

**Impact:** Guarantees the GCC compiler that source and destination buffers do not overlap, enabling auto-vectorization. The Xtensa LX7 CPU can execute 32-bit parallel loads (L32I), processing two 16-bit audio samples simultaneously within its vector registers, effectively halving memory-access overhead.

### 3. Removal of Redundant Clamping

**Problem:** Conventional drivers apply conditional branches inside the hot-path audio rendering loop to limit samples within `[-32768, 32767]`.

**Solution:** Inner-loop clipping checks have been completely removed.

**Impact:** The emulation sound engine (`Sound.c` / `PlayAudio`) already performs rigorous hardware clamping of the mixed audio signal prior to driver submission. Since the scaling multiplier `vol_mul` is strictly clamped to ≤ 256 (100%), the scaled output mathematically cannot exceed `int16_t` bounds. Removing these branches avoids CPU pipeline stalls caused by branch mispredictions in the tight loop.

### 4. Division-Free Fixed-Point Volume Scaling

**Problem:** Standard integer volume scaling requires dividing by 100: `(vol * 256) / 100`. The Xtensa LX7 core lacks a fast hardware integer division instruction.

**Solution:** Division is replaced by a fast fixed-point approximation performed once outside the inner loop:

```
vol_mul = (state.vol × 2622) >> 10
```

**Impact:** This fast approximation maps a volume level of 100 to a multiplier of exactly 256 with negligible calculation error (<0.05%), executing in a single CPU cycle.

---

## References

1. **Noise Shaping Principles** — MDPI Sensors Journal: *High-Performance Delta-Sigma Modulators and PDM Systems*. MDPI Sensors 2021, 21(10), 3470.

2. **Pulse-Density Modulation Theory** — Grokipedia: *Theoretical Foundations of Oversampling & PDM Quantization Noise*.

3. **Espressif I2S Hardware Documentation** — *ESP32-S3 Technical Reference Manual & ESP-IDF I2S Peripheral Guides*. [docs.espressif.com](https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/api-reference/peripherals/i2s.html)

---

## License

This driver is distributed under the same terms as the Retro-Go project. Commercial distribution is prohibited without permission from the original authors.

