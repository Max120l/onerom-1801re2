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
# Show the last pass rather than every pass since power-on: a lamp for freeze
# spray, where a latching frame can only ever say "it failed at some point".
# See PP_BEACON_DONE in watch.h.  Pair with `make_ramtest.py --live`.
option(MPI_BEACON_LIVE "Report only the most recent test pass" OFF)

# Same clear-on-DONE, but keep the full frame instead of reducing it to a lamp.
# For a test that can wedge partway through a pass: the frame then freezes
# showing exactly the phases it reached, which a latching frame cannot say and
# a lamp has no room to.
option(MPI_BEACON_PASS "Blink the current pass's frame, cleared each pass" OFF)
if(MPI_BEACON_PASS AND NOT MPI_BEACONS)
    message(STATUS "MPI_BEACON_PASS implies MPI_BEACONS; enabling it")
    set(MPI_BEACONS ON)
endif()
target_compile_definitions(mpi_rom PRIVATE MPI_BEACON_PASS=$<BOOL:${MPI_BEACON_PASS}>)

if(MPI_BEACON_LIVE AND NOT MPI_BEACONS)
    message(STATUS "MPI_BEACON_LIVE implies MPI_BEACONS; enabling it")
    set(MPI_BEACONS ON)
endif()

# Blink the address of the cycle immediately before the PP last restarted
# itself -- sixteen pulses, one number, nothing else.  See main.c.
option(MPI_BEACON_KILLADDR "Blink the address the PP died on" OFF)

# The same sixteen pulses, but the last address on the bus rather than the last
# one before a restart -- so a PP that wedges or runs off into RAM is covered
# too, not only one that restarts.  See main.c.
option(MPI_BEACON_LASTADDR "Blink the last address seen on the bus" OFF)
if(MPI_BEACON_LASTADDR AND NOT MPI_WATCH)
    message(STATUS "MPI_BEACON_LASTADDR implies MPI_WATCH; enabling it")
    set(MPI_WATCH ON)
endif()

if(MPI_BEACON_KILLADDR AND NOT MPI_WATCH)
    message(STATUS "MPI_BEACON_KILLADDR implies MPI_WATCH; enabling it")
    set(MPI_WATCH ON)
endif()
if(MPI_BEACONS AND NOT MPI_WATCH)
    message(STATUS "MPI_BEACONS implies MPI_WATCH; enabling it")
    set(MPI_WATCH ON)
endif()

target_compile_definitions(mpi_rom PRIVATE
    MPI_WATCH=$<BOOL:${MPI_WATCH}>
    MPI_BEACONS=$<BOOL:${MPI_BEACONS}>
    MPI_BEACON_LIVE=$<BOOL:${MPI_BEACON_LIVE}>
    MPI_BEACON_KILLADDR=$<BOOL:${MPI_BEACON_KILLADDR}>
    MPI_BEACON_LASTADDR=$<BOOL:${MPI_BEACON_LASTADDR}>)

# How many beacon pulses the frame blinks.  Different test ROMs have different
# beacon maps: the soak uses 17, the address-bus test 19.  See watch.h.
set(MPI_BEACON_COUNT "" CACHE STRING "Beacons in the frame (default 17)")
if(MPI_BEACON_COUNT)
    target_compile_definitions(mpi_rom PRIVATE PP_BEACON_COUNT=${MPI_BEACON_COUNT})
endif()

# Which beacon ends a pass, and which beacons are good news.  Only used by
# MPI_BEACON_LIVE, and only correct for the ROM being flashed: the soak says
# 6 and 0x4B, the address-bus test says 1 and 0x3.
set(MPI_BEACON_DONE "" CACHE STRING "Beacon marking end of pass (default 6)")
if(MPI_BEACON_DONE STRGREATER "")
    target_compile_definitions(mpi_rom PRIVATE PP_BEACON_DONE=${MPI_BEACON_DONE})
endif()
set(MPI_BEACON_OK_MASK "" CACHE STRING "Bitmask of non-failure beacons")
if(MPI_BEACON_OK_MASK)
    target_compile_definitions(mpi_rom PRIVATE PP_BEACON_OK_MASK=${MPI_BEACON_OK_MASK})
endif()

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
