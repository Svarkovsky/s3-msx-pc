/**
    fMSX: portable MSX emulator — Common.h

    Standard screen refresh drivers common for X11, VGA, and other
    "chunky" bitmapped video implementations. Also includes dummy
    sound drivers for fMSX.

    Original code Copyright (C) Marat Fayzullin 1994-2021
    You are not allowed to distribute this software commercially.
    Please notify the original author if you make changes.

    Optimized for ESP32-S3 (Xtensa LX7) by Ivan Svarkovsky, 2026.
    Contact: ivansvarkovsky@gmail.com

    ─────────────────────────────────────────────────────────
    Optimizations in this version:
    ─────────────────────────────────────────────────────────

    -- Base Infrastructure & CPU Pipeline Optimizations --

    - Native 512px rendering restored: Screen 6, 7, and Text80 modes
      now render full 512/480 pixels directly into the PSRAM framebuffer.
    - RefreshLine5/8/10/12 + ColorSprites: moved to IRAM via IRAM_ATTR to
      eliminate Flash cache misses during hot rendering loops.
    - SWAR (SIMD Within A Register) bit extraction for Y/J/K values.
    - Precomputed b_off = -(J<<1)-K passed to YJKColor avoids redundant
      calculations per pixel.
    - Branch prediction hints (__builtin_expect) on all cold paths.

    -- Speed & Register-Caching Optimizations --

    - Register-Cached VRAM & ZBuf: Inside RefreshLine5, reads VRAM and
      sprite buffers as 32-bit words, caching them in CPU registers
      and reducing memory bus transactions by 4x-8x.
    - SC5_LUT: 1 KB DRAM lookup table for dual-pixel pairs in SCREEN 5.
    - SprPal (LUT Fusion): Merges sprite-to-color and color-to-pixel
      lookups into a single 16-entry array.
    - Zero-Branch YJK Clipping: Precomputed clipping LUTs (clip_RG & clip_B).
    - Unaligned Write Protection & UB Fix: Safely decomposes 32-bit pixel
      bursts via WRITE_P macro with explicit uint32_t casts to prevent
      Xtensa exceptions and GCC Signed Integer Overflow Undefined Behavior.

    This file is distributed under the same terms as the original
    fMSX code by Marat Fayzullin. Commercial distribution is
    prohibited without permission from the original author.
*/

#include <esp_attr.h>

extern int msx_active_width;

/* Palette dirty flag */
#ifndef MSX_PALETTE_DIRTY_DEFINED
#define MSX_PALETTE_DIRTY_DEFINED
bool msx_palette_dirty = true;
#endif

/* ── SCREEN 5 Dual-Pixel LUT & Sprite Palette ── */
#ifndef SC5_LUT_DEFINED
#define SC5_LUT_DEFINED
static uint32_t SC5_LUT[256] __attribute__((section(".dram0.data")));

static pixel SprPal[16];
static uint16_t last_xpal0 = 0xFFFF;
static bool sc5_lut_ready = false;
static bool spr_pal_ready = false;

static const byte SprToScr[16] = {
    0x00, 0x02, 0x10, 0x12, 0x80, 0x82, 0x90, 0x92,
    0x49, 0x4B, 0x59, 0x5B, 0xC9, 0xCB, 0xD9, 0xDB
};

/* Синхронизируем палитру мгновенно, как только меняется XPal[0] или другие цвета */
static inline void SyncPalette(void) {
    for (int i = 0; i < 256; i++) {
        uint32_t c_left = XPal[i >> 4];
        uint32_t c_right = XPal[i & 0x0F];
        SC5_LUT[i] = c_left | (c_right << 16);
    }
    for (int i = 0; i < 16; i++) {
        SprPal[i] = BPal[SprToScr[i]];
    }
    sc5_lut_ready = true;
    spr_pal_ready = true;
    msx_palette_dirty = false;
}
#endif

#ifndef SCANLINE_SPRITE_FLAG_DEFINED
#define SCANLINE_SPRITE_FLAG_DEFINED
static bool scanline_has_sprites = false;
#endif

static int FirstLine = 18;

static void Sprites(byte Y, pixel *Line);
static void ColorSprites(byte Y, byte *ZBuf);
static pixel *RefreshBorder(byte Y, pixel C);
static void ClearLine(pixel *restrict P, pixel C);
static pixel YJKColor(int Y, int J, int K, int B_offset);

/* Safe 32-bit pixel assignment to prevent unaligned exceptions on ESP32-S3 */
#define WRITE_P(idx, val)                                                      \
    do {                                                                       \
        uint32_t _v = (val);                                                   \
        P[(idx)] = _v;                                                         \
        P[(idx) + 1] = _v >> 16;                                               \
    } while (0)

/** RefreshScreen() ******************************************/
void RefreshScreen(void) {
    PutImage();
}

/** ClearLine() **********************************************/
static void ClearLine(register pixel *restrict P, register pixel C) {
    register int J;
    int activeW = msx_active_width;
    if (__builtin_expect(activeW <= 0, 0))
        return;
    for (J = 0; J < activeW; J++) {
        P[J] = C;
    }
}

/** YJKColor() ***********************************************/
#ifndef YJK_CLIP_LUT_DEFINED
#define YJK_CLIP_LUT_DEFINED
static const uint8_t clip_RG[95] = {
    0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
    0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  1,  2,  3,  4,  5,
    6,  7,  8,  9,  10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24,
    25, 26, 27, 28, 29, 30, 31, 31, 31, 31, 31, 31, 31, 31, 31, 31, 31, 31, 31,
    31, 31, 31, 31, 31, 31, 31, 31, 31, 31, 31, 31, 31, 31, 31, 31, 31, 31, 31};
static const uint8_t clip_B[345] = {
    0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
    0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
    0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
    0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
    0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  1,
    2,  3,  4,  5,  6,  7,  8,  9,  10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20,
    21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 31, 31, 31, 31, 31, 31, 31, 31,
    31, 31, 31, 31, 31, 31, 31, 31, 31, 31, 31, 31, 31, 31, 31, 31, 31, 31, 31,
    31, 31, 31, 31, 31, 31, 31, 31, 31, 31, 31, 31, 31, 31, 31, 31, 31, 31, 31,
    31, 31, 31, 31, 31, 31, 31, 31, 31, 31, 31, 31, 31, 31, 31, 31, 31, 31, 31,
    31, 31, 31, 31, 31, 31, 31, 31, 31, 31, 31, 31, 31, 31, 31, 31, 31, 31, 31,
    31, 31, 31, 31, 31, 31, 31, 31, 31, 31, 31, 31, 31, 31, 31, 31, 31, 31, 31,
    31, 31, 31, 31, 31, 31, 31, 31, 31, 31, 31, 31, 31, 31, 31, 31, 31, 31, 31,
    31, 31, 31, 31, 31, 31, 31, 31, 31, 31, 31, 31, 31, 31, 31, 31, 31, 31, 31,
    31, 31, 31, 31, 31, 31, 31, 31, 31, 31, 31, 31, 31, 31, 31, 31, 31, 31, 31,
    31, 31, 31, 31, 31, 31, 31, 31, 31, 31, 31, 31, 31, 31};
