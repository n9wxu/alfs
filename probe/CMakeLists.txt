# The flash probe (docs/TEST_PLAN.md section 6): an RP2040 program that runs
# from RAM. `make probe` from the top builds it.
cmake_minimum_required(VERSION 3.13)
include(pico_sdk_import.cmake)
project(alfs_probe C CXX ASM)
pico_sdk_init()

add_executable(alfs_probe probe.c)
target_compile_options(alfs_probe PRIVATE -Wall -Wextra -Werror)
target_link_libraries(alfs_probe pico_stdlib)
pico_set_binary_type(alfs_probe no_flash)
pico_enable_stdio_usb(alfs_probe 1)
pico_enable_stdio_uart(alfs_probe 0)
pico_add_extra_outputs(alfs_probe)
