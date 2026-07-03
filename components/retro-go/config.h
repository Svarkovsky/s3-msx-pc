// test
// 17 мая 2026
// 29 мая 2026
// 13 июня 2026
// 18 июня 2026
// 02 июля 2026

#if defined(RG_TARGET_S3_MSX_PC)
#include "targets/s3-msx-pc/config.h"
#elif defined(RG_TARGET_SDL2)
#include "targets/sdl2/config.h"
#else
#warning "No target defined. Defaulting to S3-MSX-PC."
#include "targets/s3-msx-pc/config.h"
#define RG_TARGET_S3_MSX_PC
#endif

#ifndef RG_PROJECT_NAME
#define RG_PROJECT_NAME "Retro-Go (ESP32-S3 MSX-PC)"
#endif

#ifndef RG_PROJECT_WEBSITE
#define RG_PROJECT_WEBSITE "https://github.com/Svarkovsky/s3-msx-pc"
#endif

#ifndef RG_PROJECT_CREDITS
#define RG_PROJECT_CREDITS \
    "ESP32-S3 MSX-PC by Ivan Svarkovsky, 2026\n\n" \
    "Based on:\n" \
    "  Retro-Go framework by ducalex\n" \
    "  fMSX emulator by Marat Fayzullin\n\n" \
    "Feedback: ivansvarkovsky@gmail.com"
#endif

#ifndef RG_PROJECT_APP
#define RG_PROJECT_APP "unknown"
#endif

#ifndef RG_PROJECT_VER
#define RG_PROJECT_VER "1.0.4-msx-pc"
#endif

#ifndef RG_BUILD_INFO
#define RG_BUILD_INFO "(none)"
#endif

#ifndef RG_BUILD_TIME
// 2020-01-31 00:00:00, first retro-go commit :)
#define RG_BUILD_TIME 1580446800
#endif

#ifndef RG_BUILD_DATE
#define RG_BUILD_DATE __DATE__ " " __TIME__
#endif

#ifndef RG_APP_LAUNCHER
#define RG_APP_LAUNCHER "launcher"
#endif

#ifndef RG_APP_FACTORY
#define RG_APP_FACTORY NULL
#endif

#ifndef RG_UPDATER_ENABLE
#define RG_UPDATER_ENABLE 0    // 1
#endif


// If either of the following isn't defined then the updater will only perform version *checks*, not self-update
// #define RG_UPDATER_APPLICATION       RG_APP_FACTORY
// #define RG_UPDATER_DOWNLOAD_LOCATION RG_STORAGE_ROOT "/odroid/firmware"

#ifndef RG_UPDATER_GITHUB_RELEASES
//#define RG_UPDATER_GITHUB_RELEASES "https://api.github.com/repos/ducalex/retro-go/releases?per_page=10"
#define RG_UPDATER_GITHUB_RELEASES ""
#endif

#ifndef RG_PATH_MAX
#define RG_PATH_MAX 255
#endif

#ifndef RG_RECOVERY_BTN
#define RG_RECOVERY_BTN RG_KEY_ANY
#endif

#ifndef RG_BATTERY_CALC_PERCENT
#define RG_BATTERY_CALC_PERCENT(raw) (100)
#endif

#ifndef RG_BATTERY_CALC_VOLTAGE
#define RG_BATTERY_CALC_VOLTAGE(raw) (0)
#endif

// These values are to prevent jitter, so that the battery icon doesn't flicker or
// percent display doesn't oscillate between 77 and 78%, for example
#ifndef RG_BATTERY_UPDATE_THRESHOLD
#define RG_BATTERY_UPDATE_THRESHOLD 1.0f
#endif
#ifndef RG_BATTERY_UPDATE_THRESHOLD_VOLT
#define RG_BATTERY_UPDATE_THRESHOLD_VOLT 0.010f
#endif

// Number of cycles the hardware state must be maintained before the change is reflected in rg_input_read_gamepad.
// The reaction time is calculated as such: N*10ms +/- 10ms. Different hardware types have different requirements.
// Valid range is 1-9
#ifndef RG_GAMEPAD_DEBOUNCE_PRESS
#define RG_GAMEPAD_DEBOUNCE_PRESS (2)
#endif
#ifndef RG_GAMEPAD_DEBOUNCE_RELEASE
#define RG_GAMEPAD_DEBOUNCE_RELEASE (2)
#endif
// Wait for ADC value to be stable before registering it (values of 50 - 250 are typically good)
#ifndef RG_GAMEPAD_ADC_FILTER_WINDOW
#define RG_GAMEPAD_ADC_FILTER_WINDOW (150)
#endif

#ifndef RG_LOG_COLORS
#define RG_LOG_COLORS (1)
#endif

#ifndef RG_TICK_RATE
#ifdef ESP_PLATFORM
#define RG_TICK_RATE CONFIG_FREERTOS_HZ
#else
#define RG_TICK_RATE 1000
#endif
#endif

#ifndef RG_ZIP_SUPPORT
#define RG_ZIP_SUPPORT 1
#endif

#ifndef RG_SCREEN_PARTIAL_UPDATES
#define RG_SCREEN_PARTIAL_UPDATES 1
#endif

#ifndef RG_SCREEN_SAFE_AREA
#define RG_SCREEN_SAFE_AREA {0, 0, 0, 0}
#endif

#ifndef RG_SCREEN_VISIBLE_AREA
#define RG_SCREEN_VISIBLE_AREA {0, 0, 0, 0}
#endif

#ifndef RG_LANG_DEFAULT
#define RG_LANG_DEFAULT RG_LANG_EN
#endif

#ifndef RG_FONT_DEFAULT
//#define RG_FONT_DEFAULT RG_FONT_VERA_11
#define RG_FONT_DEFAULT RG_FONT_VERA_14
#endif