#endif

INLINE pixel YJKColor(int Y, int J, int K, int B_offset) {
    int R = clip_RG[Y + J + 32];
    int G = clip_RG[Y + K + 32];
    int B = clip_B[(((Y << 2) + Y + B_offset) >> 2) + 93];
    return BPal[(R & 0x1C) | ((G & 0x1C) << 3) | (B >> 3)];
}

/** RefreshBorder() ******************************************/
pixel *RefreshBorder(register byte Y, register pixel C) {
    register pixel *restrict P;
    register int H;
    int activeW = msx_active_width;
    
    // ==============================================================
    // (Overscan Blackout)
    // Перезаписываем переданный эмулятором цвет бордюра (C) на черный
    // ==============================================================
    // C = 0x0000; 
    //

    extern int frame_max_width;
    if (Y == 0)
        frame_max_width = activeW;
    else if (activeW > frame_max_width)
        frame_max_width = activeW;

    if (!Y) {
        FirstLine = (ScanLines212 ? 8 : 18) + VAdjust;
    }
    if (__builtin_expect(Y + FirstLine >= HEIGHT, 0)) {
        return 0;
    }

    XPal[0] = (!BGColor || SolidColor0) ? XPal0 : XPal[BGColor];
    P = (pixel *)XBuf;

    if (!Y) {
        int savedW = msx_active_width;
        msx_active_width = WIDTH * FirstLine;
        ClearLine(P, C);
        msx_active_width = savedW;
    }

    P += WIDTH * (FirstLine + Y);

    int leftBorder = (WIDTH - activeW) / 2 + HAdjust;
    if (__builtin_expect(leftBorder < 0, 0))
        leftBorder = 0;
    if (__builtin_expect(leftBorder + activeW > WIDTH, 0))
        leftBorder = WIDTH - activeW;

    int rightBorder = WIDTH - leftBorder - activeW;

    int savedW = msx_active_width;
    msx_active_width = leftBorder;
    ClearLine(P, C);
    msx_active_width = rightBorder;
    ClearLine(P + leftBorder + activeW, C);
    msx_active_width = savedW;

    H = ScanLines212 ? 212 : 192;

    if (__builtin_expect(Y == H - 1, 0)) {
        int remaining_lines = HEIGHT - H - FirstLine;
        if (__builtin_expect(remaining_lines > 0, 1)) {
            msx_active_width = WIDTH * remaining_lines;
            ClearLine((pixel *)XBuf + WIDTH * (FirstLine + H), C);
        }
        msx_active_width = savedW;
    }

    return P + leftBorder;
}

/** Sprites() ************************************************/
void Sprites(register byte Y, register pixel *Line) {
    static const byte SprHeights[4] = {8, 16, 16, 32};
    register pixel *restrict P, C;
    register byte OH, IH, *PT, *AT;
    register unsigned int M;
    register int L, K;

    VDPStatus[0] &= ~0x5F;
    OH = SprHeights[VDP[1] & 0x03];
    IH = SprHeights[VDP[1] & 0x02];
    AT = SprTab - 4;
    Y += VScroll;
    C = MAXSPRITE1 + 1;
    M = 0;

    for (L = 0; L < 32; ++L) {
        M <<= 1;
        AT += 4;
        K = AT[0];
        if (__builtin_expect(K == 208, 0)) {
            break;
        }
        if (K > 256 - IH) {
            K -= 256;
        }
        if ((Y > K) && (Y <= K + OH)) {
            if (!--C) {
                VDPStatus[0] |= 0x40;
                if (!OPTION(MSX_ALLSPRITE)) {
                    break;
                }
            }
            M |= 1;
        }
    }

    VDPStatus[0] |= L < 32 ? L : 31;

    for (; M; M >>= 1, AT -= 4)
        if (M & 1) {
            C = AT[3];
            L = C & 0x80 ? AT[1] - 32 : AT[1];
            C &= 0x0F;
            if ((L < 256) && (L > -OH) && C) {
                K = AT[0];
                if (K > 256 - IH) {
                    K -= 256;
                }
                P = Line + L;
                K = Y - K - 1;
                PT = SprGen + ((int)(IH > 8 ? AT[2] & 0xFC : AT[2]) << 3)
                     + (OH > IH ? (K >> 1) : K);
                C = XPal[C];
                K = L >= 0 ? 0xFFFF
                           : (0x10000 >> (OH > IH ? (-L >> 1) : -L)) - 1;
                L += (int)OH - 257;
                if (L >= 0) {
                    L = (IH > 8 ? 0x0002 : 0x0200) << (OH > IH ? (L >> 1) : L);
                    K &= ~(L - 1);
                }
                K &= ((int)PT[0] << 8) | (IH > 8 ? PT[16] : 0x00);

                if (OH > IH) {
                    if (K & 0xFF00) {
                        if (K & 0x8000) { P[1] = P[0] = C; }
                        if (K & 0x4000) { P[3] = P[2] = C; }
                        if (K & 0x2000) { P[5] = P[4] = C; }
                        if (K & 0x1000) { P[7] = P[6] = C; }
                        if (K & 0x0800) { P[9] = P[8] = C; }
                        if (K & 0x0400) { P[11] = P[10] = C; }
                        if (K & 0x0200) { P[13] = P[12] = C; }
                        if (K & 0x0100) { P[15] = P[14] = C; }
                    }
                    if (K & 0x00FF) {
                        if (K & 0x0080) { P[17] = P[16] = C; }
                        if (K & 0x0040) { P[19] = P[18] = C; }
                        if (K & 0x0020) { P[21] = P[20] = C; }
                        if (K & 0x0010) { P[23] = P[22] = C; }
                        if (K & 0x0008) { P[25] = P[24] = C; }
                        if (K & 0x0004) { P[27] = P[26] = C; }
                        if (K & 0x0002) { P[29] = P[28] = C; }
                        if (K & 0x0001) { P[31] = P[30] = C; }
                    }
                } else {
                    if (K & 0xFF00) {
                        if (K & 0x8000) { P[0] = C; }
                        if (K & 0x4000) { P[1] = C; }
                        if (K & 0x2000) { P[2] = C; }
                        if (K & 0x1000) { P[3] = C; }
                        if (K & 0x0800) { P[4] = C; }
                        if (K & 0x0400) { P[5] = C; }
                        if (K & 0x0200) { P[6] = C; }
                        if (K & 0x0100) { P[7] = C; }
                    }
                    if (K & 0x00FF) {
                        if (K & 0x0080) { P[8] = C; }
                        if (K & 0x0040) { P[9] = C; }
                        if (K & 0x0020) { P[10] = C; }
                        if (K & 0x0010) { P[11] = C; }
                        if (K & 0x0008) { P[12] = C; }
                        if (K & 0x0004) { P[13] = C; }
                        if (K & 0x0002) { P[14] = C; }
                        if (K & 0x0001) { P[15] = C; }
                    }
                }
            }
        }
}

