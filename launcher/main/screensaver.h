// screensaver.h — Starfield "Through the Universe" Screensaver
// low-RAM version: 6-byte stars, no float, no stars_2d[], CHUNK=4
//
// Copyright (C) 2026 Ivan Svarkovsky
// Contact: ivansvarkovsky@gmail.com
// Distributed under CC BY-NC-SA 4.0 license.
// https://github.com/Svarkovsky

#ifndef SCREENSAVER_H
#define SCREENSAVER_H

#include "rg_system.h"
#include "rg_display.h"
#include "rg_input.h"
#include <stdlib.h>
#include <string.h>
#include <esp_task_wdt.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#ifdef __cplusplus
extern "C" {
#endif

#define STAR_COUNT  240
#define MAX_Z       2048
#define SPEED       14
#define FOCAL       384
#define ROT_SIN     1144
#define ROT_COS     65531
#define CHUNK       4

// ── Sintab Q8.8 (amplitude ±256, 256 points = one period) ──
static const int16_t sintab[256] = {
       0,    6,   13,   19,   25,   31,   38,   44,   50,   56,   62,   68,   74,   80,   86,   92,
      98,  104,  109,  115,  121,  126,  132,  137,  142,  147,  152,  157,  162,  167,  172,  177,
     181,  185,  190,  194,  198,  202,  206,  209,  213,  216,  220,  223,  226,  229,  231,  234,
     237,  239,  241,  243,  245,  247,  248,  250,  251,  252,  253,  254,  255,  255,  256,  256,
     256,  256,  256,  255,  255,  254,  253,  252,  251,  250,  248,  247,  245,  243,  241,  239,
     237,  234,  231,  229,  226,  223,  220,  216,  213,  209,  206,  202,  198,  194,  190,  185,
     181,  177,  172,  167,  162,  157,  152,  147,  142,  137,  132,  126,  121,  115,  109,  104,
      98,   92,   86,   80,   74,   68,   62,   56,   50,   44,   38,   31,   25,   19,   13,    6,
       0,   -6,  -13,  -19,  -25,  -31,  -38,  -44,  -50,  -56,  -62,  -68,  -74,  -80,  -86,  -92,
     -98, -104, -109, -115, -121, -126, -132, -137, -142, -147, -152, -157, -162, -167, -172, -177,
    -181, -185, -190, -194, -198, -202, -206, -209, -213, -216, -220, -223, -226, -229, -231, -234,
    -237, -239, -241, -243, -245, -247, -248, -250, -251, -252, -253, -254, -255, -255, -256, -256,
    -256, -256, -256, -255, -255, -254, -253, -252, -251, -250, -248, -247, -245, -243, -241, -239,
    -237, -234, -231, -229, -226, -223, -220, -216, -213, -209, -206, -202, -198, -194, -190, -185,
    -181, -177, -172, -167, -162, -157, -152, -147, -142, -137, -132, -126, -121, -115, -109, -104,
     -98,  -92,  -86,  -80,  -74,  -68,  -62,  -56,  -50,  -44,  -38,  -31,  -25,  -19,  -13,   -6,
};

// ── LCG (faster & smaller than rand(), no libc dependency) ──
static uint32_t lcg_state;

static inline uint32_t lcg_rand16(void) {
    lcg_state = lcg_state * 1103515245u + 12345u;
    return (lcg_state >> 16) & 0x7FFF;   // 15 bits
}

static inline uint32_t lcg_rand10(void) {
    return lcg_rand16() & 0x3FF;          // 10 bits 0..1023
}

// ── Star: 6 bytes (was 16) ──
// x,y  = Q4.12 fixed-point  (±512 raw → ±8192 scaled, fits int16)
// z_hue = z_real<<2 in low 14 bits, hue (0..3) in bits 14..15
typedef struct {
    int16_t  x, y;
    uint16_t z_hue;
} Star;

static inline uint16_t rgb565_pack(int r, int g, int b) {
    r = r < 0 ? 0 : (r > 255 ? 255 : r);
    g = g < 0 ? 0 : (g > 255 ? 255 : g);
    b = b < 0 ? 0 : (b > 255 ? 255 : b);
    return ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);
}

static inline uint16_t synth_color(int hue, int z) {
    int b = 255 - ((z * 225) >> 11);
    if (b < 40) b = 40;
    switch (hue & 3) {
        case 0: return rgb565_pack(b, 0, b);
        case 1: return rgb565_pack(0, b, b);
        case 2: return rgb565_pack(b >> 1, 0, b);
        default: return rgb565_pack(b, b, b);
    }
}

