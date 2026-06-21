set(COMPONENT_SRCDIRS ".")
set(COMPONENT_ADD_INCLUDEDIRS ".")
register_component()

# Подключаем miniz (уже встроен в Retro-Go)
#set(COMPONENT_REQUIRES miniz)

# LTO для main.c
component_compile_options(-flto -fno-fat-lto-objects)

rg_setup_compile_options(-O3)