/** ColorSprites() *******************************************/
IRAM_ATTR void ColorSprites(register byte Y, byte *ZBuf) {
    static const byte SprHeights[4] = {8, 16, 16, 32};
    register byte C, IH, OH, J, OrThem;
    register byte *P, *PT, *AT;
    register int L, K;
    register unsigned int M;

    VDPStatus[0] &= ~0x5F;
    memset(ZBuf + 32, 0, 256);

    scanline_has_sprites = false;

    if (__builtin_expect(SpritesOFF, 0)) {
        return;
    }

    OrThem = 0x00;
    OH = SprHeights[VDP[1] & 0x03];
    IH = SprHeights[VDP[1] & 0x02];
    AT = SprTab - 4;
    C = MAXSPRITE2 + 1;
    M = 0;

    for (L = 0; L < 32; ++L) {
        M <<= 1;
        AT += 4;
        K = AT[0];
        if (__builtin_expect(K == 216, 0)) {
            break;
        }
        K = (byte)(K - VScroll);
        if (K > 256 - IH) {
            K -= 256;
        }
        if ((Y > K) && (Y <= K + OH)) {
            if (!--C) {
                VDPStatus[0] |= 0x40;
                if (!OPTION(MSX_ALLSPRITE)) {
                    break;
                }
            }
            M |= 1;
        }
    }

    VDPStatus[0] |= L < 32 ? L : 31;

    for (; M; M >>= 1, AT -= 4)
        if (M & 1) {
            K = (byte)(AT[0] - VScroll);
            if (K > 256 - IH) {
                K -= 256;
            }
            J = Y - K - 1;
            J = OH > IH ? (J >> 1) : J;
            C = SprTab[-0x0200 + ((AT - SprTab) << 2) + J];
            OrThem |= C & 0x40;

            if (__builtin_expect(C & 0x0F, 1)) {
                scanline_has_sprites = true;

                PT = SprGen + ((int)(IH > 8 ? AT[2] & 0xFC : AT[2]) << 3) + J;
                P = ZBuf + AT[1] + (C & 0x80 ? 0 : 32);
                C &= 0x0F;
                J = PT[0];

                if (OrThem & 0x20) {
                    if (OH > IH) {
                        if (J & 0x80) { P[0] |= C; P[1] |= C; }
                        if (J & 0x40) { P[2] |= C; P[3] |= C; }
                        if (J & 0x20) { P[4] |= C; P[5] |= C; }
                        if (J & 0x10) { P[6] |= C; P[7] |= C; }
                        if (J & 0x08) { P[8] |= C; P[9] |= C; }
                        if (J & 0x04) { P[10] |= C; P[11] |= C; }
                        if (J & 0x02) { P[12] |= C; P[13] |= C; }
                        if (J & 0x01) { P[14] |= C; P[15] |= C; }
                        if (IH > 8) { J = PT[16];
                            if (J & 0x80) { P[16] |= C; P[17] |= C; }
                            if (J & 0x40) { P[18] |= C; P[19] |= C; }
                            if (J & 0x20) { P[20] |= C; P[21] |= C; }
                            if (J & 0x10) { P[22] |= C; P[23] |= C; }
                            if (J & 0x08) { P[24] |= C; P[25] |= C; }
                            if (J & 0x04) { P[26] |= C; P[27] |= C; }
                            if (J & 0x02) { P[28] |= C; P[29] |= C; }
                            if (J & 0x01) { P[30] |= C; P[31] |= C; }
                        }
                    } else {
                        if (J & 0x80) { P[0] |= C; }
                        if (J & 0x40) { P[1] |= C; }
                        if (J & 0x20) { P[2] |= C; }
                        if (J & 0x10) { P[3] |= C; }
                        if (J & 0x08) { P[4] |= C; }
                        if (J & 0x04) { P[5] |= C; }
                        if (J & 0x02) { P[6] |= C; }
                        if (J & 0x01) { P[7] |= C; }
                        if (IH > 8) { J = PT[16];
                            if (J & 0x80) { P[8] |= C; }
                            if (J & 0x40) { P[9] |= C; }
                            if (J & 0x20) { P[10] |= C; }
                            if (J & 0x10) { P[11] |= C; }
                            if (J & 0x08) { P[12] |= C; }
                            if (J & 0x04) { P[13] |= C; }
                            if (J & 0x02) { P[14] |= C; }
                            if (J & 0x01) { P[15] |= C; }
                        }
                    }
                } else {
                    if (__builtin_expect(J == 0xFF && OH <= IH && IH == 8, 0)) {
                        P[0] = C; P[1] = C; P[2] = C; P[3] = C;
                        P[4] = C; P[5] = C; P[6] = C; P[7] = C;
                    } else if (__builtin_expect(J == 0x00 && OH <= IH && IH == 8, 0)) {
                    } else if (OH > IH) {
                        if (J & 0x80) { P[0] = P[1] = C; }
                        if (J & 0x40) { P[2] = P[3] = C; }
                        if (J & 0x20) { P[4] = P[5] = C; }
                        if (J & 0x10) { P[6] = P[7] = C; }
                        if (J & 0x08) { P[8] = P[9] = C; }
                        if (J & 0x04) { P[10] = P[11] = C; }
                        if (J & 0x02) { P[12] = P[13] = C; }
                        if (J & 0x01) { P[14] = P[15] = C; }
                        if (IH > 8) { J = PT[16];
                            if (J & 0x80) { P[16] = P[17] = C; }
                            if (J & 0x40) { P[18] = P[19] = C; }
                            if (J & 0x20) { P[20] = P[21] = C; }
                            if (J & 0x10) { P[22] = P[23] = C; }
                            if (J & 0x08) { P[24] = P[25] = C; }
                            if (J & 0x04) { P[26] = P[27] = C; }
                            if (J & 0x02) { P[28] = P[29] = C; }
                            if (J & 0x01) { P[30] = P[31] = C; }
                        }
                    } else {
                        if (J & 0x80) { P[0] = C; }
                        if (J & 0x40) { P[1] = C; }
                        if (J & 0x20) { P[2] = C; }
                        if (J & 0x10) { P[3] = C; }
                        if (J & 0x08) { P[4] = C; }
                        if (J & 0x04) { P[5] = C; }
                        if (J & 0x02) { P[6] = C; }
                        if (J & 0x01) { P[7] = C; }
                        if (IH > 8) { J = PT[16];
                            if (J & 0x80) { P[8] = C; }
                            if (J & 0x40) { P[9] = C; }
                            if (J & 0x20) { P[10] = C; }
                            if (J & 0x10) { P[11] = C; }
                            if (J & 0x08) { P[12] = C; }
                            if (J & 0x04) { P[13] = C; }
                            if (J & 0x02) { P[14] = C; }
                            if (J & 0x01) { P[15] = C; }
                        }
                    }
                }
            }
            OrThem >>= 1;
        }
}

