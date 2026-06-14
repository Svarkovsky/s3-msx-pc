# Retro-Go shared library component
set(COMPONENT_SRCDIRS ". drivers/audio fonts libs/lodepng")
set(COMPONENT_ADD_INCLUDEDIRS ". libs/lodepng")

# ESP-IDF version specific dependencies
if((IDF_VERSION_MAJOR EQUAL 5) AND (IDF_VERSION_MINOR GREATER 2)) # esp-idf 5.3+
    set(COMPONENT_REQUIRES "wear_levelling usb esp_lcd spi_flash fatfs app_update json nvs_flash esp_adc esp_timer esp_psram esp_wifi esp_http_client esp_driver_ledc driver") # esp_mm
elseif(IDF_VERSION_MAJOR EQUAL 5) # esp-idf 5.0 - 5.2
    set(COMPONENT_REQUIRES "spi_flash fatfs app_update json nvs_flash esp_adc esp_timer esp_psram esp_wifi esp_http_client")
else() # esp-idf 4.1 - 4.4
    set(COMPONENT_REQUIRES "spi_flash fatfs app_update esp_adc_cal json nvs_flash esp_wifi esp_http_client")
endif()

register_component()

# Small size is preferred because of the small cache and most things here aren't performance sensitive!
# rg_display and rg_audio benefit from higher optimization (which of -O2 or -O3 is better depends...)

# Base compile options: prefer small size, disable unused lodepng features
component_compile_options(
    -DLODEPNG_NO_COMPILE_ANCILLARY_CHUNKS
    -DLODEPNG_NO_COMPILE_ERROR_TEXT
    -Os
    -Wno-unused-function
)

# Performance-critical files get -O3
set_source_files_properties(
    rg_display.c PROPERTIES COMPILE_FLAGS "-O3"
)
set_source_files_properties(
    rg_audio.c rg_utils.c rg_surface.c libs/lodepng/lodepng.c
    PROPERTIES COMPILE_FLAGS "-O2"
)


# === Conditional Features (Network, Profiling, Release) ===
if(RG_ENABLE_NETPLAY)
    component_compile_options(-DRG_ENABLE_NETWORKING -DRG_ENABLE_NETPLAY)
elseif(RG_ENABLE_NETWORKING)
    component_compile_options(-DRG_ENABLE_NETWORKING)
endif()

if(RG_ENABLE_PROFILING)
    component_compile_options(-DRG_ENABLE_PROFILING)
endif()

if(RG_BUILD_RELEASE)
    component_compile_options(-DRG_BUILD_RELEASE=1)
endif()

# === MSX-PC Fork Specific Settings ===
# Отключаем встроенный OTA-апдейтер (проверку обновлений)
component_compile_options(-DRG_UPDATER_ENABLE=0)

# === Build Metadata ===
set(RG_BUILD_INFO "${RG_BUILD_TARGET}-${RG_BUILD_RELEASE} ${PROJECT_VER} SDK:${IDF_VER} ${IDF_TARGET}")
string(TIMESTAMP RG_BUILD_TIME "%s")

if(DEFINED RG_BUILD_TARGET)
    component_compile_options(-D${RG_BUILD_TARGET}=1)
else()
    component_compile_options(-DRG_TARGET_S3_MSX_PC=1)
    message(WARNING "RG_BUILD_TARGET not defined, defaulting to RG_TARGET_S3_MSX_PC")
endif()

component_compile_options(-DRG_PROJECT_APP="${RG_PROJECT_APP}")
component_compile_options(-DRG_BUILD_INFO="${RG_BUILD_INFO}")
component_compile_options(-DRG_BUILD_TIME=${RG_BUILD_TIME})

# Status messages for build output
message(STATUS "RG_PROJECT_APP: ${RG_PROJECT_APP}")
message(STATUS "RG_PROJECT_VER: ${RG_PROJECT_VER}")
message(STATUS "RG_BUILD_INFO: ${RG_BUILD_INFO}")
message(STATUS "RG_UPDATER_ENABLE: 0 (Disabled for MSX-PC fork)")
