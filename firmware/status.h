// The status indicator, whichever kind this board has.
//
// Rev E carries a plain LED on GPIO 29; rev F carries a WS2812B RGB pixel on
// the same GPIO, which needs an 800 kHz serial protocol and ignores gpio_put
// entirely.  Everything that reports through the light goes through here so
// that neither the serving loop nor a single line of diag.c has to know.
//
// The interface is deliberately still one bit.  Every frame in this firmware
// is read as *long pulse versus short pulse*, and all of docs/DIAGNOSTICS.md
// is written that way -- so colour is not allowed to become the data channel,
// or a rev F board and a rev E board would need two sets of reading
// instructions for the same firmware.  Colour says what *kind* of thing is
// being shown; the pulse still says what it is.
//
//   status_style(slot)   choose the colour subsequent lit periods use
//   status_set(lit)      light it, or not
//
// On rev E status_style() compiles to nothing and status_set() is the same
// gpio_put it always was, so the plain rev E build is byte-for-byte what it
// was before this file existed.
//
// Core 0 only.  The serving loop on core 1 never touches the light, and the
// rev F driver's PIO FIFO has no locking -- which costs nothing, because a
// status light is exactly the kind of work that has no business on the core
// with a bus deadline.

#ifndef STATUS_H
#define STATUS_H

#include <stdbool.h>
#include <stdint.h>

#include "board.h"

// ---------------------------------------------------------------------------
// The palette
// ---------------------------------------------------------------------------
//
// GRB, because that is the order a WS2812B wants on the wire.  Kept dim on
// purpose: these pixels are floodlights at 0xFF, and a frame is something you
// sit and watch for half a minute.  If the colours come out permuted, the
// pixel is one of the clone parts that wants RGB order -- that is the first
// thing to check, and the only thing to change.
#define STATUS_GRB(g, r, b) \
    (((uint32_t)(g) << 16) | ((uint32_t)(r) << 8) | (uint32_t)(b))

// Frame markers.  The 2.5 s lit block that starts a frame, coloured by which
// instrument is running -- eleven build variants report through this one
// light, and on rev E there is no way to tell from the board which is
// flashed.  It cannot mislead: the marker carries no data, only identity.
#define STATUS_MARK_WATCH    STATUS_GRB(0x00, 0x00, 0x18)  // blue    watchpoints
#define STATUS_MARK_BEACON   STATUS_GRB(0x10, 0x00, 0x14)  // cyan    a test ROM
#define STATUS_MARK_ADDR     STATUS_GRB(0x00, 0x12, 0x14)  // magenta an address
#define STATUS_MARK_BITS     STATUS_GRB(0x10, 0x14, 0x14)  // white   plane 0 bits

// Frame data.  One colour for every pulse, set or clear: the length is the
// data, as it is on rev E, as the documentation says.
#define STATUS_DATA          STATUS_GRB(0x0E, 0x12, 0x08)

// Verdicts, for the modes that really are pass/fail -- the freeze-spray lamp
// and the soak digit.  Here good and bad are unambiguous, so colour can carry
// it, and on rev F the lamp stops needing the "steady means bad" convention
// that reads backwards to everyone who meets it.
#define STATUS_GOOD          STATUS_GRB(0x18, 0x00, 0x00)  // green
#define STATUS_BAD           STATUS_GRB(0x00, 0x18, 0x00)  // red
#define STATUS_STUCK         STATUS_GRB(0x0A, 0x18, 0x00)  // amber

// The plain build's serving light.  Rev E has three states and one of them is
// darkness, so "nothing is asking" and "the board is dead" look identical.
// Rev F can tell them apart, which is worth having the moment a machine comes
// up silent.
#define STATUS_BOOT          STATUS_GRB(0x00, 0x00, 0x18)  // blue: firmware alive
#define STATUS_SERVING       STATUS_GRB(0x18, 0x00, 0x00)  // green: answering
#define STATUS_DROPPING      STATUS_GRB(0x0A, 0x18, 0x00)  // amber: replies untaken
#define STATUS_IDLE          STATUS_GRB(0x00, 0x06, 0x00)  // faint red: powered

#if BOARD_HAS_NEOPIXEL

void status_init(void);
void status_style(uint32_t grb);
void status_set(bool lit);

#else

#include "hardware/gpio.h"

static inline void status_init(void) {
    gpio_init(GPIO_STATUS_LED);
    gpio_set_dir(GPIO_STATUS_LED, GPIO_OUT);
    gpio_put(GPIO_STATUS_LED, STATUS_LED_OFF);
}

static inline void status_style(uint32_t grb) { (void)grb; }

static inline void status_set(bool lit) {
    gpio_put(GPIO_STATUS_LED, lit ? STATUS_LED_ON : STATUS_LED_OFF);
}

#endif  // BOARD_HAS_NEOPIXEL

#endif  // STATUS_H