/** RefreshLineF() *******************************************/
void RefreshLineF(register byte Y) {
    register pixel *restrict P;
    P = RefreshBorder(Y, XPal[BGColor]);
    if (P) {
        ClearLine(P, XPal[BGColor]);
    }
}

/** RefreshLine0() *******************************************/
void RefreshLine0(register byte Y) {
    msx_active_width = 256;
    register pixel *restrict P, FC, BC;
    register byte X, *T, *G;

    BC = XPal[BGColor];
    P = RefreshBorder(Y, BC);
    if (!P) { return; }
    if (__builtin_expect(!ScreenON, 0)) { ClearLine(P, BC); }
    else {
        P[0] = P[1] = P[2] = P[3] = P[4] = P[5] = P[6] = P[7] = P[8] = BC;
        G = (FontBuf && (Mode & MSX_FIXEDFONT) ? FontBuf : ChrGen)
            + ((Y + VScroll) & 0x07);
        T = ChrTab + 40 * (Y >> 3);
        FC = XPal[FGColor];
        P += 9;
        for (X = 0; X < 40; X++, T++, P += 6) {
            Y = G[(int)*T << 3];
            P[0] = Y & 0x80 ? FC : BC; P[1] = Y & 0x40 ? FC : BC;
            P[2] = Y & 0x20 ? FC : BC; P[3] = Y & 0x10 ? FC : BC;
            P[4] = Y & 0x08 ? FC : BC; P[5] = Y & 0x04 ? FC : BC;
        }
        P[0] = P[1] = P[2] = P[3] = P[4] = P[5] = P[6] = BC;
    }
}

/** RefreshLine1() *******************************************/
void RefreshLine1(register byte Y) {
    msx_active_width = 256;
    register pixel *restrict P, FC, BC;
    register byte K, X, *T, *G;

    P = RefreshBorder(Y, XPal[BGColor]);
    if (!P) { return; }
    if (__builtin_expect(!ScreenON, 0)) { ClearLine(P, XPal[BGColor]); }
    else {
        Y += VScroll;
        G = (FontBuf && (Mode & MSX_FIXEDFONT) ? FontBuf : ChrGen) + (Y & 0x07);
        T = ChrTab + ((int)(Y & 0xF8) << 2);
        for (X = 0; X < 32; X++, T++, P += 8) {
            K = ColTab[*T >> 3]; FC = XPal[K >> 4]; BC = XPal[K & 0x0F];
            K = G[(int)*T << 3];
            P[0] = K & 0x80 ? FC : BC; P[1] = K & 0x40 ? FC : BC;
            P[2] = K & 0x20 ? FC : BC; P[3] = K & 0x10 ? FC : BC;
            P[4] = K & 0x08 ? FC : BC; P[5] = K & 0x04 ? FC : BC;
            P[6] = K & 0x02 ? FC : BC; P[7] = K & 0x01 ? FC : BC;
        }
        if (__builtin_expect(!SpritesOFF, 1)) { Sprites(Y, P - 256); }
    }
}

/** RefreshLine2() *******************************************/
void RefreshLine2(register byte Y) {
    msx_active_width = 256;
    register pixel *restrict P, FC, BC;
    register byte K, X, *T;
    register int I, J;

    P = RefreshBorder(Y, XPal[BGColor]);
    if (!P) { return; }
    if (__builtin_expect(!ScreenON, 0)) { ClearLine(P, XPal[BGColor]); }
    else {
        Y += VScroll;
        T = ChrTab + ((int)(Y & 0xF8) << 2);
        I = ((int)(Y & 0xC0) << 5) + (Y & 0x07);
        for (X = 0; X < 32; X++, T++, P += 8) {
            J = (int)*T << 3; K = ColTab[(I + J) & ColTabM];
            FC = XPal[K >> 4]; BC = XPal[K & 0x0F];
            K = ChrGen[(I + J) & ChrGenM];
            P[0] = K & 0x80 ? FC : BC; P[1] = K & 0x40 ? FC : BC;
            P[2] = K & 0x20 ? FC : BC; P[3] = K & 0x10 ? FC : BC;
            P[4] = K & 0x08 ? FC : BC; P[5] = K & 0x04 ? FC : BC;
            P[6] = K & 0x02 ? FC : BC; P[7] = K & 0x01 ? FC : BC;
        }
        if (__builtin_expect(!SpritesOFF, 1)) { Sprites(Y, P - 256); }
    }
}

/** RefreshLine3() *******************************************/
void RefreshLine3(register byte Y) {
    msx_active_width = 256;
    register pixel *restrict P;
    register byte X, K, *T, *G;

    P = RefreshBorder(Y, XPal[BGColor]);
    if (!P) { return; }
    if (__builtin_expect(!ScreenON, 0)) { ClearLine(P, XPal[BGColor]); }
    else {
        Y += VScroll;
        T = ChrTab + ((int)(Y & 0xF8) << 2);
        G = ChrGen + ((Y & 0x1C) >> 2);
        for (X = 0; X < 32; X++, T++, P += 8) {
            K = G[(int)*T << 3];
            P[0] = P[1] = P[2] = P[3] = XPal[K >> 4];
            P[4] = P[5] = P[6] = P[7] = XPal[K & 0x0F];
        }
        if (__builtin_expect(!SpritesOFF, 1)) { Sprites(Y, P - 256); }
    }
}

