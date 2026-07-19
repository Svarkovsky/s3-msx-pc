set(COMPONENT_SRCDIRS ". drivers/audio fonts libs/stlz")
set(COMPONENT_ADD_INCLUDEDIRS ". libs/stlz")

set(COMPONENT_REQUIRES "wear_levelling usb esp_lcd spi_flash fatfs app_update json nvs_flash esp_adc esp_timer esp_psram esp_driver_ledc driver")

if(CMAKE_SOURCE_DIR MATCHES "launcher")
    set(COMPONENT_REQUIRES "${COMPONENT_REQUIRES} esp_wifi esp_http_client")
    message(STATUS "Retro-Go: Building LAUNCHER. WiFi and HTTP components enabled.")
else()
    set(RG_ENABLE_NETWORKING 0)
    set(RG_ENABLE_NETPLAY 0)
endif()

register_component()

component_compile_options(
    -Os
    -Wno-unused-function
)

set_source_files_properties(
    rg_audio.c rg_display.c PROPERTIES COMPILE_FLAGS "-O3"
)
set_source_files_properties(
    rg_utils.c rg_surface.c libs/stlz/ds-stlz.c
    PROPERTIES COMPILE_FLAGS "-O2"
)

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

component_compile_options(-DRG_UPDATER_ENABLE=0)

set(RG_BUILD_INFO "${RG_BUILD_TARGET}-${RG_BUILD_RELEASE} ${PROJECT_VER} SDK:${IDF_VER} ${IDF_TARGET}")
string(TIMESTAMP RG_BUILD_TIME "%s")

if(DEFINED RG_BUILD_TARGET)
    component_compile_options(-D${RG_BUILD_TARGET}=1)
else()
    component_compile_options(-DRG_TARGET_S3_MSX_PC=1)
endif()

component_compile_options(-DRG_PROJECT_APP="${RG_PROJECT_APP}")
component_compile_options(-DRG_BUILD_INFO="${RG_BUILD_INFO}")
component_compile_options(-DRG_BUILD_TIME=${RG_BUILD_TIME})
