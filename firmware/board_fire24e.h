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
// KR1801RE2 chip pinout
// ---------------------------------------------------------------------------
//
// From the KR1801RE2 datasheet, table 11.26 and figure 11.30.  Power follows
// JEDEC and matches what the Fire 24 PCB hard-wires: pin 24 = Ucc, pin 12 =
// GND, so the board can be used unmodified.
//
//   pin  signal          pin  signal          pin  signal
//    1   RD   (nDIN)      9   AD9             17   AD12
//    2   AN   (nRPLY)    10   AD10            18   AD13
//    3   SYN  (nSYNC)    11   AD11            19   AD14
//    4   AD4             12   GND             20   AD15
//    5   AD5             13   AD3             21   n/c
//    6   AD6             14   AD2             22   n/c
//    7   AD7             15   AD1             23   CS
//    8   AD8             16   AD0             24   Ucc
//
// All signals are active low / inverted, which is why the k1801 RTL names them
// nAD, nSYNC, nDIN, nRPLY.  AN on pin 2 is the reply -- nRPLY, asserted by the
// ROM to complete a transfer.  Table 11.26 lists it as an input, which is a
// misprint; figure 11.30 draws it on the output side.  It is driven here, open
// drain: see GPIO_nRPLY below and the direction masks in main.c.
//
// Cross-checked against the MS 0511 schematic (sheet 1, DS1-DS4), which wires
// all four ROMs exactly as above and settles two things:
//
//   CS is active low.  DS1, DS2 and DS3 have pin 23 strapped straight to
//   ground, i.e. permanently selected.  Only DS4 -- the 205, covering the
//   switchable 100000 window -- has it driven, from the CGM's CE0.
//
//   Pin 1 is fed by EDIN, not by the raw K1DIN net.  EDIN comes off the CGM's
//   output side, a read strobe already qualified by the port 177054 banking,
//   so a window switched to RAM simply never strobes its ROM.  That is the
//   real per-window gate; CS only arbitrates the one switchable window.

// GPIO number carrying each inverted address/data line, nAD0 first.  Order is
// scrambled relative to the socket, which costs nothing: the address is
// unscrambled through a lookup table and the data is pre-scrambled at boot.
//
// Note none of these land on GPIO 8 or 9, so all sixteen AD lines sit inside
// the 24-bit field the PIO reads and writes.
#define AD_GPIO { \
    /* nAD0  */  3, /* nAD1  */  2, /* nAD2  */  1, /* nAD3  */  0, \
    /* nAD4  */ 19, /* nAD5  */ 20, /* nAD6  */ 21, /* nAD7  */ 22, \
    /* nAD8  */ 23, /* nAD9  */  7, /* nAD10 */  6, /* nAD11 */  5, \
    /* nAD12 */  4, /* nAD13 */ 11, /* nAD14 */ 13, /* nAD15 */ 10 }

// These three must match the .define values in mpi_rom.pio, which the PIO
// assembler bakes into the WAIT instructions.  build_pin_masks() checks this.
#define GPIO_nSYNC  18    // SYN, socket pin 3
#define GPIO_nDIN   16    // RD,  socket pin 1
#define GPIO_nRPLY  17    // AN,  socket pin 2

// CS, socket pin 23.  Active low, per the schematic strapping it to ground on
// three of the four sockets.  Only meaningful in the DS4 socket, where the CGM
// drives it from CE0; elsewhere it reads permanently selected, which is right.
#define GPIO_nSEL              15
#define GPIO_nSEL_ACTIVE_HIGH  0

// Which chip's window this socket's CS gates, as a chip code -- or 0xFF for a
// socket that straps CS to ground.
//
// This matters as soon as one board answers for more than one window.  CE0 in
// the DS4 socket means "the 100000 window belongs to the on-board ROM rather
// than to RAM or a cartridge"; it says nothing about the other three windows,
// which have no CE of their own and are gated by EDIN alone.  Applying CS
// globally would take all four windows down whenever the machine banked
// something else into 100000.
//
//   DS4 (the 205, code 011) -> 03
//   DS1, DS2, DS3           -> 0xFF
#define SOCKET_CS_CODE         03

// Socket pins 21 and 22 are not connected.  They are inside the 24-bit field
// but contribute to no address bit, so they only need a pull to stop them
// floating.
#define UNUSED_SOCKET_GPIOS { 12, 14 }

#endif // BOARD_FIRE24E_H