/** RefreshLine4() *******************************************/
IRAM_ATTR void RefreshLine4(register byte Y) {
    msx_active_width = 256;
    register pixel *restrict P, FC, BC;
    register byte K, X, C, *T, *R;
    register int I, J;
    byte ZBuf[320] __attribute__((aligned(4)));

    P = RefreshBorder(Y, XPal[BGColor]);
    if (!P) { return; }
    if (__builtin_expect(!ScreenON, 0)) { ClearLine(P, XPal[BGColor]); }
    else {
        ColorSprites(Y, ZBuf); R = ZBuf + 32;
        Y += VScroll;
        T = ChrTab + ((int)(Y & 0xF8) << 2);
        I = ((int)(Y & 0xC0) << 5) + (Y & 0x07);
        for (X = 0; X < 32; X++, R += 8, P += 8, T++) {
            J = (int)*T << 3; K = ColTab[(I + J) & ColTabM];
            FC = XPal[K >> 4]; BC = XPal[K & 0x0F];
            K = ChrGen[(I + J) & ChrGenM];
            C = R[0]; P[0] = C ? XPal[C] : (K & 0x80) ? FC : BC;
            C = R[1]; P[1] = C ? XPal[C] : (K & 0x40) ? FC : BC;
            C = R[2]; P[2] = C ? XPal[C] : (K & 0x20) ? FC : BC;
            C = R[3]; P[3] = C ? XPal[C] : (K & 0x10) ? FC : BC;
            C = R[4]; P[4] = C ? XPal[C] : (K & 0x08) ? FC : BC;
            C = R[5]; P[5] = C ? XPal[C] : (K & 0x04) ? FC : BC;
            C = R[6]; P[6] = C ? XPal[C] : (K & 0x02) ? FC : BC;
            C = R[7]; P[7] = C ? XPal[C] : (K & 0x01) ? FC : BC;
        }
    }
}

/** RefreshLine5() *******************************************
 ** SC5_LUT + REGISTER CACHE + SAFE VRAM READS + UB FIX     **/
