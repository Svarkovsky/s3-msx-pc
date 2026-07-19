#pragma once

#define RG_TARGET_NAME             "S3-MSX-PC"

// === Память ===
#define RG_STORAGE_ROOT             "/sd" 
#define RG_STORAGE_DRIVER           2
#define RG_STORAGE_INTERNAL         1
#define RG_STORAGE_HOST             SPI3_HOST
#define RG_STORAGE_SPEED            40000000
#define RG_STORAGE_FLASH_PARTITION  "storage"

// === Аудио (PDM) ===
#define RG_AUDIO_USE_INT_DAC        0
#define RG_AUDIO_USE_EXT_DAC        1
#define RG_GPIO_SND_I2S_BCK         9   // PDM CLK
#define RG_GPIO_SND_I2S_WS          -1  // Не используется в PDM
#define RG_GPIO_SND_I2S_DATA        10  // PDM DOUT  (Primary / Right)
#define RG_GPIO_SND_I2S_DATA2       11  // PDM DOUT2 (Secondary / Left)  

// === Видео: VGA 640×480@60Hz ===
// Экран 640×480 для GUI и совместимости с мониторами
//#define RG_SCREEN_PARTIAL_UPDATES   0
#define RG_SCREEN_DRIVER            2
#define RG_SCREEN_WIDTH             640
#define RG_SCREEN_HEIGHT            480
#define RG_SCREEN_ROTATE            0
#define RG_SCREEN_VISIBLE_AREA      {0, 0, 0, 0}
#define RG_SCREEN_SAFE_AREA         {0, 0, 0, 0}

// VGA пины (R-2R Ladder)
#define RG_GPIO_VGA_R0              1
#define RG_GPIO_VGA_R1              2
#define RG_GPIO_VGA_G0              4
#define RG_GPIO_VGA_G1              5
#define RG_GPIO_VGA_B0              6
#define RG_GPIO_VGA_B1              7
#define RG_GPIO_VGA_HSYNC           13
#define RG_GPIO_VGA_VSYNC           14

// === USB Клавиатура ===
#define RG_ENABLE_USB_HOST          1
#define RG_GAMEPAD_MAP_KB           1
#define RG_GAMEPAD_GPIO_MAP {}
#define RG_GAMEPAD_ADC_MAP  {}

// === Батарея ===
#define RG_BATTERY_DRIVER           0
#define RG_BATTERY_CALC_PERCENT(raw) (100)
#define RG_BATTERY_CALC_VOLTAGE(raw) (5.0f)

#define RG_RECOVERY_BTN             0
#define RG_ALLOC_PSRAM_FORCE        1
