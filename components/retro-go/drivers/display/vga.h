// SPDX-License-Identifier: GPL-3.0-or-later

/**
    vga.h — VGA 640×480@60Hz driver for Retro-Go (ESP32-S3)
    Copyright (C) 2026 Ivan Svarkovsky

    ─────────────────────────────────────────────────────────
    Architecture: DRAM Render Buffer + Hardware Byte-Swap
    ─────────────────────────────────────────────────────────

    Pixel Data Flow:
      Retro-Go write_update() → sram_render_buffer (Fast DRAM, 5KB)
           │
      lcd_send_buffer(): Pure memcpy → vga_fb (PSRAM)
           │
      LCD_CAM DMA reads vga_fb (PSRAM)
           │
      Hardware Byte-Swap (lcd_byte_order=1, zero CPU cost)
           │
      GPIO → VGA DAC (R-2R ladder, 16-bit)
*/

#pragma once
#include <stdint.h>
#include <string.h>
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_rgb.h"
#include "soc/lcd_cam_struct.h"
#include "esp_rom_sys.h" // esp_rom_delay_us
#include "esp_attr.h"    // DRAM_ATTR, IRAM_ATTR

#define VGA_WIDTH   640
#define VGA_HEIGHT  480

#define VGA_PCLK    24000000     // 240 / 10

#define MAX_CHUNK_PIXELS (RG_SCREEN_WIDTH * 4) // 2560 пикселей = 5 КБ

static esp_lcd_panel_handle_t panel_handle = NULL;
static uint16_t *vga_fb = NULL;
uint16_t *shared_framebuffer = NULL;
static bool framebuffer_initialized = false;

static int window_x = 0, window_y = 0, window_w = 0, window_h = 0, current_y = 0;

uint16_t DRAM_ATTR __attribute__((aligned(64))) sram_render_buffer[MAX_CHUNK_PIXELS];

void rg_display_off(void) {
    if (panel_handle) { esp_lcd_panel_disp_on_off(panel_handle, false); }
}

void rg_display_on(void) {
    if (panel_handle) { esp_lcd_panel_disp_on_off(panel_handle, true); }
}

static void lcd_set_backlight(float percent) { (void)percent; }
static void lcd_set_rotation(int rotation) { (void)rotation; }

static void lcd_init(void) {
    if (framebuffer_initialized) {
        return;
    }

    RG_LOGI("VGA Init: 640x480@60Hz (Hardware Byte-Swap + DRAM Buffer)");

    esp_lcd_rgb_panel_config_t panel_config = {
        .data_width = 16,
        .psram_trans_align = 64,
        .num_fbs = 1,                                
        .bounce_buffer_size_px = 10 * VGA_WIDTH,
        .clk_src = LCD_CLK_SRC_PLL240M,
        .disp_gpio_num = -1,
        .pclk_gpio_num = 45,
        .vsync_gpio_num = RG_GPIO_VGA_VSYNC,
        .hsync_gpio_num = RG_GPIO_VGA_HSYNC,
        .de_gpio_num = -1,
        .data_gpio_nums = {
            -1, -1, -1, RG_GPIO_VGA_B0, RG_GPIO_VGA_B1,
            -1, -1, -1, -1, RG_GPIO_VGA_G0, RG_GPIO_VGA_G1,
            -1, -1, -1, RG_GPIO_VGA_R0, RG_GPIO_VGA_R1
        },
        .timings = {
            .pclk_hz = VGA_PCLK,
            .h_res = VGA_WIDTH,
            .v_res = VGA_HEIGHT,
            .hsync_back_porch  = 48,   
            .hsync_front_porch = 16,   
            .hsync_pulse_width = 96,    
            .vsync_back_porch = 33,  
            .vsync_front_porch = 10,  
            .vsync_pulse_width = 2,
            .flags.hsync_idle_low = 0,
            .flags.vsync_idle_low = 0,
        },
        .flags.fb_in_psram = 1,
    };

    ESP_ERROR_CHECK(esp_lcd_new_rgb_panel(&panel_config, &panel_handle));
    ESP_ERROR_CHECK(esp_lcd_panel_reset(panel_handle));
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel_handle));

    ESP_ERROR_CHECK(esp_lcd_rgb_panel_get_frame_buffer(panel_handle, 1, (void **)&vga_fb));
    shared_framebuffer = vga_fb;
    framebuffer_initialized = true;

    memset(vga_fb, 0, VGA_WIDTH * VGA_HEIGHT * 2);

    LCD_CAM.lcd_user.lcd_byte_order = 1;

    RG_LOGI("VGA Init done");
}

static void lcd_deinit(void) {
    if (shared_framebuffer) {
        memset(shared_framebuffer, 0, VGA_WIDTH * VGA_HEIGHT * 2);
    }
    if (panel_handle) {
        esp_lcd_panel_del(panel_handle);
        panel_handle = NULL;
    }
    framebuffer_initialized = false;
    shared_framebuffer = NULL;
    vga_fb = NULL;
}

static void lcd_set_window(int left, int top, int width, int height) {
    window_x = left;
    window_y = top;
    window_w = width;
    window_h = height;
    current_y = top;
}

static inline uint16_t *lcd_get_buffer(size_t length) {
    (void)length;
    return sram_render_buffer;
}

static inline void IRAM_ATTR lcd_send_buffer(uint16_t *buffer, size_t length) {
    if (__builtin_expect((length == 0 || window_w == 0 || !vga_fb), 0)) { return; }

    int lines = length / window_w;

    if (__builtin_expect((lines == 0), 0)) { return; }

    int line_bytes = window_w * 2;
    for (int i = 0; i < lines; i++) {
        uint16_t *dst = &vga_fb[(current_y + i) * VGA_WIDTH + window_x];
        
        memcpy(dst, &buffer[i * window_w], line_bytes);
        
        // esp_rom_delay_us(3); 
    }

    current_y += lines;
}

static void lcd_sync(void) { }
