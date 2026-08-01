cmake_minimum_required(VERSION 3.13)

# One ROM Fire 24 rev E carries an RP2354A: an RP2350A with 2 MB of stacked
# flash, so there is no external flash chip to configure.
set(PICO_PLATFORM rp2350)
set(PICO_BOARD pico2)

include($ENV{PICO_SDK_PATH}/external/pico_sdk_import.cmake)

project(mpi_rom C CXX ASM)
set(CMAKE_C_STANDARD 11)
pico_sdk_init()

add_executable(mpi_rom
    main.c
    rom_images.c        # generated: tools/gen_rom_images.py
)

pico_generate_pio_header(mpi_rom ${CMAKE_CURRENT_LIST_DIR}/mpi_rom.pio)

target_include_directories(mpi_rom PRIVATE ${CMAKE_CURRENT_LIST_DIR})

target_link_libraries(mpi_rom
    pico_stdlib
    pico_multicore
    hardware_pio
)

# Define this only once the 1801RE2 power and signal pinout in board_fire24e.h
# has been checked against the datasheet or a known-good board.
# target_compile_definitions(mpi_rom PRIVATE RE2_PINOUT_CONFIRMED=1)

pico_add_extra_outputs(mpi_rom)
