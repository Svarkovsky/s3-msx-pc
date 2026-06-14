# MSX ESP32-S3 Optimization Notes

Technical deep-dives into embedded emulation optimization for the ESP32-S3 platform.

**Author:** Ivan Svarkovsky, 2026  
**Contact:** ivansvarkovsky@gmail.com  
**Architecture:** ESP32-S3 (Xtensa LX7, 240 MHz, dual-core)  
**Platform:** Retro-Go RTOS / fMSX  

## Table of Contents

- [1. The Emulation Paradox: Why MSX2+ Runs Faster Than MSX2](#1-the-emulation-paradox-why-msx2-runs-faster-than-msx2)
  - [1.1 Hardware Scrolling vs. Brute Force (V9958 vs. V9938)](#11-hardware-scrolling-vs-brute-force-v9958-vs-v9938)
  - [1.2 The Power of Custom Optimizations (SWAR in YJK Modes)](#12-the-power-of-custom-optimizations-swar-in-yjk-modes)
  - [1.3 The Architecture Beneath: Why This Works on ESP32-S3](#13-the-architecture-beneath-why-this-works-on-esp32-s3)
- [2. Zero-Copy Rendering: Why Direct-to-PSRAM Won](#2-zero-copy-rendering-why-direct-to-psram-won)
  - [2.1 The DRAM Buffer Misconception](#21-the-dram-buffer-misconception)
  - [2.2 PSRAM Sequential Writes vs. Random Reads](#22-psram-sequential-writes-vs-random-reads)
  - [2.3 The Final Zero-Copy Architecture](#23-the-final-zero-copy-architecture)
- [3. VDP Engine Optimizations (V9938.c)](#3-vdp-engine-optimizations-v9938c)
  - [3.1 IRAM Placement for Hot Paths](#31-iram-placement-for-hot-paths)
  - [3.2 Integer Dispatch: Eliminating Function Pointers](#32-integer-dispatch-eliminating-function-pointers)
  - [3.3 always_inline for Flash-Free Helpers](#33-always_inline-for-flash-free-helpers)
  - [3.4 Folded Logical Operations](#34-folded-logical-operations)
- [4. YJK SWAR Renderer (Common.h)](#4-yjk-swar-renderer-commonh)
  - [4.1 32-bit Aligned VRAM Fetches](#41-32-bit-aligned-vram-fetches)
  - [4.2 Branchless Bit Extraction](#42-branchless-bit-extraction)
  - [4.3 Precomputed Chroma Offset and LUT Clamping](#43-precomputed-chroma-offset-and-lut-clamping)
  - [4.4 Parallel Sprite Overlay](#44-parallel-sprite-overlay)
- [5. Screen 5 Dual-Pixel LUT](#5-screen-5-dual-pixel-lut)
- [6. Z80 Memory Access Optimizations (MSX.c)](#6-z80-memory-access-optimizations-msxc)
  - [6.1 Software Line Cache](#61-software-line-cache)
  - [6.2 Branch Prediction Hints](#62-branch-prediction-hints)
- [7. VGA Driver Architecture (vga.h)](#7-vga-driver-architecture-vgah)
  - [7.1 DRAM Render Buffer + Hardware Byte-Swap](#71-dram-render-buffer--hardware-byte-swap)
- [8. Dual Audio Engine (Sound.c)](#8-dual-audio-engine-soundc)
  - [8.1 SND_MODE_FAST: Branchless Meander (Marat Fayzullin)](#81-snd_mode_fast-branchless-meander-marat-fayzullin)
  - [8.2 SND_MODE_ACCURATE: Studio Chip Emulation (Mitsutaka Okazaki)](#82-snd_mode_accurate-studio-chip-emulation-mitsutaka-okazaki)
  - [8.3 PSG: AY-3-8910 with DC Blocker (emu2149.c)](#83-psg-ay-3-8910-with-dc-blocker-emu2149c)
  - [8.4 OPLL: YM2413 FM-PAC — The 400KB Table Elimination (emu2413.c)](#84-opll-ym2413-fm-pac--the-400kb-table-elimination-emu2413c)
  - [8.5 SCC: Konami Wavetable with LPF (emu2212.c)](#85-scc-konami-wavetable-with-lpf-emu2212c)
  - [8.6 Stereo Soundstage Design](#86-stereo-soundstage-design)
  - [8.7 SoftClip Limiter](#87-softclip-limiter)
- [9. Xtensa LX7 Audio Optimizations](#9-xtensa-lx7-audio-optimizations)
  - [9.1 IRAM_ATTR for Hot Paths](#91-iram_attr-for-hot-paths)
  - [9.2 DRAM_ATTR Memory Strategy](#92-dram_attr-memory-strategy)
  - [9.3 Branch Prediction Hints](#93-branch-prediction-hints)
  - [9.4 Bitwise Branchless Operations](#94-bitwise-branchless-operations)
- [10. Performance Metrics](#10-performance-metrics)
- [11. Z80 CPU Core Optimizations (Z80.c)](#11-z80-cpu-core-optimizations-z80c)
  - [11.1 Threaded Code: Computed Goto](#111-threaded-code-computed-goto)
  - [11.2 Register Caching (Level 1)](#112-register-caching-level-1)
  - [11.3 Smart Turbo Mode](#113-smart-turbo-mode)
  - [11.4 CodesCB Force-Inlining](#114-codescb-force-inlining)
  - [11.5 LDIR/LDDR Batch Optimization](#115-ldirlddr-batch-optimization)
  - [11.6 INDR/OTDR Bug Fix](#116-indrotdr-bug-fix)
  - [11.7 Table Placement in DRAM](#117-table-placement-in-dram)
  - [11.8 IRAM Placement Strategy](#118-iram-placement-strategy)
  - [11.9 Branch Prediction Architecture](#119-branch-prediction-architecture)
- [12. The Takeaway](#12-the-takeaway)
- [13. DS-LZ State Compression (State.h)](#13-ds-lz-state-compression-stateh)
  - [13.1 The Domain-Specific Advantage](#131-the-domain-specific-advantage)
  - [13.2 Architecture](#132-architecture)
  - [13.3 Performance](#133-performance)
  - [13.4 Why It Beats General-Purpose Compressors](#134-why-it-beats-general-purpose-compressors)
  - [13.5 Evolution](#135-evolution)

---

## 1. The Emulation Paradox: Why MSX2+ Runs Faster Than MSX2

During the deep optimization process of the ESP32-S3 MSX project, a counter-intuitive "Emulation Paradox" emerged:

**Why does emulating the advanced MSX2+ require fewer CPU cycles and run faster on the ESP32 than emulating the older, simpler MSX2?**

Logically, more complex hardware should be harder to emulate. However, core analysis logs revealed the opposite. There are two distinct technical reasons for this paradox.

### 1.1 Hardware Scrolling vs. Brute Force (V9958 vs. V9938)

The MSX2+ features an upgraded V9958 video chip with native horizontal scrolling. To scroll the screen, a game simply writes a couple of bytes to a register—meaning the CPU emulation overhead is practically 0%.

The older MSX2 (V9938) lacked native horizontal scrolling. To achieve a scrolling effect, game developers had to spam the VDP with hundreds of block-copy commands (HMMM and LMMM) every single frame. Emulating this constant, pixel-by-pixel memory shuffling puts a much heavier load on the ESP32 than simply shifting a screen pointer.

#### The V9938 Emulation Tax: Inside the HMMM Engine

The V9938's HMMM (High-speed Move VRAM to VRAM) command is the primary weapon for software scrolling. Here is the actual engine loop from the emulated VDP:

```c
IRAM_ATTR void HmmmEngine(void) {
    register int SX = MMC.SX;
    register int SY = MMC.SY;
    register int DX = MMC.DX;
    register int DY = MMC.DY;
    // ... state registers cached ...

    delta = MMC.delta;
    cnt = VdpOpsCnt;

    switch (ScrMode) {
        case 5:
            pre_loop *VDP_VRMP5(ADX, DY) = *VDP_VRMP5(ASX, SY);
            post_xxyy(256) break;
        case 6:
            pre_loop *VDP_VRMP6(ADX, DY) = *VDP_VRMP6(ASX, SY);
            post_xxyy(512) break;
        case 7:
            pre_loop *VDP_VRMP7(ADX, DY) = *VDP_VRMP7(ASX, SY);
            post_xxyy(512) break;
        case 8:
            pre_loop *VDP_VRMP8(ADX, DY) = *VDP_VRMP8(ASX, SY);
            post_xxyy(256) break;
    }
```

Each `pre_loop` iteration expands into:

```c
while ((cnt -= delta) > 0) {
    // One pixel transfer
    *VDP_VRMP5(ADX, DY) = *VDP_VRMP5(ASX, SY);

    // Coordinate advance with boundary checks
    if (!--ANX || ((ASX += TX) & MX) || ((ADX += TX) & MX)) {
        if (!(--NY & 1023) || (SY += TY) == -1 || (DY += TY) == -1)
            break;
        else {
            ASX = SX;
            ADX = DX;
            ANX = NX;
        }
    }
}
```

The `VDP_VRMP5` macro resolves to:

```c
#define VDP_VRMP5(X, Y) (VRAM + ((Y & 1023) << 7) + ((X & 255) >> 1))
```

This is a **random access pattern** into the emulated VRAM array. The source and destination addresses are computed independently per pixel, with no spatial locality guarantees. On the ESP32-S3, each access to PSRAM-resident VRAM risks a D-cache miss. The Octal SPI bus fetches 64-byte cache lines, but a single pixel read only consumes one byte—leaving the remaining 63 bytes of the fetched line likely unused if the next pixel jumps to a different tile.

The `post_xxyy` macro adds branch misprediction pressure. The Xtensa LX7 has a 3-stage pipeline with static branch prediction (backward taken, forward not taken). The irregular `NY & 1023` wraparound and the `== -1` terminal checks create unpredictable control flow, forcing pipeline flushes.

A single 16×16 tile scroll via HMMM requires 256 iterations of this loop. A full-screen scroll at 60 FPS requires hundreds of such commands. The CPU spends more time waiting for PSRAM random reads and pipeline refills than executing game logic.

#### The V9958 Alternative: Zero-Cost Scroll

The V9958 introduces `R#25` (horizontal scroll) and `R#26` (vertical scroll). The emulation path is a single register write:

```c
case 25:
    VDP[25] = V;
    SetScreen();  // Recomputes ChrTab/ColTab base pointers only
    break;
```

The `SetScreen()` function updates pointer arithmetic for the scanline renderer:

```c
ChrTab = VRAM + ((int)(VDP[2] & MSK[J].R2) << I);
```

No pixels move. No VRAM is touched. The scroll offset is applied during scanline rendering by adjusting the memory base pointer—a constant-time operation. The D-cache sees zero additional pressure. The CPU pipeline never breaks stride.

### 1.2 The Power of Custom Optimizations (SWAR in YJK Modes)

The MSX2+ introduced exclusive YJK graphics modes (Screen 10 and 12) to output high-color images. Historically, these modes are considered "heavy" and computationally intensive.

However, because these modes are highly specific, a custom, heavily optimized rendering engine was written for them. By introducing 32-bit aligned memory fetches and branchless bitwise math (SWAR — SIMD Within A Register), the slow pixel-by-pixel processing was bypassed. As a result, these theoretically heavy MSX2+ graphics modes are processed faster and much more efficiently on the ESP32-S3 architecture than the legacy 8-bit MSX2 modes.

#### YJK Decoding: The Naive Approach vs. SWAR

YJK encoding stores four horizontal pixels in four bytes. The layout per 4-pixel group:

```text
Byte 0: Y0[4:0] | J[2:0]
Byte 1: Y1[4:0] | J[5:3]
Byte 2: Y2[4:0] | K[2:0]
Byte 3: Y3[4:0] | K[5:3]
```

Where J and K are 6-bit signed chroma values shared across all four pixels. RGB conversion:

```c
// Naive per-pixel decoder
int R = Y + J;
int G = Y + K;
int B = (5*Y - 2*J - K) / 4;

// Clamp to [0, 31]
if (R > 31) R = 31; else if (R < 0) R = 0;
if (G > 31) G = 31; else if (G < 0) G = 0;
if (B > 31) B = 31; else if (B < 0) B = 0;
```

This requires **6 branches per pixel**, integer division, and irregular memory access for Y/J/K extraction. At 256 pixels per scanline, this is 1536 branch instructions per line—catastrophic for the Xtensa LX7's simple branch predictor.

#### The SWAR Renderer: `RefreshLine12`

The optimized implementation loads the entire 4-pixel group as a single 32-bit word:

```c
for (X = 1; X < 64; X++, T += 4, R += 4, P += 4) {
    // Aligned 32-bit fetch: 4 bytes → 1 L32I instruction
    uint32_t t_val = (uint32_t)T[0] | ((uint32_t)T[1] << 8)
                     | ((uint32_t)T[2] << 16) | ((uint32_t)T[3] << 24);

    // Parallel sprite buffer load
    uint32_t r_val = (uint32_t)R[0] | ((uint32_t)R[1] << 8)
                     | ((uint32_t)R[2] << 16) | ((uint32_t)R[3] << 24);
```

**J extraction via bitwise fusion:**

```c
    // J = {t_val[15:13], t_val[7:5]} — 6-bit signed
    int J = ((t_val >> 16) & 0x07) | ((t_val >> 21) & 0x38);
    // Sign-extend without branches: XOR 0x20, subtract 0x20
    J = (J ^ 0x20) - 0x20;
```

The `(J ^ 0x20) - 0x20` trick exploits two's complement arithmetic. For a 6-bit value `j`, `j ^ 0x20` flips the sign bit. Subtracting `0x20` then produces the correct sign-extended result:

| Input | Binary | `^ 0x20` | `- 0x20` | Result |
|-------|--------|----------|----------|--------|
| +31   | 011111 | 111111   | 011111   | +31    |
| 0     | 000000 | 010000   | 000000   | 0      |
| -1    | 111111 | 101111   | 111111   | -1     |
| -32   | 100000 | 000000   | 100000   | -32    |

**K extraction is identical:**

```c
    int K = (t_val & 0x07) | ((t_val >> 5) & 0x38);
    K = (K ^ 0x20) - 0x20;
```

**Precomputed chroma offset for B:**

```c
    int b_off = -(J << 1) - K;  // Computed once per 4-pixel group
```

**Y values extracted in parallel:**

```c
    byte y0 = (t_val >> 3) & 0x1F;
    byte y1 = (t_val >> 11) & 0x1F;
    byte y2 = (t_val >> 19) & 0x1F;
    byte y3 = (t_val >> 27) & 0x1F;
```

**The `YJKColor` function—LUT-based, branchless:**

```c
INLINE pixel YJKColor(int Y, int J, int K, int B_offset) {
    // clip_RG: 95-entry LUT covering Y+J+32 and Y+K+32 ranges
    int R = clip_RG[Y + J + 32];        // Single index, no comparison
    int G = clip_RG[Y + K + 32];        // Reuse same LUT
    // clip_B: 345-entry LUT for the (5Y - 2J - K)/4 expression
    int B = clip_B[(((Y << 2) + Y + B_offset) >> 2) + 93];
    // Pack into 5-6-5 RGB via bitwise OR
    return BPal[(R & 0x1C) | ((G & 0x1C) << 3) | (B >> 3)];
}
```

The `clip_RG` LUT is indexed by `Y + J + 32`, where `Y ∈ [0,31]` and `J ∈ [-32,31]`, giving a range of `[-32, 94]`—hence 95 entries. The first 32 entries are `0` (clamp to minimum), entries 32–63 are the identity mapping, and entries 64–94 are `31` (clamp to maximum). The entire clamp-and-saturate operation collapses to a single memory load.

**Sprite overlay—branchless predication:**

```c
    P[0] = (r_val & 0xFF) ? XPal[r_val & 0xFF] 
                          : YJKColor((t_val >> 3) & 0x1F, J, K, b_off);
    P[1] = ((r_val >> 8) & 0xFF) ? XPal[(r_val >> 8) & 0xFF]
                                 : YJKColor((t_val >> 11) & 0x1F, J, K, b_off);
    P[2] = ((r_val >> 16) & 0xFF) ? XPal[(r_val >> 16) & 0xFF]
                                  : YJKColor((t_val >> 19) & 0x1F, J, K, b_off);
    P[3] = ((r_val >> 24) & 0xFF) ? XPal[(r_val >> 24) & 0xFF]
                                  : YJKColor((t_val >> 27) & 0x1F, J, K, b_off);
```

The `(r_val & 0xFF) ? ... : ...` pattern compiles to conditional move instructions on Xtensa, not branches. The compiler generates predicated moves rather than branching execution paths. With `__builtin_expect` hints indicating that sprites are typically absent on most pixels, the branch predictor correctly assumes the YJK path. The common case is straight-line execution.

#### Legacy Screen 2: The Branch Tax

Contrast with `RefreshLine2`, the MSX2 workhorse:

```c
for (X = 0; X < 32; X++, T++, P += 8) {
    J = (int)*T << 3; 
    K = ColTab[(I + J) & ColTabM];            // DRAM lookup
    FC = XPal[K >> 4];                        // DRAM lookup
    BC = XPal[K & 0x0F];                      // DRAM lookup
    K = ChrGen[(I + J) & ChrGenM];            // DRAM lookup

    P[0] = K & 0x80 ? FC : BC;  // Branch 1
    P[1] = K & 0x40 ? FC : BC;  // Branch 2
    P[2] = K & 0x20 ? FC : BC;  // Branch 3
    P[3] = K & 0x10 ? FC : BC;  // Branch 4
    P[4] = K & 0x08 ? FC : BC;  // Branch 5
    P[5] = K & 0x04 ? FC : BC;  // Branch 6
    P[6] = K & 0x02 ? FC : BC;  // Branch 7
    P[7] = K & 0x01 ? FC : BC;  // Branch 8
}
```

Eight branches per tile, 32 tiles per line: **256 branch instructions per scanline**. The pattern `K & 0x80 ? FC : BC` is data-dependent—the bit pattern varies per tile, defeating static prediction. The Xtensa pipeline stalls on every mispredict, and the 8-cycle refill penalty multiplies across the line.

The YJK renderer processes 4 pixels with **identical control flow**—no per-pixel branches, no data-dependent prediction. The CPU executes in a straight-line burst, maximizing the 3-stage pipeline throughput.

### 1.3 The Architecture Beneath: Why This Works on ESP32-S3

| Component               | MSX2 (V9938)                                                | MSX2+ (V9958)                           |
| ----------------------- | ----------------------------------------------------------- | --------------------------------------- |
| **Scrolling**           | `HMMM`/`LMMM`: random R/W to VRAM (PSRAM) → cache thrashing | Register: 0 VRAM accesses               |
| **Scanline Rendering**  | Table lookups, pixel-by-pixel branches                      | SWAR batch processing, aligned loads    |
| **VRAM Access Pattern** | Random (reading tiles, sprites, commands)                   | Sequential (YJK — linear 4-byte groups) |
| **CPU Stall**           | High (PSRAM misses)                                         | Low (burst reads, register ops)         |

---

## 2. Zero-Copy Rendering: Why Direct-to-PSRAM Won

### 2.1 The DRAM Buffer Misconception

Early development assumed that since external PSRAM is slow, writing every single pixel directly to it during active rendering would be a total bottleneck. The plan was to allocate and keep a 116 KB intermediate video buffer (XBuf) inside the fast, internal DRAM, render everything to DRAM first, and then copy the finished frame to the display buffer in PSRAM.

This completely overlooked how the data cache (D-cache) operates inside the ESP32-S3.

### 2.2 PSRAM Sequential Writes vs. Random Reads

**PSRAM struggles with Random Reads (The Z80 bottleneck):** When the Z80 interpreter jumps around the codebase, reading single-byte opcodes from disjointed memory locations, it causes frequent cache misses. Every single miss forces the controller to fetch a full 64-byte cache line from PSRAM over the Octal SPI bus, stalling the CPU for up to 100–150 cycles. This is why running execution cores or random-access tables out of PSRAM chokes the system.

**PSRAM handles Sequential Writes exceptionally well (The VDP breakthrough):** When the VDP renders a scanline, it writes pixels sequentially (P[0], P[1], P[2]...). The ESP32-S3's D-cache intercepts these sequential writes and holds them in a fast internal buffer. Once it has a full 64-byte block, the cache controller flushes it to the PSRAM in a single, highly optimized background burst via the wide Octal SPI bus. The CPU doesn't have to wait for the write to finish; it just keeps running.

### 2.3 The Final Zero-Copy Architecture

Once this was understood, the intermediate 116 KB DRAM XBuf buffer was completely eliminated. Now, the fMSX core renders directly into the PSRAM framebuffer (`updates[0]` allocated in `MEM_SLOW`).

By trusting the D-cache to handle the sequential VDP writes, two massive wins were achieved:

1. **Freed up 116 KB of precious internal DRAM**, which is critical on the ESP32-S3.
2. **Completely eliminated a heavy CPU-side memcpy per frame** that was previously required to move data from the DRAM buffer to the display PSRAM.

Direct-to-PSRAM zero-copy rendering is not only feasible, but it is actually the most efficient way to handle video on this chip.

---

## 3. VDP Engine Optimizations (V9938.c)

### 3.1 IRAM Placement for Hot Paths

All hot-path functions are forced into IRAM via `IRAM_ATTR` to eliminate Flash cache misses during the 256 VDP command executions per frame and the per-scanline rendering loops:

```c
IRAM_ATTR void SrchEngine(void);
IRAM_ATTR void LineEngine(void);
IRAM_ATTR void LmmvEngine(void);
IRAM_ATTR void LmmmEngine(void);
IRAM_ATTR void HmmvEngine(void);
IRAM_ATTR void HmmmEngine(void);
IRAM_ATTR void YmmmEngine(void);
IRAM_ATTR void HmmcEngine(void);
IRAM_ATTR void VDPWrite(byte V);
IRAM_ATTR byte VDPRead(void);
IRAM_ATTR byte VDPDraw(byte Op);
IRAM_ATTR void LoopVDP(void);
```

### 3.2 Integer Dispatch: Eliminating Function Pointers

The original fMSX used a function pointer for VDP command dispatch:

```c
// Original design (Flash-resident function pointer)
static void (*VdpEngine)(void) = 0;
```

On Xtensa, an indirect call via function pointer requires:
1. Load the pointer from DRAM (potential cache miss).
2. `CALLX` instruction (indirect jump, unpredictable target).
3. Pipeline flush on misprediction.
4. If the target is in Flash: cache miss + 100+ cycle penalty.

The optimized replacement:

```c
static uint8_t ActiveVdpEngine = 0;  // 0 = none, CM_SRCH = 6, etc.

IRAM_ATTR static void RunActiveEngine(void) {
    switch (ActiveVdpEngine) {
        case CM_SRCH: SrchEngine(); break;   // 6
        case CM_LINE: LineEngine(); break;   // 7
        case CM_LMMV: LmmvEngine(); break;   // 8
        case CM_LMMM: LmmmEngine(); break;   // 9
        case CM_LMCM: LmcmEngine(); break;   // 10
        case CM_LMMC: LmmcEngine(); break;   // 11
        case CM_HMMV: HmmvEngine(); break;   // 12
        case CM_HMMM: HmmmEngine(); break;   // 13
        case CM_YMMM: YmmmEngine(); break;   // 14
        case CM_HMMC: HmmcEngine(); break;   // 15
    }
}
```

The `switch` on a small integer range compiles to a jump table or a sequence of `BEQ`/`BNE` branches with predictable fall-through. All targets are in IRAM, so no Flash misses occur.

### 3.3 always_inline for Flash-Free Helpers

The VDP pixel accessors are marked to prevent GCC from emitting them as separate Flash-resident functions:

```c
__attribute__((always_inline)) INLINE byte VDPpoint5(int SX, int SY) {
    return (*VDP_VRMP5(SX, SY) >> (((~SX) & 1) << 2)) & 15;
}

__attribute__((always_inline)) INLINE void VDPpset5(int DX, int DY, byte CL, byte OP) {
    register byte SH = ((~DX) & 1) << 2;
    VDPpsetlowlevel(VDP_VRMP5(DX, DY), CL << SH, ~(15 << SH), OP);
}
```

Without `always_inline`, an IRAM-resident engine calling a Flash-resident helper would trigger a Flash cache miss mid-engine—defeating the purpose of IRAM placement.

### 3.4 Folded Logical Operations

The original V9938 logical operations switch had 13 cases for `OP` values 0–15. The optimized version folds transparent operations (OP ≥ 8) into their base counterparts:

```c
__attribute__((always_inline)) INLINE void VDPpsetlowlevel(byte *P, byte CL, byte M, byte OP) {
    if (OP >= 8) {           // Transparent operations: TSET, TAND, TOR, TXOR, TNOT
        if (!CL) return;     // Transparent + color 0 = no-op
        OP -= 8;             // Map to base operation
    }
    switch (OP) {            // Only 5 cases: SET, AND, OR, XOR, NOT
        case 0: *P = (*P & M) | CL; break;           // SET/IMP
        case 1: *P = *P & (CL | M); break;           // AND
        case 2: *P |= CL; break;                     // OR
        case 3: *P ^= CL; break;                     // XOR
        case 4: *P = (*P & M) | ~(CL | M); break;    // NOT
    }
}
```

The 13-case switch collapses to 5. The `OP >= 8` guard is a single unsigned comparison, predicted not-taken for the common opaque-operation case.

---

## 4. YJK SWAR Renderer (Common.h)

### 4.1 32-bit Aligned VRAM Fetches

```c
for (X = 1; X < 64; X++, T += 4, R += 4, P += 4) {
    // 1. Load 4 bytes of VRAM at once (aligned 32-bit)
    uint32_t t_val = (uint32_t)T[0] | ((uint32_t)T[1] << 8)
                     | ((uint32_t)T[2] << 16) | ((uint32_t)T[3] << 24);
    uint32_t r_val = (uint32_t)R[0] | ((uint32_t)R[1] << 8)
                     | ((uint32_t)R[2] << 16) | ((uint32_t)R[3] << 24);
```

### 4.2 Branchless Bit Extraction

```c
    int K = (t_val & 0x07) | ((t_val >> 5) & 0x38);
    K = (K ^ 0x20) - 0x20;  // branchless sign-extend

    int J = ((t_val >> 16) & 0x07) | ((t_val >> 21) & 0x38);
    J = (J ^ 0x20) - 0x20;
```

### 4.3 Precomputed Chroma Offset and LUT Clamping

```c
    int b_off = -(J << 1) - K;  // precomputed once per 4 pixels

    // LUT-based, zero branches
    int R = clip_RG[Y + J + 32];
    int G = clip_RG[Y + K + 32];
    int B = clip_B[(((Y << 2) + Y + B_offset) >> 2) + 93];
    return BPal[(R & 0x1C) | ((G & 0x1C) << 3) | (B >> 3)];
```

### 4.4 Parallel Sprite Overlay

```c
    P[0] = (r_val & 0xFF) ? XPal[r_val & 0xFF] : YJKColor(y0, J, K, b_off);
    P[1] = ((r_val >> 8) & 0xFF) ? XPal[(r_val >> 8) & 0xFF] : YJKColor(y1, J, K, b_off);
    P[2] = ((r_val >> 16) & 0xFF) ? XPal[(r_val >> 16) & 0xFF] : YJKColor(y2, J, K, b_off);
    P[3] = ((r_val >> 24) & 0xFF) ? XPal[(r_val >> 24) & 0xFF] : YJKColor(y3, J, K, b_off);
```

---

## 5. Screen 5 Dual-Pixel LUT

The `SC5_LUT` precomputes dual-pixel pairs for Screen 5 (256 entries × 4 bytes = 1 KB DRAM):

```c
static uint32_t SC5_LUT[256];

// Precomputation: for each byte (2 pixels in Screen 5)
// SC5_LUT[byte] = (left_pixel << 16) | right_pixel
static inline void SyncPalette(void) {
    for (int i = 0; i < 256; i++) {
        uint32_t c_left = XPal[i >> 4];
        uint32_t c_right = XPal[i & 0x0F];
        SC5_LUT[i] = c_left | (c_right << 16);
    }
}
```

In the renderer:
```c
    WRITE_P(0, SC5_LUT[t_low & 0xFF]);        // 2 pixels per 1 lookup
    WRITE_P(2, SC5_LUT[(t_low >> 8) & 0xFF]); // another 2 pixels
```

---

## 6. Z80 Memory Access Optimizations (MSX.c)

### 6.1 Software Line Cache

A 1 KB direct-mapped cache for ROM reads at 0x4000-0x7FFF:

```c
__attribute__((section(".data"))) static uint8_t ROMLineCache[16][64];
__attribute__((section(".data"))) static uint32_t ROMLineTag[16];
#define LINE_IDX(A) (((A) >> 6) & 0x0F)
```

On write-through in `WrZ80`:
```c
if (pg == 2 || pg == 3) {
    int idx = LINE_IDX(off);
    uint32_t line_base = (uint32_t)(RAM[pg]) + (off & ~0x3F);
    if (ROMLineTag[idx] == line_base) {
        ROMLineCache[idx][off & 0x3F] = V;  // Update cached line
    }
}
```

### 6.2 Branch Prediction Hints

```c
#define LIKELY(x) __builtin_expect(!!(x), 1)
#define UNLIKELY(x) __builtin_expect(!!(x), 0)

IRAM_ATTR byte RdZ80(word A) {
    if (LIKELY((A & 0x3F88) != 0x3F88)) {
        return (RAM[A >> 13][A & 0x1FFF]);
    }
    // ... cold paths ...
}
```

---

## 7. VGA Driver Architecture (vga.h)

### 7.1 DRAM Render Buffer + Hardware Byte-Swap

```c
// Buffer in internal DRAM for fast Retro-Go writes
static uint16_t DRAM_ATTR __attribute__((aligned(64))) sram_render_buffer[MAX_CHUNK_PIXELS];

// Hardware byte-swap in LCD_CAM (zero CPU cost)
LCD_CAM.lcd_user.lcd_byte_order = 1;
```

Pixel data flow:
1. Retro-Go `write_update()` → `sram_render_buffer` (Fast DRAM, 5KB)
2. `lcd_send_buffer()`: Pure `memcpy` → `vga_fb` (PSRAM)
3. LCD_CAM DMA reads `vga_fb` (PSRAM)
4. Hardware byte-swap (`lcd_byte_order=1`)
5. GPIO → VGA DAC (R-2R ladder, 16-bit)

---

## 8. Dual Audio Engine (Sound.c)

This audio engine represents a unique hybrid system with two fully independent synthesis modes, switchable at runtime:

| Component       | Description                                        | Original Author             |
| --------------- | -------------------------------------------------- | --------------------------- |
| **PSG**         | AY-3-8910 / YM2149 (3 channels, square/noise/ADSR) | Mitsutaka Okazaki (emu2149) |
| **OPLL**        | YM2413 FM-PAC (9 FM channels, 5 percussion)        | Mitsutaka Okazaki (emu2413) |
| **SCC**         | Konami SCC (5 wavetable channels)                  | Mitsutaka Okazaki (emu2212) |
| **Fast Engine** | Branchless meander synthesis                       | Marat Fayzullin (fMSX)      |

#### Audio Data Flow

```text
┌─────────────────────────────────────────────────────────────┐
│                    Sound.c (Orchestrator)                   │
│  ┌─────────────┐  ┌─────────────┐  ┌─────────────────────┐  │
│  │ SND_MODE_   │  │ SND_MODE_   │  │   MIDI Logger       │  │
│  │ FAST        │  │ ACCURATE    │  │ (MIDI file export)  │  │
│  │ (Marat)     │  │ (Okazaki)   │  │                     │  │
│  └──────┬──────┘  └──────┬──────┘  └─────────────────────┘  │
│         │                │                                  │
│    ┌────┴────┐      ┌────┴────┐                             │
│    │WaveCH[] │      │PSG+OPLL+│                             │
│    │16 ch    │      │SCC      │                             │
│    │simple   │      │studio   │                             │
│    │square   │      │chips    │                             │
│    └────┬────┘      └────┬────┘                             │
│         │                │                                  │
│    ┌────┴────┐      ┌────┴────┐                             │
│    │SoftClip │      │SoftClip │                             │
│    │Limiter  │      │Limiter  │                             │
│    └────┬────┘      └────┬────┘                             │
│         │                │                                  │
│    ┌────┴────────────────┴────┐                             │
│    │    Stereo/Mono Mixer     │                             │
│    │  PSG center | OPLL right │                             │
│    │  SCC left   | SoftClip   │                             │
│    └────────────┬─────────────┘                             │
│                 │                                           │
│         ┌───────┴───────┐                                   │
│         ▼               ▼                                   │
│    ┌─────────┐     ┌─────────┐                              │
│    │  I2S    │     │  I2S    │                              │
│    │  Left   │     │  Right  │                              │
│    └─────────┘     └─────────┘                              │
└─────────────────────────────────────────────────────────────┘
```

### 8.1 SND_MODE_FAST: Branchless Meander (Marat Fayzullin)

The Fayzullin mode is a brilliant simplification: instead of accurately emulating chips, it synthesizes a "recognizable approximation" with minimal CPU cost.

**Key Hack: Branchless Square Wave**

```c
// Instead of if-based zero-crossing detection:
// (((L1-K)^(L1+K)) >> 15) & 1
// 
// XOR between previous and next phase gives 1 only at
// zero-crossing — without branches!
//
// (((L1 >> 15) & 1) * 255) - 128 — amplitude ±127 without if()
// (1 - trans) * amp * V — zeroes out when trans=1 (transition)
```

| Parameter           | Value                                               |
| ------------------- | --------------------------------------------------- |
| **CPU per channel** | ~1 cycle/sample                                     |
| **Channels**        | 16 (SND_CHANNELS)                                   |
| **Wave types**      | SQUARE, TRIANGLE, NOISE, WAVE                       |
| **Panning**         | Hard: channels 0-5 = center, 6-15 = alternating L/R |

**Fast Mode Panning**

```c
int pan_l = (J < 6) ? 256 : ((J & 1) ? 200 : 70);
int pan_r = (J < 6) ? 256 : ((J & 1) ? 70 : 200);
```

- Channels 0-5: center (mono) — typically PSG + primary channels
- Channels 6+: even left (70%), odd right (200%) — pseudo-stereo

### 8.2 SND_MODE_ACCURATE: Studio Chip Emulation (Mitsutaka Okazaki)

Three studio-grade chips with cycle-accurate emulation:

### 8.3 PSG: AY-3-8910 with DC Blocker (emu2149.c)

```c
// DC Blocker — critically important!
static int psg_dc = 0;
// ...
psg_dc += (raw_psg - psg_dc) >> 8;        // α = 1/256
psg_val = (raw_psg - psg_dc) * 2;         // DC removal + amplification
```

| Filter     | Type    | Cutoff          | Purpose                              |
| ---------- | ------- | --------------- | ------------------------------------ |
| DC Blocker | IIR HPF | ~30 Hz @ 32kHz  | Removes DC offset, protects speakers |

**Why this matters:** The PSG outputs an asymmetric signal (logarithmic DAC table). Without a DC blocker, a DC component accumulates, leading to:
- Speaker distortion
- Loss of dynamic range
- Inability to amplify (clipping)

### 8.4 OPLL: YM2413 FM-PAC — The 400KB Table Elimination (emu2413.c)

The most impressive optimization feat in the entire project.

**The Original Problem**
Standard emu2413 uses massive lookup tables:
- `dphaseTable[16][16][16]` — phase increments
- `tllTable[16][16][32]` — total level + key scaling
- `noiseTable[16][16]` — noise phases
- Total: >400 KB in PSRAM/Flash

**The Solution: Arithmetic Instead of Tables**

```c
// calc_pg_dphase — replaces 3D table with 1-cycle arithmetic
INLINE static e_uint32 calc_pg_dphase(e_uint32 fnum, e_uint32 block, e_uint32 ML) {
    static const e_uint32 mltable[16] = { 1,2,4,6,8,10,12,14,16,18,20,20,24,24,30,30 };
    e_uint32 base = ((fnum * mltable[ML]) << block) >> 2;

    if (rate == 49716) return base;  // Fast path for standard rate

    return (e_uint32)(((uint64_t)base * rate_adjust_mult) >> 16);
}

// calc_noise_dphase — replaces noiseTable with bit shift!
INLINE static e_uint32 calc_noise_dphase(e_uint32 i, e_uint32 j) {
    e_uint32 base = i << j;  // Just a shift!
    if (rate == 49716) return base;
    return (e_uint32)(((uint64_t)base * rate_adjust_mult) >> 16);
}
```

| Metric                | Before            | After              | Gain                |
| --------------------- | ----------------- | ------------------ | ------------------- |
| **Lookup Tables**     | >400 KB           | ~11.4 KB           | **35x smaller**     |
| **Location**          | PSRAM/Flash       | DRAM (DRAM_ATTR)   | **No cache misses** |
| **Phase computation** | Memory load       | Integer arithmetic | **1 cycle**         |
| **Latency**           | Flash wait states | DRAM single-cycle  | **Deterministic**   |

**DRAM_ATTR Strategy**

```c
// All tables in fast DRAM instead of slow Flash
static DRAM_ATTR e_uint16 fullsintable[PG_WIDTH];      // 512 bytes
static DRAM_ATTR e_uint16 halfsintable[PG_WIDTH];     // 512 bytes
static DRAM_ATTR e_int32 pmtable[PM_PG_WIDTH];         // 1 KB
static DRAM_ATTR e_int32 amtable[AM_PG_WIDTH];         // 1 KB
static DRAM_ATTR e_int16 DB2LIN_TABLE[...];            // 2 KB
static DRAM_ATTR e_uint16 AR_ADJUST_TABLE[...];         // 256 bytes
static DRAM_ATTR e_uint32 dphaseARTable[16][16];       // 1 KB
static DRAM_ATTR e_uint32 dphaseDRTable[16][16];       // 1 KB
static DRAM_ATTR e_uint32 tll_base[16][8][4];          // 512 bytes
static DRAM_ATTR e_int32 rksTable[2][8][2];            // 128 bytes
```

`DRAM_ATTR` is a critical ESP32 marker: data is placed in Internal SRAM (8.5 KB of tables), not in Flash/PSRAM. Access to Internal SRAM takes 1 cycle, whereas Flash incurs a cache miss penalty (up to 100+ cycles).

**IRAM_ATTR for Hot Path**

```c
EMU2413_API IRAM_ATTR e_int16 OPLL_calc(OPLL *opll) {
    // This function is called 32,000 times per second (at 32kHz)
    // IRAM_ATTR copies it into Instruction RAM (32 KB)
    // Eliminates cache misses on the hot path!
}
```

### 8.5 SCC: Konami Wavetable with LPF (emu2212.c)

```c
// Single-pole LPF for smoothing wavetable steppiness
static int scc_lpf = 0;
scc_lpf += (raw_scc - scc_lpf) >> 1;  // α = 0.5, fc ≈ 5.1 kHz @ 32kHz
scc_val = scc_lpf;
```

| Parameter          | Value                                  |
| ------------------ | -------------------------------------- |
| **Wavetable size** | 32 bytes × 5 channels                  |
| **Interpolation**  | LINEAR (selectable: NONE/LINEAR/CUBIC) |
| **LPF Cutoff**     | ~5.1 kHz                               |
| **Purpose**        | Smoothing aliasing on high notes       |

### 8.6 Stereo Soundstage Design

```text
        LEFT                    CENTER                   RIGHT
          │                       │                        │
    ┌─────┴─────┐           ┌─────┴──────┐           ┌─────┴─────┐
    │   SCC     │           │   PSG      │           │   OPLL    │
    │  (warm)   │           │(foundation)│           │ (bright)  │
    │  78% L    │           │  100% L/R  │           │  78% R    │
    │  27% R    │           │            │           │  27% L    │
    └───────────┘           └────────────┘           └───────────┘

    ←─────── 200/70 = 2.86:1 panning ratio ───────→
```

This is a deliberate artistic decision:
- **SCC on the left:** Konami wavetable sound — "warm", "organic", analog
- **OPLL on the right:** Yamaha FM synthesis — "cold", "digital", "glassy"
- **PSG in the center:** foundation, rhythm and bass anchor

The panning coefficients:
```c
out_l += (opll_val * 70) >> 8;   // ~0.27
out_r += (opll_val * 200) >> 8;  // ~0.78
out_l += (scc_val * 200) >> 8;   // ~0.78
out_r += (scc_val * 70) >> 8;    // ~0.27
```

### 8.7 SoftClip Limiter

```c
// Threshold: 18000 (55% of 32767)
// Compression: 4:1 above threshold
//
// Example:
// Input: 25000 → 18000 + (7000 >> 2) = 18000 + 1750 = 19750
// Input: 40000 → 18000 + (22000 >> 2) = 18000 + 5500 = 23500
// Input: 100000 → 18000 + 82000>>2 = 18000 + 20500 = 38500 → clamp to 32767
```

| Parameter        | Value                                   |
| ---------------- | --------------------------------------- |
| **Threshold**    | 18000 (~55% headroom)                   |
| **Ratio**        | 4:1 (soft knee)                         |
| **Hard ceiling** | 32767 / -32768                          |
| **Purpose**      | Clipping protection when mixing 3 chips |

---

## 9. Xtensa LX7 Audio Optimizations

### 9.1 IRAM_ATTR for Hot Paths

```c
// IRAM_ATTR copies function to Instruction RAM (32 KB total)
// Eliminates Flash cache misses during audio generation
EMU2149_API IRAM_ATTR e_int16 PSG_calc(PSG *psg);
EMU2413_API IRAM_ATTR e_int16 OPLL_calc(OPLL *opll);
```

### 9.2 DRAM_ATTR Memory Strategy

```c
// DRAM_ATTR places data in Internal SRAM (fast, deterministic)
// Instead of PSRAM/Flash (slow, cache-miss prone)
static DRAM_ATTR e_uint16 fullsintable[PG_WIDTH];
static DRAM_ATTR e_int32 pmtable[PM_PG_WIDTH];
```

| Allocator                       | Location      | Latency                    |
| ------------------------------- | ------------- | -------------------------- |
| `heap_caps_malloc(...INTERNAL)` | Internal SRAM | 1 cycle                    |
| `malloc()` (default)            | PSRAM         | 1-10 cycles + cache misses |
| `ps_malloc()`                   | PSRAM         | 10-100 cycles              |

### 9.3 Branch Prediction Hints

```c
#define LIKELY(x)   __builtin_expect(!!(x), 1)   // x likely true
#define UNLIKELY(x) __builtin_expect(!!(x), 0)   // x likely false

// In hot path:
if(UNLIKELY(SndRate < 8192)) return;  // Compiler places unlikely branch at end

if(UNLIKELY(slot->egout >= (DB_MUTE-1))) {  // EG finish — rare event
    slot->output[0] = 0;
}
```

Xtensa LX7 has static branch prediction: forward jumps = not taken, backward = taken. `__builtin_expect` helps the compiler choose optimal layout.

### 9.4 Bitwise Branchless Operations

| Operation | Code                          | Cycles               | Replaces         |
| --------- | ----------------------------- | -------------------- | ---------------- |
| Minimum   | `x = a < b ? a : b`           | 1 (conditional move) | if-else (branch) |
| Clamping  | `x > 32767 ? 32767 : x`       | 1 (MIN instruction)  | if-else (branch) |
| Scaling   | `(x * 70) >> 8`               | 1 (MUL + SRL)        | float division   |
| Sign      | `(x ^ (x >> 31)) - (x >> 31)` | 2                    | abs() (branch)   |

---

## 10. Performance Metrics

**Video Performance**

| Mode              | FPS     | Notes                                  |
| ----------------- | ------- | -------------------------------------- |
| **MSX2 (V9938)**  | ~45-55  | HMMM scrolling thrashes PSRAM          |
| **MSX2+ (V9958)** | 60+     | Hardware scrolling, zero VRAM pressure |
| **MSX2+ YJK**     | 60+     | SWAR batch processing, aligned loads   |

**Audio Performance**

| Mode                        | FPS   | CPU Load | Notes                  |
| --------------------------- | ----- | -------- | ---------------------- |
| **Fast**                    | 74-75 | ~15%     | All games, 16 channels |
| **Accurate (PSG+SCC)**      | 74-75 | ~25%     | Without OPLL           |
| **Accurate (PSG+OPLL+SCC)** | 55-60 | ~40%     | Full studio mix        |

**Memory Usage**

| Type      | Available | Used     | Remaining  |
| --------- | --------- | -------- | ---------- |
| **DRAM**  | 320 KB    | ~172 KB  | **148 KB** |
| **PSRAM** | 8 MB      | ~1.2 MB  | **6.8 MB** |

**Optimization Statistics**

| File      | Bit Shifts | AND Masks | XOR Ops | Ternary Ops | Inline Hints |
| --------- | ---------- | --------- | ------- | ----------- | ------------ |
| Sound.c   | 47         | 54        | 3       | 19          | 4            |
| emu2413.c | 100        | 170       | 9       | 7           | 40           |
| emu2149.c | 12         | 30        | 5       | 1           | 5            |
| emu2212.c | 20         | 73        | 0       | 0           | 6            |

---

## 11. Z80 CPU Core Optimizations (Z80.c)

The Z80 emulation core is the heart of the entire system — every video frame, every audio sample, every I/O operation flows through this interpreter. On a 3.5 MHz Z80 (MSX standard), the emulator must execute roughly ~100,000–200,000 instructions per frame (at 60 FPS). At 240 MHz, the ESP32-S3 has ~4,000 cycles per emulated Z80 cycle — but Flash cache misses can burn hundreds of cycles in a single stall.

The original fMSX Z80 core used a classic switch/case dispatcher. On Xtensa, this is catastrophic:
- Switch/case compiles to a jump table in Flash
- Every opcode fetch = Flash read = potential cache miss
- The 3-stage pipeline stalls on every mispredict

### 11.1 Threaded Code: Computed Goto

The entire dispatcher was replaced with threaded code (computed goto):

```c
// Static jump table in DRAM (1 KB)
static const void *const op_table[256] Z80_TABLE_ATTR = {
    &&op_NOP, &&op_LD_BC_WORD, &&op_LD_xBC_A, ...
};

// NEXT_OP macro — the core dispatch engine
#define NEXT_OP() \
    do { \
        if (UNLIKELY(ICount <= 0)) goto check_ints; \
        I = OpZ80(PC++); \
        if (TurboMode && LIKELY(I != 0xD3 && I != 0xDB)) { \
            ICount -= (Cycles[I] >> 1); \
        } else { \
            ICount -= Cycles[I]; \
        } \
        INCR(1); \
        goto *op_table[I]; \
    } while(0)
```

| Aspect                 | switch/case                   | Computed Goto             |
| ---------------------- | ----------------------------- | ------------------------- |
| **Dispatch mechanism** | Jump table in Flash           | Label addresses in DRAM   |
| **Cache behavior**     | Flash miss possible           | DRAM — always hit         |
| **Branch prediction**  | Indirect jump (unpredictable) | Direct goto (predictable) |
| **Pipeline stalls**    | 8+ cycles per dispatch        | 2–3 cycles per dispatch   |
| **Xtensa penalty**     | CALL8 + window rotation       | Direct branch             |

The `goto *op_table[I]` is a direct branch on Xtensa. The target address is loaded from DRAM (1 cycle), and the CPU jumps directly — no function call overhead, no stack manipulation, no window rotation.

### 11.2 Register Caching (Level 1)

The Z80 PC (Program Counter) and ICount (Instruction Counter) are accessed on every single opcode. In the original code, this meant:

```c
// Original: struct dereference on EVERY opcode
I = OpZ80(R->PC.W++);
R->ICount -= Cycles[I];
```

This is a memory access to the Z80 structure (in DRAM or PSRAM). Even in DRAM, it's a load/store cycle that competes with other memory traffic.

The optimized version pins these to hardware registers:

```c
// Optimized: local variables → compiler assigns to registers
uint32_t PC = R->PC.W;      // Loaded once, stays in register
int ICount = R->ICount;     // Loaded once, stays in register

// All opcode handlers use local PC/ICount directly
// No struct dereferencing in the hot path!
```

The `Z80_PC` macro trick:

```c
// For legacy headers (Codes.h, CodesCB.h, etc.):
#define Z80_PC R->PC.W   // Used by prefix handlers

// Then redefined for RunZ80:
#undef Z80_PC
#define Z80_PC PC        // Maps to local variable!
```

This allows the same opcode definitions in `Codes.h` to work with both:
- Prefix handlers (CB, DD, ED, FD): use `R->PC.W` (struct member)
- `RunZ80` hot path: use `PC` (hardware register)

### 11.3 Smart Turbo Mode

A global `TurboMode` variable enables 2x CPU throughput while preserving timing-critical operations:

```c
uint8_t TurboMode = 0;

// In NEXT_OP:
if (TurboMode && LIKELY(I != 0xD3 && I != 0xDB)) {
    ICount -= (Cycles[I] >> 1);  // Half cost for most instructions
} else {
    ICount -= Cycles[I];          // Normal cost
}
```

**Protected opcodes (never turbo'd):**

| Opcode           | Instruction                     | Why Protected                         |
| ---------------- | ------------------------------- | ------------------------------------- |
| `0xD3`           | OUT A,(n)                       | VDP register writes need exact timing |
| `0xDB`           | IN A,(n)                        | VDP status reads need exact timing    |
| `0xED 0x40-0x7F` | IN/OUT block transfers          | Disk/tape I/O timing                  |
| `0xED 0xA0-0xBF` | LDI/LDD/CPI/CPD block transfers | Memory block ops                      |

**Result:** ~8.23 MHz effective Z80 frequency when `TurboMode=1` (vs 3.5 MHz standard).

### 11.4 CodesCB Force-Inlining

The CB prefix handler (bit manipulation, rotates, shifts) is called frequently — every RLC, BIT, SET, RES instruction goes through it. The original implementation was a function call:

```c
// Original: function call overhead
case PFX_CB: CodesCB(R); break;
```

On Xtensa, a function call requires:
1. `CALL8` instruction (8 registers to save)
2. Window rotation (register window slide)
3. Stack frame setup
4. Return address management

The optimized version:

```c
// Optimized: always_inline + pointer passing
__attribute__((always_inline)) static inline void 
CodesCB(register Z80 *R, uint32_t *PC, int *ICount) {
    register byte I;
    I = OpZ80((*PC)++);
    if (TurboMode) {
        *ICount -= (CyclesCB[I] >> 1);
    } else {
        *ICount -= CyclesCB[I];
    }
    INCR(1);
    switch(I) {
        #include "CodesCB.h"
    }
}
```

**Key insight:** The function receives `&PC` and `&ICount` pointers — no sync/restore needed. The CB handler operates directly on the cached registers, then falls through back to `NEXT_OP()`.

### 11.5 LDIR/LDDR Batch Optimization

Block transfer instructions (LDIR, LDDR, CPIR, CPDR) copy/compare memory blocks. The original implementation executed one byte per emulated instruction — correct but slow.

The optimized version uses a native C loop:

```c
case LDIR:
{
    R->AF.B.l &= ~(H_FLAG | N_FLAG | P_FLAG);
    if (R->BC.W) {
        do {
            WrZ80(R->DE.W++, RdZ80(R->HL.W++));
            R->BC.W--;
            R->ICount -= 21;
        } while (R->BC.W && R->ICount > 0);

        if (R->BC.W) {
            // Interrupted by ICount exhaustion
            R->AF.B.l |= P_FLAG;
            R->PC.W -= 2;  // Re-execute LDIR next time
        } else {
            // Completed: last iteration costs 16, not 21
            R->ICount += 5;
        }
    } else {
        R->ICount -= 16;
    }
    break;
}
```

**Why this is faster:**
- Loop unrolling by the C compiler generates efficient assembly
- No per-iteration dispatch overhead (no `NEXT_OP` between bytes)
- `ICount` check ensures we don't overshoot the timing boundary
- Cycle correction (+5 on completion) maintains exact Z80 timing accuracy

### 11.6 INDR/OTDR Bug Fix

The original Marat code had a subtle bug in INDR/OTDR:

```c
// Original (BUGGY):
case INDR:
    WrZ80(R->HL.W--, InZ80(R->BC.W));
    if(!--R->BC.B.h) { ... }   // WRONG! Decrements BEFORE check
```

The Z80 INDR instruction should decrement B, then check if B != 0 to continue. The original code used `!(--R->BC.B.h)` which checks if the result is zero after decrement — but the Z80 flags are set based on the decremented value, creating a logic mismatch.

```c
// Fixed:
case INDR:
    WrZ80(R->HL.W--, InZ80(R->BC.W));
    if(--R->BC.B.h) { ... }    // CORRECT! Decrement, then check non-zero
```

This bug affected disk and tape loaders that relied on exact INDR behavior for data transfer protocols.

### 11.7 Table Placement in DRAM

All Z80 lookup tables are forced into Internal SRAM:

```c
#if defined(CONFIG_IDF_TARGET_ESP32S3) || defined(ESP32)
  #define Z80_TABLE_ATTR __attribute__((section(".dram0.data")))
#else
  #define Z80_TABLE_ATTR
#endif

static const byte Cycles[256] Z80_TABLE_ATTR;
static const byte CyclesCB[256] Z80_TABLE_ATTR;
static const byte CyclesED[256] Z80_TABLE_ATTR;
static const byte CyclesXX[256] Z80_TABLE_ATTR;
static const byte CyclesXXCB[256] Z80_TABLE_ATTR;
static const byte ZSTable[256] Z80_TABLE_ATTR;
static const byte PZSTable[256] Z80_TABLE_ATTR;
static const word DAATable[2048] Z80_TABLE_ATTR;
```

| Table             | Size  | Purpose                    | Access Frequency        |
| ----------------- | ----- | -------------------------- | ----------------------- |
| `Cycles[256]`     | 256 B | Opcode cycle counts        | Every instruction       |
| `CyclesCB[256]`   | 256 B | CB prefix cycles           | Every CB instruction    |
| `CyclesED[256]`   | 256 B | ED prefix cycles           | Every ED instruction    |
| `CyclesXX[256]`   | 256 B | DD/FD prefix cycles        | Every IX/IY instruction |
| `CyclesXXCB[256]` | 256 B | DD/FD+CB cycles            | Rare (bit ops on IX/IY) |
| `ZSTable[256]`    | 256 B | Zero/Sign flags            | Every ALU operation     |
| `PZSTable[256]`   | 256 B | Parity/Zero/Sign flags     | Every rotate/shift      |
| `DAATable[2048]`  | 4 KB  | Decimal Adjust Accumulator | Every DAA instruction   |

Total table footprint: ~6 KB in DRAM — well within the 320 KB budget.

### 11.8 IRAM Placement Strategy

```c
// RunZ80 — the main execution loop
word __attribute__((section(".iram1.text"))) RunZ80(register Z80 * restrict R) { ... }

// ExecZ80 — cycle-accurate execution
int __attribute__((section(".iram1.text"))) ExecZ80(register Z80 * restrict R, register int RunCycles) { ... }

// Prefix handlers (called from hot path)
IRAM_ATTR static void CodesED(register Z80 *R) { ... }
IRAM_ATTR static void CodesDD(register Z80 *R) { ... }
IRAM_ATTR static void CodesFD(register Z80 *R) { ... }
```

The `restrict` keyword tells the compiler that `R` is the only pointer to that Z80 structure, enabling aggressive optimizations:
- No aliasing checks needed
- Struct members can be cached in registers longer
- Load/store reordering is safe

### 11.9 Branch Prediction Architecture

Xtensa LX7 uses static branch prediction:
- Forward branches (goto forward): predicted NOT taken
- Backward branches (goto backward): predicted TAKEN

The `NEXT_OP()` macro exploits this:

```c
#define NEXT_OP() \
    do { \
        if (UNLIKELY(ICount <= 0)) goto check_ints;  // Forward → predicted NOT taken \
        I = OpZ80(PC++);                             // Fall through (common case) \
        /* ... */ \
        goto *op_table[I];                           // Direct jump (no prediction needed) \
    } while(0)
```

The `check_ints` label is forward from the dispatch loop, so the CPU predicts not taken — correct for 99%+ of instructions. Only when `ICount` expires (every ~1000 instructions) does the branch mispredict.

The `UNLIKELY()` macro compiles to:

```asm
    // Xtensa assembly
    bgez a4, .Lnext_op      // if ICount > 0, continue (predicted taken)
    j check_ints            // forward jump (predicted not taken)
.Lnext_op:
    l8ui a5, a6, 0          // I = OpZ80(PC++)
```

---

## 12. The Takeaway

In the world of embedded emulation, the complexity of the original system is rarely the bottleneck. To achieve a stable 60 FPS without frameskip on a $5 chip, the battle is won by identifying how original software worked around older hardware limitations, and surgically rewriting those bottlenecks to play nice with modern microcontroller caching and bus topologies.

The MSX2+ doesn't run faster because it's simpler. It runs faster because its advanced features—hardware scrolling, YJK color—let the original software avoid the expensive workarounds that the MSX2 demanded. The V9958's `R#25` replaces a thousand `HMMM` pixel copies. The YJK modes, despite their theoretical complexity, expose regular 4-pixel batches that yield to SWAR optimization—while the legacy tile modes hide their complexity behind per-pixel branch mosaics.

The emulator's job is not to faithfully recreate every transistor. It is to recognize that a register write on the V9958 replaces a thousand pixel copies on the V9938, and to ensure that the rendering path for those "heavy" YJK modes is tuned for 32-bit alignment, branchless execution, and cache-friendly sequential access.

### CPU Core Takeaway

The Z80 core demonstrates that interpreter optimization is an architecture-specific art:

1. **Computed goto** eliminates the dispatcher bottleneck — but only works well on architectures with fast indirect branches (Xtensa qualifies).
2. **Register caching** requires understanding the compiler's register allocator — `restrict` and `register` hints guide it.
3. **Smart Turbo** preserves correctness while maximizing throughput — the key is identifying which operations are timing-sensitive.
4. **DRAM placement of tables** is non-negotiable on Flash-heavy systems — 6 KB of tables in SRAM saves thousands of cycles per frame.
5. **Force-inlining prefix handlers** eliminates call overhead — but must be paired with pointer-passing to avoid sync costs.

*"This is not just an emulator port — it is a redesign for silicon constraints."*

---

## 13. DS-LZ State Compression (State.h)

The emulator saves its entire state (4.4 MB RAM + VRAM) to SD card with 822× compression, achieving 5.23 KB save files for title screens and 39.87 KB for in-game states. The compression engine, Delta-Stride LZ (DS-LZ), is a domain-optimized LZ77 variant designed specifically for tile-based graphics and retro emulator memory dumps.

### 13.1 The Domain-Specific Advantage

General-purpose compressors (gzip, LZ4, Zstd) treat save state data as opaque bytes. They search for repeated byte sequences in a sliding window, unaware of the underlying data structure. DS-LZ exploits a critical property of MSX VRAM: it is organized in 128-byte tile rows (Screen 0-5) or 256-byte rows (Screen 6-12).

**Key insight:** Two adjacent rows of the same tile are often identical or differ by only a single bit. If you XOR each byte with the corresponding byte in the previous row (vertical delta-XOR with the correct stride), identical rows become streams of zeros. LZ77 compresses zeros with near-perfect efficiency — a single command can encode thousands of consecutive zero bytes.

This single piece of domain knowledge — "the stride is 128" — is worth more than any amount of entropy coding, optimal parsing, or large-window search that general-purpose algorithms bring.

### 13.2 Architecture

| Component | Technique | Benefit |
|-----------|-----------|---------|
| **Pre-processor** | 2D delta-XOR (vertical then horizontal) | Turns 80-90% of VRAM into zeros |
| **Hash function** | Multiplicative with golden ratio constants | Better distribution for sparse post-delta data |
| **Match engine** | 2-way associative hash, 64KB sliding window | Fast lookup with controlled collisions |
| **Token encoding** | Variable-length: near (8-bit offset), far (16-bit), rep (cached offset) | Rep-match in 1 byte vs 3 bytes for far-match |
| **Lazy matching** | Cost-based evaluation, threshold 16 | Near-optimal parse without expensive DP |
| **I/O layer** | 4KB buffered reads and writes | 6× faster SD card access vs per-byte I/O |
| **Delta encode** | 32-bit vectorized XOR for vertical pass | 1.5-2× faster than byte-by-byte loop |
| **Match extension** | 32-bit compare (4 bytes per iteration) | 3-4× faster than single-byte comparison |

**Command format (backward compatible, Header[9] = 2):**

| Range | Type | Encoding |
|-------|------|----------|
| `0xxxxxxx` (0–127) | Literals | Length = cmd+1 (1–128 bytes) |
| `10xxxxxx` (128–191) | Near match | Length = (cmd&0x3F)+3 (3–66), 8-bit offset |
| `110xxxxx` (192–223) | Far match short | Length = (cmd&0x1F)+4 (4–35), 16-bit offset |
| `1110xxxx` (224–239) | Rep match short | Length = (cmd&0x0F)+3 (3–18), uses last_offset |
| `11110xxx` (240–247) | Long literals | Length = (cmd&0x07)+129 (129–136) |
| `11111000` (248) | Escape | Extended lengths via sub-commands |

### 13.3 Performance

| Metric | Value |
|--------|-------|
| **Compression ratio** (title screen) | 822× (4.4 MB → 5.23 KB) |
| **Compression ratio** (in-game) | 108× (4.4 MB → 39.87 KB) |
| **Save time** | ~50 ms |
| **Load time** | ~20 ms |
| **Encoder RAM** | 32 KB PSRAM (4096 × 2-way × 4-byte hash table) |
| **Decoder RAM** | 0 bytes heap (in-place reconstruction into VRAM/RAM arrays) |
| **Format version** | Header[9] = 2 (backward compatible) |
| **Code footprint** | ~500 lines of C (single header, zero dependencies) |

### 13.4 Why It Beats General-Purpose Compressors

| Algorithm | VRAM Compression (title screen) | Encoder RAM | Domain-Aware |
|-----------|--------------------------------|-------------|--------------|
| **DS-LZ** | **822×** | 32 KB PSRAM | ✅ |
| gzip/DEFLATE | ~50× | 256 KB | ❌ |
| LZ4 | ~2× | 0 | ❌ |
| LZSA2 | ~3× | 0 | ❌ |
| Zstd level 9 | ~4× | 128 MB | ❌ |
| ZX7 (optimal LZ77) | ~3× | 0 | ❌ |

The domain-specific delta-XOR transform is worth more than any amount of entropy coding or optimal parsing. Knowing that the data is organized in 128-byte tile rows is the entire advantage. Everything else — rep-match caching, variable-length codes, cost-based lazy matching — is just efficiently harvesting the gains that the transform already created.

### 13.5 Evolution

| Milestone | Changes | Compression Gain | Speed Gain |
|-----------|---------|-----------------|------------|
| **Initial** | Core DS-LZ: delta + rep-match + VLC + cost-based lazy | baseline | baseline |
| **I/O & match** | Buffered I/O (4KB), 32-bit match extension | 0% | 2× (save/load) |
| **Delta & hash** | Vectorized delta (32-bit XOR), improved hash function | +2% | 1.5× |

**Future directions:**
- Tile-First Reordering: rearrange VRAM from row-major to tile-major before delta, enabling single-command encoding of repeated tiles
- Optimal Parsing: replace greedy/lazy matching with full dynamic programming for maximum compression
- 4 KB DRAM hash table: move from 32 KB PSRAM to 4 KB internal DRAM using 16-bit offsets

### 13.6 License & Attribution

DS-LZ is an independent implementation based on public-domain LZ77 (Lempel-Ziv, 1977). All patents have expired.

The DS-LZ compression engine is:

**CC BY-NC-SA 4.0** — Creative Commons Attribution-NonCommercial-ShareAlike 4.0

| Requirement | Meaning |
|-------------|---------|
| **Attribution** | Credit must be given to Ivan Svarkovsky |
| **NonCommercial** | Commercial use prohibited |
| **ShareAlike** | Derivatives must use the same license |

A standalone single-header library (`ds-lz.h`) is available under the same license: [github.com/Svarkovsky/dslz](https://github.com/Svarkovsky/dslz)

The SaveState/LoadState framework and MSX-specific structures remain Copyright (C) Marat Fayzullin 1994-2021.

*Last updated: 2026-06-14*