IRAM_ATTR void RefreshLine5(register byte Y) {
    msx_active_width = 256;
    register pixel *restrict P;
    register byte X, *T, *R;
    byte ZBuf[320] __attribute__((aligned(4)));

    P = RefreshBorder(Y, XPal[BGColor]);
    if (!P) { return; }
    if (__builtin_expect(!ScreenON, 0)) { ClearLine(P, XPal[BGColor]); return; }

    if (__builtin_expect(
            msx_palette_dirty || !sc5_lut_ready || XPal[0] != last_xpal0, 0)) {
        last_xpal0 = XPal[0];
        SyncPalette();
    }

    T = ChrTab + (((int)(Y + VScroll) << 7) & ChrTabM & 0x7FFF);
    ColorSprites(Y, ZBuf);
    R = ZBuf + 32;

    for (X = 0; X < 16; X++, R += 16, P += 16, T += 8) {
        uint32_t *R32 = (uint32_t *)R;

        uint32_t t_low = (uint32_t)T[0] | ((uint32_t)T[1] << 8)
                         | ((uint32_t)T[2] << 16) | ((uint32_t)T[3] << 24);
        uint32_t t_high = (uint32_t)T[4] | ((uint32_t)T[5] << 8)
                          | ((uint32_t)T[6] << 16) | ((uint32_t)T[7] << 24);

        uint32_t r0_3 = R32[0];
        uint32_t r4_7 = R32[1];
        uint32_t r8_11 = R32[2];
        uint32_t r12_15 = R32[3];

        if ((r0_3 | r4_7 | r8_11 | r12_15) == 0) {
            WRITE_P(0, SC5_LUT[t_low & 0xFF]);
            WRITE_P(2, SC5_LUT[(t_low >> 8) & 0xFF]);
            WRITE_P(4, SC5_LUT[(t_low >> 16) & 0xFF]);
            WRITE_P(6, SC5_LUT[t_low >> 24]);
            WRITE_P(8, SC5_LUT[t_high & 0xFF]);
            WRITE_P(10, SC5_LUT[(t_high >> 8) & 0xFF]);
            WRITE_P(12, SC5_LUT[(t_high >> 16) & 0xFF]);
            WRITE_P(14, SC5_LUT[t_high >> 24]);
        } else if ((r0_3 | r4_7) == 0) {
            WRITE_P(0, SC5_LUT[t_low & 0xFF]);
            WRITE_P(2, SC5_LUT[(t_low >> 8) & 0xFF]);
            WRITE_P(4, SC5_LUT[(t_low >> 16) & 0xFF]);
            WRITE_P(6, SC5_LUT[t_low >> 24]);
            uint32_t bg4 = SC5_LUT[t_high & 0xFF];
            uint32_t s8 = r8_11 & 0xFF, s9 = (r8_11 >> 8) & 0xFF;
            WRITE_P(8, (s8 ? XPal[s8] : (bg4 & 0xFFFF))
                    | (s9 ? ((uint32_t)XPal[s9] << 16) : (bg4 & 0xFFFF0000)));
            uint32_t bg5 = SC5_LUT[(t_high >> 8) & 0xFF];
            uint32_t s10 = (r8_11 >> 16) & 0xFF, s11 = r8_11 >> 24;
            WRITE_P(10, (s10 ? XPal[s10] : (bg5 & 0xFFFF))
                    | (s11 ? ((uint32_t)XPal[s11] << 16) : (bg5 & 0xFFFF0000)));
            uint32_t bg6 = SC5_LUT[(t_high >> 16) & 0xFF];
            uint32_t s12 = r12_15 & 0xFF, s13 = (r12_15 >> 8) & 0xFF;
            WRITE_P(12, (s12 ? XPal[s12] : (bg6 & 0xFFFF))
                    | (s13 ? ((uint32_t)XPal[s13] << 16) : (bg6 & 0xFFFF0000)));
            uint32_t bg7 = SC5_LUT[t_high >> 24];
            uint32_t s14 = (r12_15 >> 16) & 0xFF, s15 = r12_15 >> 24;
            WRITE_P(14, (s14 ? XPal[s14] : (bg7 & 0xFFFF))
                    | (s15 ? ((uint32_t)XPal[s15] << 16) : (bg7 & 0xFFFF0000)));
        } else if ((r8_11 | r12_15) == 0) {
            uint32_t bg0 = SC5_LUT[t_low & 0xFF];
            uint32_t s0 = r0_3 & 0xFF, s1 = (r0_3 >> 8) & 0xFF;
            WRITE_P(0, (s0 ? XPal[s0] : (bg0 & 0xFFFF))
                    | (s1 ? ((uint32_t)XPal[s1] << 16) : (bg0 & 0xFFFF0000)));
            uint32_t bg1 = SC5_LUT[(t_low >> 8) & 0xFF];
            uint32_t s2 = (r0_3 >> 16) & 0xFF, s3 = r0_3 >> 24;
            WRITE_P(2, (s2 ? XPal[s2] : (bg1 & 0xFFFF))
                    | (s3 ? ((uint32_t)XPal[s3] << 16) : (bg1 & 0xFFFF0000)));
            uint32_t bg2 = SC5_LUT[(t_low >> 16) & 0xFF];
            uint32_t s4 = r4_7 & 0xFF, s5 = (r4_7 >> 8) & 0xFF;
            WRITE_P(4, (s4 ? XPal[s4] : (bg2 & 0xFFFF))
                    | (s5 ? ((uint32_t)XPal[s5] << 16) : (bg2 & 0xFFFF0000)));
            uint32_t bg3 = SC5_LUT[t_low >> 24];
            uint32_t s6 = (r4_7 >> 16) & 0xFF, s7 = r4_7 >> 24;
            WRITE_P(6, (s6 ? XPal[s6] : (bg3 & 0xFFFF))
                    | (s7 ? ((uint32_t)XPal[s7] << 16) : (bg3 & 0xFFFF0000)));
            WRITE_P(8, SC5_LUT[t_high & 0xFF]);
            WRITE_P(10, SC5_LUT[(t_high >> 8) & 0xFF]);
            WRITE_P(12, SC5_LUT[(t_high >> 16) & 0xFF]);
            WRITE_P(14, SC5_LUT[t_high >> 24]);
        } else {
            uint32_t bg0 = SC5_LUT[t_low & 0xFF];
            uint32_t s0 = r0_3 & 0xFF, s1 = (r0_3 >> 8) & 0xFF;
            WRITE_P(0, (s0 ? XPal[s0] : (bg0 & 0xFFFF))
                    | (s1 ? ((uint32_t)XPal[s1] << 16) : (bg0 & 0xFFFF0000)));
            uint32_t bg1 = SC5_LUT[(t_low >> 8) & 0xFF];
            uint32_t s2 = (r0_3 >> 16) & 0xFF, s3 = r0_3 >> 24;
            WRITE_P(2, (s2 ? XPal[s2] : (bg1 & 0xFFFF))
                    | (s3 ? ((uint32_t)XPal[s3] << 16) : (bg1 & 0xFFFF0000)));
            uint32_t bg2 = SC5_LUT[(t_low >> 16) & 0xFF];
            uint32_t s4 = r4_7 & 0xFF, s5 = (r4_7 >> 8) & 0xFF;
            WRITE_P(4, (s4 ? XPal[s4] : (bg2 & 0xFFFF))
                    | (s5 ? ((uint32_t)XPal[s5] << 16) : (bg2 & 0xFFFF0000)));
            uint32_t bg3 = SC5_LUT[t_low >> 24];
            uint32_t s6 = (r4_7 >> 16) & 0xFF, s7 = r4_7 >> 24;
            WRITE_P(6, (s6 ? XPal[s6] : (bg3 & 0xFFFF))
                    | (s7 ? ((uint32_t)XPal[s7] << 16) : (bg3 & 0xFFFF0000)));
            uint32_t bg4 = SC5_LUT[t_high & 0xFF];
            uint32_t s8 = r8_11 & 0xFF, s9 = (r8_11 >> 8) & 0xFF;
            WRITE_P(8, (s8 ? XPal[s8] : (bg4 & 0xFFFF))
                    | (s9 ? ((uint32_t)XPal[s9] << 16) : (bg4 & 0xFFFF0000)));
            uint32_t bg5 = SC5_LUT[(t_high >> 8) & 0xFF];
            uint32_t s10 = (r8_11 >> 16) & 0xFF, s11 = r8_11 >> 24;
            WRITE_P(10, (s10 ? XPal[s10] : (bg5 & 0xFFFF))
                    | (s11 ? ((uint32_t)XPal[s11] << 16) : (bg5 & 0xFFFF0000)));
            uint32_t bg6 = SC5_LUT[(t_high >> 16) & 0xFF];
            uint32_t s12 = r12_15 & 0xFF, s13 = (r12_15 >> 8) & 0xFF;
            WRITE_P(12, (s12 ? XPal[s12] : (bg6 & 0xFFFF))
                    | (s13 ? ((uint32_t)XPal[s13] << 16) : (bg6 & 0xFFFF0000)));
            uint32_t bg7 = SC5_LUT[t_high >> 24];
            uint32_t s14 = (r12_15 >> 16) & 0xFF, s15 = r12_15 >> 24;
            WRITE_P(14, (s14 ? XPal[s14] : (bg7 & 0xFFFF))
                    | (s15 ? ((uint32_t)XPal[s15] << 16) : (bg7 & 0xFFFF0000)));
        }
    }
}

/** RefreshLine8() *******************************************
 ** LUT Fusion + REGISTER-CACHED VRAM + SAFE VRAM READS      **/