static inline void run_screensaver(void) {
    int w = rg_display_get_width(), h = rg_display_get_height();
    int cx = w / 2, cy = h / 2;

    // ── DRAM allocations: stars 1440 B + buf 2560 B = ~4 KB total ──
    Star *stars = (Star *)malloc(STAR_COUNT * sizeof(Star));
    uint16_t *buf = (uint16_t *)heap_caps_malloc(w * CHUNK * 2,
                                                 MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!buf) buf = (uint16_t *)malloc(w * CHUNK * 2);

    if (!stars || !buf) {
        if (stars) free(stars);
        if (buf) free(buf);
        return;
    }

    // Seed LCG from hardware timer
    lcg_state = (uint32_t)rg_system_timer();

    // Init starfield
    for (int i = 0; i < STAR_COUNT; i++) {
        int16_t  x   = (int16_t)(((int32_t)lcg_rand10() - 0x200) << 4);  // Q4.12
        int16_t  y   = (int16_t)(((int32_t)lcg_rand10() - 0x200) << 4);
        uint16_t z   = (uint16_t)(1 + (lcg_rand16() % MAX_Z));
        uint8_t  hue = (uint8_t)(lcg_rand10() & 3);
        stars[i].x    = x;
        stars[i].y    = y;
        stars[i].z_hue = (z << 2) | ((uint16_t)hue << 14);
    }

    rg_display_sync(true);
    int64_t start_time = rg_system_timer();

    while (!rg_input_read_gamepad()) {
        int64_t now = rg_system_timer();

        // ── Drift: no float, sintab Q8.8 + fixed-point scale ──
        // phase1 period ≈ 4.19 s (matches original sin(1.5·t))
        // phase2 period ≈ 8.39 s (matches original cos(0.7·t))
        uint32_t phase1 = (uint32_t)((now - start_time) >> 14);
        uint32_t phase2 = (uint32_t)((now - start_time) >> 15);
        int drift_cx = cx + (((int32_t)sintab[phase1 & 0xFF] * (w >> 3)) >> 8);
        int drift_cy = cy + (((int32_t)sintab[phase2 & 0xFF] * (h >> 3)) >> 8);

        // ── Rotate & move all stars (in-place, no stars_2d[]) ──
        for (int i = 0; i < STAR_COUNT; i++) {
            int32_t  nx = ((int32_t)stars[i].x * ROT_COS) >> 16;
            int32_t  ny = ((int32_t)stars[i].x * ROT_SIN) >> 16;
            int32_t  x2 = nx - (((int32_t)stars[i].y * ROT_SIN) >> 16);
            int32_t  y2 = ny + (((int32_t)stars[i].y * ROT_COS) >> 16);

            int16_t z_val = stars[i].z_hue & 0x3FFF;   
            int16_t nz    = z_val - (SPEED << 2);     

            if (nz <= (1 << 2)) {                     // z_real ≤ 1.0  
                x2 = ((int32_t)lcg_rand10() - 0x200) << 4;
                y2 = ((int32_t)lcg_rand10() - 0x200) << 4;
                nz = (int16_t)(MAX_Z << 2);
                uint8_t hue = (uint8_t)(lcg_rand10() & 3);
                stars[i].z_hue = nz | ((uint16_t)hue << 14);
            } else {
                // Маска (nz & 0x3FFF) гарантирует, что координата z не повредит биты цвета
                stars[i].z_hue = (stars[i].z_hue & 0xC000) | (nz & 0x3FFF);
            }

            stars[i].x = (int16_t)x2;
            stars[i].y = (int16_t)y2;
        }
        // ── Render by CHUNK lines ──
        for (int y = 0; y < h; y += CHUNK) {
            int n = (y + CHUNK > h) ? h - y : CHUNK;
            memset(buf, 0, w * n * 2);

            for (int i = 0; i < STAR_COUNT; i++) {
                // Project on-the-fly (no stars_2d[] cache)
                uint16_t z_hue = stars[i].z_hue;
                uint16_t z     = (z_hue & 0x3FFF) >> 2;   // z_real 1..2048
                if (z == 0) z = 1;

                int sx = drift_cx + (((int32_t)stars[i].x >> 4) * FOCAL) / z;
                int sy = drift_cy + (((int32_t)stars[i].y >> 4) * FOCAL) / z;

                if (sy < y - 4 || sy >= y + n + 4) continue;

                int ly = sy - y;
                uint8_t hue = (uint8_t)(z_hue >> 14);
                uint16_t c  = synth_color(hue, z);
                uint8_t sz  = (z < 180) ? 3 : (z < 450 ? 2 : (z < 900 ? 1 : 0));

                #define PUT_PIXEL(px, py) do { \
                    if ((px) >= 0 && (px) < w && (py) >= 0 && (py) < n) \
                        buf[(py) * w + (px)] = c; \
                } while(0)

                if (sz == 0) {
                    PUT_PIXEL(sx, ly);
                } else if (sz == 1) {
                    PUT_PIXEL(sx, ly);     PUT_PIXEL(sx + 1, ly);
                    PUT_PIXEL(sx, ly + 1); PUT_PIXEL(sx + 1, ly + 1);
                } else {
                    for (int dy = -1; dy <= 1; dy++) {
                        PUT_PIXEL(sx - 1, ly + dy);
                        PUT_PIXEL(sx,     ly + dy);
                        PUT_PIXEL(sx + 1, ly + dy);
                    }
                    if (sz == 3) {
                        PUT_PIXEL(sx - 3, ly);     PUT_PIXEL(sx - 2, ly);
                        PUT_PIXEL(sx + 2, ly);     PUT_PIXEL(sx + 3, ly);
                        PUT_PIXEL(sx,     ly - 3); PUT_PIXEL(sx,     ly - 2);
                        PUT_PIXEL(sx,     ly + 2); PUT_PIXEL(sx,     ly + 3);
                    }
                }
                #undef PUT_PIXEL
            }

            rg_display_write_rect(0, y, w, n, w * 2, buf, 0);
        }

        int64_t dt = rg_system_timer() - now;
        int delay_ms = (int)((16666 - dt) / 1000);
        if (delay_ms > 0) rg_task_delay(delay_ms);
        vTaskDelay(1);
    }

    free(stars);
    free(buf);
    rg_display_clear(C_BLACK);
}

#ifdef __cplusplus
}
#endif
#endif
