cmake_minimum_required(VERSION 3.5)

set(COMPONENTS "main retro-go fmsx")
set(RG_ENABLE_NETWORKING 1)

include(../base.cmake)

project(fmsx)

# LTO
target_compile_options(${PROJECT_NAME}.elf PRIVATE -flto)
target_link_options(${PROJECT_NAME}.elf PRIVATE -flto)