IRAM_ATTR void RefreshLine8(register byte Y) {
    msx_active_width = 256;

    register pixel *restrict P;
    register byte X, *T, *R;
    byte ZBuf[320] __attribute__((aligned(4)));

    P = RefreshBorder(Y, BPal[VDP[7]]);
    if (!P) { return; }
    if (__builtin_expect(!ScreenON, 0)) { ClearLine(P, BPal[VDP[7]]); return; }

    if (__builtin_expect(
            msx_palette_dirty || !spr_pal_ready || XPal[0] != last_xpal0, 0)) {
        last_xpal0 = XPal[0];
        SyncPalette();
    }

    ColorSprites(Y, ZBuf);
    R = ZBuf + 32;
    T = ChrTab + (((int)(Y + VScroll) << 8) & ChrTabM & 0xFFFF);

    for (X = 0; X < 32; X++, T += 8, R += 8, P += 8) {
        uint32_t *R32 = (uint32_t *)R;

        uint32_t t_low = (uint32_t)T[0] | ((uint32_t)T[1] << 8)
                         | ((uint32_t)T[2] << 16) | ((uint32_t)T[3] << 24);
        uint32_t t_high = (uint32_t)T[4] | ((uint32_t)T[5] << 8)
                          | ((uint32_t)T[6] << 16) | ((uint32_t)T[7] << 24);

        if ((R32[0] | R32[1]) == 0) {
            uint32_t solid_pattern = (t_low & 0xFF) * 0x01010101u;
            if (__builtin_expect(t_low == solid_pattern && t_high == solid_pattern, 0)) {
                uint32_t color32 =
                    BPal[t_low & 0xFF] | ((uint32_t)BPal[t_low & 0xFF] << 16);
                WRITE_P(0, color32); WRITE_P(2, color32);
                WRITE_P(4, color32); WRITE_P(6, color32);
            } else {
                WRITE_P(0, BPal[t_low & 0xFF]
                        | ((uint32_t)BPal[(t_low >> 8) & 0xFF] << 16));
                WRITE_P(2, BPal[(t_low >> 16) & 0xFF]
                        | ((uint32_t)BPal[t_low >> 24] << 16));
                WRITE_P(4, BPal[t_high & 0xFF]
                        | ((uint32_t)BPal[(t_high >> 8) & 0xFF] << 16));
                WRITE_P(6, BPal[(t_high >> 16) & 0xFF]
                        | ((uint32_t)BPal[t_high >> 24] << 16));
            }
        } else {
            P[0] = R[0] ? SprPal[R[0]] : BPal[t_low & 0xFF];
            P[1] = R[1] ? SprPal[R[1]] : BPal[(t_low >> 8) & 0xFF];
            P[2] = R[2] ? SprPal[R[2]] : BPal[(t_low >> 16) & 0xFF];
            P[3] = R[3] ? SprPal[R[3]] : BPal[t_low >> 24];
            P[4] = R[4] ? SprPal[R[4]] : BPal[t_high & 0xFF];
            P[5] = R[5] ? SprPal[R[5]] : BPal[(t_high >> 8) & 0xFF];
            P[6] = R[6] ? SprPal[R[6]] : BPal[(t_high >> 16) & 0xFF];
            P[7] = R[7] ? SprPal[R[7]] : BPal[t_high >> 24];
        }
    }
}

/** RefreshLine10() ******************************************/
IRAM_ATTR void RefreshLine10(register byte Y) {
    msx_active_width = 256;
    register pixel *restrict P;
    register byte C, X, *T, *R;
    byte ZBuf[320] __attribute__((aligned(4)));

    P = RefreshBorder(Y, BPal[VDP[7]]);
    if (!P) { return; }
    if (__builtin_expect(!ScreenON, 0)) { ClearLine(P, BPal[VDP[7]]); return; }

    if (__builtin_expect(
            msx_palette_dirty || !spr_pal_ready || XPal[0] != last_xpal0, 0)) {
        last_xpal0 = XPal[0];
        SyncPalette();
    }

    ColorSprites(Y, ZBuf); R = ZBuf + 32;
    T = ChrTab + (((int)(Y + VScroll) << 8) & ChrTabM & 0xFFFF);
    C = R[0]; P[0] = C ? XPal[C] : BPal[VDP[7]];
    C = R[1]; P[1] = C ? XPal[C] : BPal[VDP[7]];
    C = R[2]; P[2] = C ? XPal[C] : BPal[VDP[7]];
    C = R[3]; P[3] = C ? XPal[C] : BPal[VDP[7]];
    R += 4; P += 4;
    for (X = 0; X < 63; X++, T += 4, R += 4, P += 4) {
        uint32_t t_val = (uint32_t)T[0] | ((uint32_t)T[1] << 8)
                         | ((uint32_t)T[2] << 16) | ((uint32_t)T[3] << 24);
        uint32_t r_val = (uint32_t)R[0] | ((uint32_t)R[1] << 8)
                         | ((uint32_t)R[2] << 16) | ((uint32_t)R[3] << 24);

        int K = (t_val & 0x07) | ((t_val >> 5) & 0x38);
        K = (K ^ 0x20) - 0x20;
        int J = ((t_val >> 16) & 0x07) | ((t_val >> 21) & 0x38);
        J = (J ^ 0x20) - 0x20;
        int b_off = -(J << 1) - K;

        byte y0 = (t_val >> 3) & 0x1F;
        byte y1 = (t_val >> 11) & 0x1F;
        byte y2 = (t_val >> 19) & 0x1F;
        byte y3 = (t_val >> 27) & 0x1F;

        byte r0 = r_val & 0xFF;
        byte r1 = (r_val >> 8) & 0xFF;
        byte r2 = (r_val >> 16) & 0xFF;
        byte r3 = (r_val >> 24) & 0xFF;

        P[0] = r0 ? XPal[r0] : y0 & 1 ? XPal[y0 >> 1] : YJKColor(y0, J, K, b_off);
        P[1] = r1 ? XPal[r1] : y1 & 1 ? XPal[y1 >> 1] : YJKColor(y1, J, K, b_off);
        P[2] = r2 ? XPal[r2] : y2 & 1 ? XPal[y2 >> 1] : YJKColor(y2, J, K, b_off);
        P[3] = r3 ? XPal[r3] : y3 & 1 ? XPal[y3 >> 1] : YJKColor(y3, J, K, b_off);
    }
}

