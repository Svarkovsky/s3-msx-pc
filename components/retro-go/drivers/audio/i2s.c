/**
 * Retro-Go PDM Audio Driver for ESP32-S3
 *
 * Hardware PDM stereo output via I2S peripheral.
 * Optimized for Xtensa LX7 by Ivan Svarkovsky, 2026.
 *
 * Optimizations:
 * - DMA buffer allocated once in internal SRAM (no stack VLA)
 * - Flat loop for GCC auto-vectorization (no struct dereference)
 * - Fixed-point volume scaling (multiply instead of divide)
 * - Full 16-bit range, no artificial headroom
 *
 * Pin mapping (stereo):
 *   GPIO 9  — PDM CLK
 *   GPIO 10 — DOUT  (Right channel)
 *   GPIO 11 — DOUT2 (Left channel)
 */

#include "rg_system.h"
#include "rg_audio.h"

#if RG_AUDIO_USE_EXT_DAC

#include "freertos/FreeRTOS.h"
#include "driver/i2s_pdm.h"
#include "driver/gpio.h"
#include "esp_heap_caps.h"

#define DMA_BUFFER_COUNT 4
#define DMA_BUFFER_LEN   640
#define MAX_STEREO_SAMPLES (1024 * 2)

#ifndef UNLIKELY
#define UNLIKELY(x) __builtin_expect(!!(x), 0)
#endif

static i2s_chan_handle_t tx_chan = NULL;
static int16_t *stereo_buf = NULL;
static struct { const char *err; int vol; bool mute; } state;

static bool driver_init(int device, int sample_rate) {
    if (tx_chan) return true;
    state.err = NULL;

    if (!stereo_buf) {
        stereo_buf = (int16_t *)heap_caps_malloc(MAX_STEREO_SAMPLES * sizeof(int16_t), 
                                                 MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
        if (!stereo_buf) {
            state.err = "Failed to allocate DMA stereo buffer"; 
            return false;
        }
    }

    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan_cfg.auto_clear = true;
    if (i2s_new_channel(&chan_cfg, &tx_chan, NULL) != ESP_OK) {
        state.err = "i2s_new_channel failed"; return false;
    }

    i2s_pdm_tx_config_t pdm_tx_cfg = {
        .clk_cfg = I2S_PDM_TX_CLK_DEFAULT_CONFIG(sample_rate),
        .slot_cfg = I2S_PDM_TX_SLOT_DAC_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO), 
        .gpio_cfg = {
            .clk = RG_GPIO_SND_I2S_BCK,
            .dout = RG_GPIO_SND_I2S_DATA,
            .dout2 = RG_GPIO_SND_I2S_DATA2,    
            .invert_flags = { .clk_inv = false },
        },
    };

    if (i2s_channel_init_pdm_tx_mode(tx_chan, &pdm_tx_cfg) != ESP_OK) {
        state.err = "i2s_channel_init_pdm_tx_mode failed"; return false;
    }

    if (i2s_channel_enable(tx_chan) != ESP_OK) {
        state.err = "i2s_channel_enable failed"; return false;
    }

    RG_LOGI("PDM Stereo ready (CLK=%d, DOUT/Right=%d, DOUT2/Left=%d).\n",
            RG_GPIO_SND_I2S_BCK, RG_GPIO_SND_I2S_DATA, RG_GPIO_SND_I2S_DATA2);
    return true;
}

static bool driver_deinit(void) {
    if (tx_chan) {
        i2s_channel_disable(tx_chan);
        i2s_del_channel(tx_chan);
        tx_chan = NULL;
    }
    if (stereo_buf) {
        free(stereo_buf);
        stereo_buf = NULL;
    }
    gpio_reset_pin(RG_GPIO_SND_I2S_DATA);
    gpio_reset_pin(RG_GPIO_SND_I2S_DATA2);  
    gpio_reset_pin(RG_GPIO_SND_I2S_BCK);
    return true;
}

static bool __attribute__((optimize("O3"))) driver_submit(const rg_audio_frame_t *frames, size_t count) {
    if (!tx_chan || !stereo_buf) return false;

    if (UNLIKELY(count > MAX_STEREO_SAMPLES / 2)) {
        count = MAX_STEREO_SAMPLES / 2;
    }

    int32_t vol_mul = state.mute ? 0 : (state.vol * 2622) >> 10;
    if (vol_mul > 256) vol_mul = 256;

    const int16_t * __restrict__ src = (const int16_t *)frames;
    int16_t * __restrict__ dst = stereo_buf;
    size_t total_samples = count * 2;

    for (size_t i = 0; i < total_samples; i++) {
        dst[i] = (int16_t)((src[i] * vol_mul + 128) >> 8);
    }

    size_t written = 0;
    i2s_channel_write(tx_chan, stereo_buf, total_samples * sizeof(int16_t), &written, pdMS_TO_TICKS(100));
    return true;
}

static bool driver_set_mute(bool mute) { state.mute = mute; return true; }
static bool driver_set_volume(int volume) { state.vol = volume; return true; }

static bool driver_set_sample_rates(int sample_rate) {
    if (!tx_chan) return false;
    i2s_pdm_tx_clk_config_t clk_cfg = I2S_PDM_TX_CLK_DEFAULT_CONFIG(sample_rate);
    return i2s_channel_reconfig_pdm_tx_clock(tx_chan, &clk_cfg) == ESP_OK;
}

const rg_audio_driver_t rg_audio_driver_i2s = {
    .name = "i2s_pdm_stereo", 
    .init = driver_init, 
    .deinit = driver_deinit, 
    .submit = driver_submit,
    .set_mute = driver_set_mute, 
    .set_volume = driver_set_volume, 
    .set_sample_rate = driver_set_sample_rates,
    .get_error = (void*) &state.err,
};

#endif // RG_AUDIO_USE_EXT_DAC
