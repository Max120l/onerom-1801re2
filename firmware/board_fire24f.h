// One ROM Fire 24 rev F: the furniture around the socket.
//
// Include board.h, not this file.  The socket-to-GPIO map, the KR1801RE2
// pinout and every signal assignment live there, because rev E and rev F
// share them exactly -- every signal pin, both X pads included, lands on the
// same GPIO on both revisions.  Only the indicator and the jumper GPIOs move,
// which is why one firmware serves both and choosing a board changes nothing
// on the serving path.
//
// Taken from the One ROM project's own board description,
// rust/config/json/fire-24-f.json, and cross-checked against the rev F
// findings in docs/BOARD-NOTES.md.

#ifndef BOARD_FIRE24F_H
#define BOARD_FIRE24F_H

#define BOARD_NAME  "One ROM Fire 24 rev F"

// ---------------------------------------------------------------------------
// Status indicator: a WS2812B RGB pixel
// ---------------------------------------------------------------------------
//
// A single XL-1010RGBC-WS2812B on GPIO 29, where rev E has a plain LED.  It
// speaks the 800 kHz serial protocol, so gpio_put means nothing to it: it is
// driven by a PIO program on pio1 (serving owns pio0), from core 0.  See
// status.h for the palette and ws2812.pio for the timing.
#define BOARD_HAS_NEOPIXEL   1
#define GPIO_NEOPIXEL       29

// ---------------------------------------------------------------------------
// The jumper block
// ---------------------------------------------------------------------------
//
// The same four columns with the same silkscreen letters as rev E, but the
// GPIOs behind them are rotated.  Measured on rev F hardware; see
// docs/BOARD-NOTES.md for how, and for why half the block is unusable.
//
//   label  GPIO  closed jumper ties to  also wired to    usable?
//     A     26   GND                    --               yes
//     B     27   GND                    --               yes
//     C     25   BOOT (via R2/QSPI_SS)  SWCLK (MCU p23)  no
//     D     24   RUN  (10K to +3V3)     SWDIO (MCU p25)  no
//
// C and D each appear on a second MCU pin -- the SWD pads -- whose own
// internal pulls (SWCLK down, SWDIO up) fight any pull the firmware applies,
// so jumper_fitted() cannot read them: on hardware C reads as permanently
// fitted whether or not it is.  Only A and B are clean shorts to ground.
//
// Note the rev F schematic's SEL_x net names are rotated relative to the
// silkscreen -- net SEL_A lands on the pad labelled D.  Identify jumpers from
// the silkscreen and the PCB netlist, never from the net names.
#define GPIO_SEL_JUMPERS  { 26, 27, 25, 24 }

// Jumper A, the recovery jumper -- the same letter as rev E, a different GPIO.
// Fit it and power on to reach the bootrom's USB mode.  See board_fire24e.h
// and README.md for why this exists.
#define GPIO_RECOVERY_JUMPER  26

#endif // BOARD_FIRE24F_H
