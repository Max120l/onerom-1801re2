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
    decode.c
    rom_images.c        # generated: tools/gen_rom_images.py
)

pico_generate_pio_header(mpi_rom ${CMAKE_CURRENT_LIST_DIR}/mpi_rom.pio)

# Diagnostic build: score a handful of monitor addresses and blink the result.
# Off by default -- a board left in a machine should be a ROM and nothing else.
option(MPI_WATCH "Watch monitor addresses and report them on the LED" OFF)
target_compile_definitions(mpi_rom PRIVATE MPI_WATCH=$<BOOL:${MPI_WATCH}>)

# Answer every read in our windows regardless of the socket's CS.  Paired with
# MPI_WATCH this isolates one variable: build both, flash both, and the frame
# says whether honouring CS is costing the machine reads.
option(MPI_IGNORE_CS "Serve every window unconditionally, ignoring socket CS" OFF)
if(MPI_IGNORE_CS)
    target_compile_definitions(mpi_rom PRIVATE SOCKET_CS_CODE=0xFF)
endif()

target_include_directories(mpi_rom PRIVATE ${CMAKE_CURRENT_LIST_DIR})

target_link_libraries(mpi_rom
    pico_stdlib
    pico_multicore
    hardware_pio
)

pico_add_extra_outputs(mpi_rom)
