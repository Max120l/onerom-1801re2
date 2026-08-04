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

# Answer every read in our windows regardless of the socket's CS.  Paired with
# MPI_WATCH this isolates one variable: build both, flash both, and the frame
# says whether honouring CS is costing the machine reads.
option(MPI_IGNORE_CS "Serve every window unconditionally, ignoring socket CS" OFF)
if(MPI_IGNORE_CS)
    target_compile_definitions(mpi_rom PRIVATE SOCKET_CS_CODE=0xFF)
endif()

# Briefly drive nRPLY high on release instead of leaving the rise to the bus.
# See the mpi_respond_assist program in mpi_rom.pio for why this exists.
option(MPI_RPLY_ASSIST "Assist the rising edge of nRPLY" OFF)
target_compile_definitions(mpi_rom PRIVATE MPI_RPLY_ASSIST=$<BOOL:${MPI_RPLY_ASSIST}>)

# Report our own test ROM's beacons on the LED instead of the stock monitor's
# behaviour.  Pair with an image built by tools/make_ramtest.py.
option(MPI_BEACONS "Blink the test ROM's beacons rather than watchpoints" OFF)

# MPI_BEACONS needs MPI_WATCH, and silently did not say so. Beacon counting and
# the LED frame both live inside #if MPI_WATCH, so -DMPI_BEACONS=ON on its own
# built a firmware that ignored every beacon and ran the ordinary status LED --
# which on hardware looked like the board blinking away busily while reporting
# nothing at all. An option that quietly does nothing is worse than one that
# does not exist.
#
# Resolved before either is defined, so exactly one MPI_WATCH reaches the
# compiler. Defining it twice and relying on the later flag winning is how the
# next version of this bug gets written.
if(MPI_BEACONS AND NOT MPI_WATCH)
    message(STATUS "MPI_BEACONS implies MPI_WATCH; enabling it")
    set(MPI_WATCH ON)
endif()

target_compile_definitions(mpi_rom PRIVATE
    MPI_WATCH=$<BOOL:${MPI_WATCH}>
    MPI_BEACONS=$<BOOL:${MPI_BEACONS}>)

# System clock in kHz.  150000 is the RP2350's rated speed and the default; the
# only reason to raise it is to shorten the address-to-reply path, which matters
# only if replies are being assembled too late to be taken.
set(MPI_SYS_CLK_KHZ "" CACHE STRING "System clock in kHz (default 150000)")
if(MPI_SYS_CLK_KHZ)
    target_compile_definitions(mpi_rom PRIVATE MPI_SYS_CLK_KHZ=${MPI_SYS_CLK_KHZ})
endif()

target_include_directories(mpi_rom PRIVATE ${CMAKE_CURRENT_LIST_DIR})

target_link_libraries(mpi_rom
    pico_stdlib
    pico_multicore
    hardware_pio
)

pico_add_extra_outputs(mpi_rom)
