// Board and chip pin mapping for One ROM Fire 24 rev E emulating a 1801RE2.
//
// The socket-pin -> GPIO half of this file is taken from the One ROM project's
// own board description (rust/config/json/fire-24-e.json) and is known good.
// The chip-pin -> signal half is NOT yet verified; see the RE2 section below.

#ifndef BOARD_FIRE24E_H
#define BOARD_FIRE24E_H

// ---------------------------------------------------------------------------
// One ROM Fire 24 rev E: 24-pin socket to RP2354A GPIO
// ---------------------------------------------------------------------------
//
// Socket pins 12 and 24 are power (GND and VCC) and are not routed to GPIOs.
// Every other socket pin lands on a GPIO in 0..7 or 10..23.  GPIO 8 and 9 go
// to the X1/X2 jumper pads, NOT to the socket -- this gap is the single most
// important constraint on the design, because it means no 16 socket pins are
// ever contiguous in GPIO space.  See README.md.

#define SOCKET_PIN_TO_GPIO { \
    /*  1 */ 16, /*  2 */ 17, /*  3 */ 18, /*  4 */ 19, \
    /*  5 */ 20, /*  6 */ 21, /*  7 */ 22, /*  8 */ 23, \
    /*  9 */  7, /* 10 */  6, /* 11 */  5, /* 12 */ 0xFF /* GND */, \
    /* 13 */  0, /* 14 */  1, /* 15 */  2, /* 16 */  3, \
    /* 17 */  4, /* 18 */ 11, /* 19 */ 13, /* 20 */ 10, \
    /* 21 */ 12, /* 22 */ 14, /* 23 */ 15, /* 24 */ 0xFF /* VCC */ }

#define GPIO_X1              9    // jumper pad, not a socket pin
#define GPIO_X2              8    // jumper pad, not a socket pin
#define GPIO_STATUS_LED     29
#define GPIO_SEL_JUMPERS  { 25, 24, 26, 27 }

// The PIO reads and writes GPIO 0..23 as one 24-bit field.  GPIO 8 and 9 fall
// inside that field but are deliberately never muxed to the PIO, so PIO writes
// to them are inert and a fitted X jumper cannot be shorted by an output
// driver.
#define BUS_FIELD_BASE       0
#define BUS_FIELD_WIDTH     24

// ---------------------------------------------------------------------------
// 1801RE2 chip pinout -- NOT VERIFIED
// ---------------------------------------------------------------------------
//
// Everything below is a placeholder.  Three things must be confirmed against
// the chip datasheet, the UKNC schematic, or the RE-mulator documentation
// before this firmware is plugged into a host.  The first two destroy hardware
// if wrong:
//
//   1. POWER.  This code assumes socket pin 24 = +5V and pin 12 = GND, which is
//      what the Fire 24 PCB hard-wires to its regulator and ground plane.  If
//      the 1801RE2 puts power anywhere else, the Fire 24 cannot be used
//      unmodified -- do not plug it in.
//
//   2. SIGNALS.  Which socket pin carries nAD0..nAD15, nSYNC, nDIN and nRPLY.
//
//   3. ENABLE.  Which pin the UKNC's port 177054 decode drives to bank this
//      window away.  Getting this wrong does not destroy anything immediately,
//      but it means driving the bus while the machine's RAM is also driving it.
//
// Confirm all three, fill in the tables, then define RE2_PINOUT_CONFIRMED.

#if !defined(RE2_PINOUT_CONFIRMED)
#error "Verify the 1801RE2 power, signal and enable pinout, fill in these tables, then -DRE2_PINOUT_CONFIRMED. See README.md."
#endif

// GPIO number carrying each inverted address/data line, nAD0 first.
#define AD_GPIO { \
    /* nAD0  */ 0, /* nAD1  */ 0, /* nAD2  */ 0, /* nAD3  */ 0, \
    /* nAD4  */ 0, /* nAD5  */ 0, /* nAD6  */ 0, /* nAD7  */ 0, \
    /* nAD8  */ 0, /* nAD9  */ 0, /* nAD10 */ 0, /* nAD11 */ 0, \
    /* nAD12 */ 0, /* nAD13 */ 0, /* nAD14 */ 0, /* nAD15 */ 0 }

// These three must match the .define values in mpi_rom.pio, which the PIO
// assembler bakes into the WAIT instructions.  build_pin_masks() checks this.
#define GPIO_nSYNC   0
#define GPIO_nDIN    0
#define GPIO_nRPLY   0

// Chip enable from the host's window-select logic.  0xFF means "this socket
// has no enable signal" -- only correct if you have checked, because on the
// UKNC every ROM window can be banked out in favour of RAM.
#define GPIO_nSEL              0xFF
#define GPIO_nSEL_ACTIVE_HIGH  0

#endif // BOARD_FIRE24E_H
