set(COMPONENT_SRCDIRS ". src/EMULib src/EMULib/emu src/fMSX src/Z80")
set(COMPONENT_ADD_INCLUDEDIRS ". src/EMULib src/EMULib/emu src/fMSX src/Z80")
set(COMPONENT_REQUIRES "retro-go")
register_component()

# ===== LTO только для эмулятора fmsx =====
 component_compile_options(-flto -fno-fat-lto-objects)
# ==========================================

# ===  ===
rg_setup_compile_options(
  #  -Wno-error=stringop-truncation
    -Wno-error=misleading-indentation
    -Wno-error=format-overflow
    -Wno-implicit-fallthrough
    -Wno-format-overflow
    -Wno-stringop-truncation
    -Wno-unused-but-set-variable
    -Wno-unused-variable
    -DBPS16 -DUNIX -DLSB_FIRST
    -DNARROW
    -fomit-frame-pointer
    -fno-strict-aliasing
    -mlongcalls
    -mtarget-align
    -fshort-enums
    -ffunction-sections           
    -fdata-sections               
)


# === ВСЕ C-файлы: msxfix.h ===
file(GLOB_RECURSE c_sources "src/*.c")
set_source_files_properties(${c_sources} PROPERTIES COMPILE_FLAGS "-include msxfix.h")

# === Mitsutaka Okazaki: отключаем -Werror для чужого кода ===
file(GLOB okazaki_sources "src/EMULib/emu/*.c")
set_source_files_properties(${okazaki_sources}
    PROPERTIES COMPILE_FLAGS
    "-O2 -include msxfix.h -Wno-error -Wno-implicit-fallthrough -Wno-parentheses"
)

# === Z80.c: 32KB I-cache ===
set_source_files_properties(
    "${CMAKE_CURRENT_SOURCE_DIR}/src/Z80/Z80.c"
    PROPERTIES COMPILE_FLAGS "\
        -O3 \
        -fomit-frame-pointer \
        -fno-strict-aliasing \
        -fwrapv \
        -frename-registers \
        -fsched-interblock \
        -fshrink-wrap \
        -include msxfix.h"
)

# === MSX.c: ===
set_source_files_properties(
    "${CMAKE_CURRENT_SOURCE_DIR}/src/fMSX/MSX.c"
    PROPERTIES COMPILE_FLAGS "-O3 -fomit-frame-pointer -fshrink-wrap -fshort-enums -include msxfix.h -Wno-error=misleading-indentation"
)

# === V9938.c: ===
set_source_files_properties(
    "${CMAKE_CURRENT_SOURCE_DIR}/src/fMSX/V9938.c"
    PROPERTIES COMPILE_FLAGS "-O2 -fomit-frame-pointer -fshrink-wrap -fshort-enums -include msxfix.h"
)

# === Touch.c ===
set_source_files_properties(
    "${CMAKE_CURRENT_SOURCE_DIR}/src/EMULib/Touch.c"
    PROPERTIES COMPILE_FLAGS "-include msxfix.h -Wno-strict-prototypes"
)

target_link_options(${COMPONENT_TARGET} PRIVATE
    -Wl,--gc-sections            
    -Wl,--strip-all    
 #   -Wl, --section-start=.iram1.text=0x4038C000          
)