/** RefreshLine12() ******************************************/
IRAM_ATTR void RefreshLine12(register byte Y) {
    msx_active_width = 256;
    register pixel *restrict P;
    register byte C, X, *T, *R;
    byte ZBuf[320] __attribute__((aligned(4)));

    P = RefreshBorder(Y, BPal[VDP[7]]);
    if (!P) { return; }
    if (__builtin_expect(!ScreenON, 0)) { ClearLine(P, BPal[VDP[7]]); return; }

    if (__builtin_expect(
            msx_palette_dirty || !spr_pal_ready || XPal[0] != last_xpal0, 0)) {
        last_xpal0 = XPal[0];
        SyncPalette();
    }

    ColorSprites(Y, ZBuf); R = ZBuf + 32;
    T = ChrTab + (((int)(Y + VScroll) << 8) & ChrTabM & 0xFFFF)
        + (HScroll512 && (HScroll > 255) ? 0x10000 : 0) + (HScroll & 0xFC);

    C = R[0]; P[0] = C ? XPal[C] : BPal[VDP[7]];
    C = R[1]; P[1] = C ? XPal[C] : BPal[VDP[7]];
    C = R[2]; P[2] = C ? XPal[C] : BPal[VDP[7]];
    C = R[3]; P[3] = C ? XPal[C] : BPal[VDP[7]];
    R += 4; P += 4;

    for (X = 1; X < 64; X++, T += 4, R += 4, P += 4) {
        uint32_t t_val = (uint32_t)T[0] | ((uint32_t)T[1] << 8)
                         | ((uint32_t)T[2] << 16) | ((uint32_t)T[3] << 24);
        uint32_t r_val = (uint32_t)R[0] | ((uint32_t)R[1] << 8)
                         | ((uint32_t)R[2] << 16) | ((uint32_t)R[3] << 24);

        int K = (t_val & 0x07) | ((t_val >> 5) & 0x38);
        K = (K ^ 0x20) - 0x20;
        int J = ((t_val >> 16) & 0x07) | ((t_val >> 21) & 0x38);
        J = (J ^ 0x20) - 0x20;
        int b_off = -(J << 1) - K;

        P[0] = (r_val & 0xFF) ? XPal[r_val & 0xFF] : YJKColor((t_val >> 3) & 0x1F, J, K, b_off);
        P[1] = ((r_val >> 8) & 0xFF) ? XPal[(r_val >> 8) & 0xFF] : YJKColor((t_val >> 11) & 0x1F, J, K, b_off);
        P[2] = ((r_val >> 16) & 0xFF) ? XPal[(r_val >> 16) & 0xFF] : YJKColor((t_val >> 19) & 0x1F, J, K, b_off);
        P[3] = ((r_val >> 24) & 0xFF) ? XPal[(r_val >> 24) & 0xFF] : YJKColor((t_val >> 27) & 0x1F, J, K, b_off);
    }
}

#ifdef NARROW

/** RefreshLine6() *******************************************/
void RefreshLine6(register byte Y) {
    msx_active_width = 512;
    register pixel *restrict P;
    register byte X, *T, *R, C;
    byte ZBuf[320] __attribute__((aligned(4)));

    P = RefreshBorder(Y, XPal[BGColor & 0x03]);
    if (!P) { return; }
    if (__builtin_expect(!ScreenON, 0)) { ClearLine(P, XPal[BGColor & 0x03]); }
    else {
        ColorSprites(Y, ZBuf); R = ZBuf + 32;
        T = ChrTab + (((int)(Y + VScroll) << 7) & ChrTabM & 0x7FFF);
        for (X = 0; X < 64; X++) {
            C = R[0]; P[0] = XPal[C ? C : T[0] >> 6];
            C = R[0]; P[1] = XPal[C ? C : (T[0] >> 4) & 0x03];
            C = R[1]; P[2] = XPal[C ? C : (T[0] >> 2) & 0x03];
            C = R[1]; P[3] = XPal[C ? C : T[0] & 0x03];
            C = R[2]; P[4] = XPal[C ? C : T[1] >> 6];
            C = R[2]; P[5] = XPal[C ? C : (T[1] >> 4) & 0x03];
            C = R[3]; P[6] = XPal[C ? C : (T[1] >> 2) & 0x03];
            C = R[3]; P[7] = XPal[C ? C : T[1] & 0x03];
            R += 4; P += 8; T += 2;
        }
    }
}

/** RefreshLine7() *******************************************/
void RefreshLine7(register byte Y) {
    msx_active_width = 512;
    register pixel *restrict P;
    register byte C, X, *T, *R;
    byte ZBuf[320] __attribute__((aligned(4)));

    P = RefreshBorder(Y, XPal[BGColor]);
    if (!P) { return; }
    if (__builtin_expect(!ScreenON, 0)) { ClearLine(P, XPal[BGColor]); }
    else {
        ColorSprites(Y, ZBuf); R = ZBuf + 32;
        T = ChrTab + (((int)(Y + VScroll) << 8) & ChrTabM & 0xFFFF);
        for (X = 0; X < 64; X++) {
            C = R[0]; P[0] = XPal[C ? C : T[0] >> 4];
            C = R[0]; P[1] = XPal[C ? C : T[0] & 0x0F];
            C = R[1]; P[2] = XPal[C ? C : T[1] >> 4];
            C = R[1]; P[3] = XPal[C ? C : T[1] & 0x0F];
            C = R[2]; P[4] = XPal[C ? C : T[2] >> 4];
            C = R[2]; P[5] = XPal[C ? C : T[2] & 0x0F];
            C = R[3]; P[6] = XPal[C ? C : T[3] >> 4];
            C = R[3]; P[7] = XPal[C ? C : T[3] & 0x0F];
            R += 4; P += 8; T += 4;
        }
    }
}

/** RefreshLineTx80() ****************************************/
void RefreshLineTx80(register byte Y) {
    msx_active_width = 512;
    register pixel *restrict P, FC, BC;
    register byte X, M, *T, *C, *G;

    BC = XPal[BGColor];
    P = RefreshBorder(Y, BC);
    if (!P) { return; }
    if (__builtin_expect(!ScreenON, 0)) { ClearLine(P, BC); }
    else {
        for (int i = 0; i < 16; i++) { P[i] = BC; }
        P += 16;
        G = (FontBuf && (Mode & MSX_FIXEDFONT) ? FontBuf : ChrGen) + ((Y + VScroll) & 0x07);
        T = ChrTab + ((80 * (Y >> 3)) & ChrTabM);
        C = ColTab + ((10 * (Y >> 3)) & ColTabM);
        for (X = 0, M = 0x00; X < 80; X++, T++, P += 6) {
            if (!(X & 0x07)) { M = *C++; }
            if (M & 0x80) { FC = XPal[XFGColor]; BC = XPal[XBGColor]; }
            else { FC = XPal[FGColor]; BC = XPal[BGColor]; }
            M <<= 1;
            Y = *(G + ((int)*T << 3));
            P[0] = Y & 0x80 ? FC : BC; P[1] = Y & 0x40 ? FC : BC;
            P[2] = Y & 0x20 ? FC : BC; P[3] = Y & 0x10 ? FC : BC;
            P[4] = Y & 0x08 ? FC : BC; P[5] = Y & 0x04 ? FC : BC;
        }
        for (int i = 0; i < 16; i++) { P[i] = BC; }
    }
}

#endif /* NARROW */
