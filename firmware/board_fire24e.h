// One ROM Fire 24 rev E: the furniture around the socket.
//
// Include board.h, not this file.  The socket-to-GPIO map, the KR1801RE2
// pinout and every signal assignment live there, because rev E and rev F
// share them exactly; this header carries only what the revision changes.
//
// Taken from the One ROM project's own board description,
// rust/config/json/fire-24-e.json.

#ifndef BOARD_FIRE24E_H
#define BOARD_FIRE24E_H

#define BOARD_NAME  "One ROM Fire 24 rev E"

// ---------------------------------------------------------------------------
// Status indicator: a plain LED
// ---------------------------------------------------------------------------
//
// +3V3 -> R5 (1K) -> anode, cathode -> this pin.  It lights when the pin is
// driven LOW.  Rev F replaces it with a WS2812B on the same GPIO, which is
// why every caller goes through status.h rather than gpio_put.
#define BOARD_HAS_NEOPIXEL   0
#define GPIO_STATUS_LED     29
#define STATUS_LED_ON        0
#define STATUS_LED_OFF       1

// ---------------------------------------------------------------------------
// The jumper block
// ---------------------------------------------------------------------------
//
// Four columns, labelled A B C D on the board's underside.  Fitting a jumper
// shorts the two pads of one column.  What those pads are is not uniform, and
// only half the block is usable -- see docs/BOARD-NOTES.md, which records the
// rev F measurement that established it.  The same structure holds here with
// different GPIOs behind the same letters:
//
//   label  GPIO  closed jumper ties to  also wired to    usable?
//     A     25   GND                    --               yes
//     B     24   GND                    --               yes
//     C     26   BOOT (via R2/QSPI_SS)  SWCLK            no
//     D     27   RUN  (10K to +3V3)     SWDIO            no
//
// C and D each appear on a second MCU pin -- the SWD pads -- whose own
// internal pulls fight any pull the firmware applies, so jumper_fitted()
// cannot read them.  Only A and B are clean shorts to ground.
#define GPIO_SEL_JUMPERS  { 25, 24, 26, 27 }

// Jumper A, the recovery jumper.  This firmware carries no USB stack, so
// flashing it replaces One ROM's picoboot, and the board exposes no BOOTSEL
// button -- without an escape hatch the only way back would be SWD on the
// C/D pads.  Fit this jumper and power on: the board goes to the bootrom's
// USB mode instead of touching the bus at all.
#define GPIO_RECOVERY_JUMPER  25

#endif // BOARD_FIRE24E_H
