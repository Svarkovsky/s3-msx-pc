/**
 * Retro-Go PDM Audio Driver for ESP32-S3
 *
 * Hardware PDM stereo output via I2S peripheral.
 * Adapted for MSX emulator (fMSX) by Ivan Svarkovsky, 2026.
 *
 * Features:
 * - Native PDM TX mode via ESP-IDF v5.x i2s_pdm API
 * - True stereo output on two GPIO pins (DOUT + DOUT2)
 *
 * Pin mapping (stereo):
 *   GPIO 9  — PDM CLK
 *   GPIO 10 — DOUT  (Right channel)
 *   GPIO 11 — DOUT2 (Left channel)
 *
 * Data format:
 *   I2S_PDM_TX_SLOT_DAC_DEFAULT_CONFIG with I2S_SLOT_MODE_STEREO
 *   automatically selects TWO_LINE_DAC mode
 *
 *   Buffer layout (int16_t pairs):
 *     stereo_buf[i*2]     = Left  → DOUT2 (GPIO 11)
 *     stereo_buf[i*2+1]   = Right → DOUT  (GPIO 10)
 *
 * References:
 *   ESP-IDF Programming Guide — I2S PDM TX
 *   https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/api-reference/peripherals/i2s.html
 */
 


#include "rg_system.h"
#include "rg_audio.h"

#if RG_AUDIO_USE_EXT_DAC

#include "freertos/FreeRTOS.h"
#include "driver/i2s_pdm.h"
#include "driver/gpio.h"

#define DMA_BUFFER_COUNT 4
#define DMA_BUFFER_LEN   640

static i2s_chan_handle_t tx_chan = NULL;
static struct { const char *err; int vol; bool mute; } state;

static bool driver_init(int device, int sample_rate) {
    if (tx_chan) return true;
    state.err = NULL;

    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan_cfg.auto_clear = true;
    if (i2s_new_channel(&chan_cfg, &tx_chan, NULL) != ESP_OK) {
        state.err = "i2s_new_channel failed"; return false;
    }

    // <-- Используем DAC_DEFAULT_CONFIG вместо обычного DEFAULT_CONFIG.
    // Он автоматически выберет TWO_LINE_DAC при STEREO.
    i2s_pdm_tx_config_t pdm_tx_cfg = {
        .clk_cfg = I2S_PDM_TX_CLK_DEFAULT_CONFIG(sample_rate),
        .slot_cfg = I2S_PDM_TX_SLOT_DAC_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO), // <-- STEREO!
        .gpio_cfg = {
            .clk = RG_GPIO_SND_I2S_BCK,
            .dout = RG_GPIO_SND_I2S_DATA,
            .dout2 = RG_GPIO_SND_I2S_DATA2,   // <-- Второй пин!
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
    gpio_reset_pin(RG_GPIO_SND_I2S_DATA);
    gpio_reset_pin(RG_GPIO_SND_I2S_DATA2);  // <-- Сброс второго пина
    gpio_reset_pin(RG_GPIO_SND_I2S_BCK);
    return true;
}

static bool driver_submit(const rg_audio_frame_t *frames, size_t count) {
    if (!tx_chan) return false;
    float volume = state.mute ? 0.f : (state.vol * 0.01f);

    // <-- Теперь стерео-буфер: 2 int16_t на каждый фрейм
    int16_t stereo_buf[count * 2];
    for (size_t i = 0; i < count; i++) {
        int32_t left  = frames[i].left  * volume;
        int32_t right = frames[i].right * volume;
        stereo_buf[i * 2]     = (left  < -32768) ? -32768 : (left  > 32767) ? 32767 : left;
        stereo_buf[i * 2 + 1] = (right < -32768) ? -32768 : (right > 32767) ? 32767 : right;
    }

    size_t written = 0;
    // <-- Передаём count*2 сэмплов (count*4 байта)
    i2s_channel_write(tx_chan, stereo_buf, count * 2 * sizeof(int16_t), &written, pdMS_TO_TICKS(100));
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
    .name = "i2s_pdm_stereo", .init = driver_init, .deinit = driver_deinit, .submit = driver_submit,
    .set_mute = driver_set_mute, .set_volume = driver_set_volume, .set_sample_rate = driver_set_sample_rates,
    .get_error = (void*) &state.err,
};

#endif // RG_AUDIO_USE_EXT_DAC
