// The rev F status pixel.  Empty on rev E, where status.h inlines a gpio_put.

#include "status.h"

#if BOARD_HAS_NEOPIXEL

#include "hardware/clocks.h"
#include "hardware/pio.h"
#include "pico/time.h"

#include "ws2812.pio.h"

// pio1, not pio0.  Serving has both of pio0's state machines and its
// instruction memory, and the two blocks share no state at all -- so the one
// piece of this firmware with a hard deadline cannot be perturbed by the one
// piece that is purely cosmetic.  Cheap insurance: PIO blocks are not scarce
// here, and the alternative is a class of bug that would show up as
// intermittent bus timing, which is precisely the thing this project exists
// to be trusted about.
#define NEO_PIO  pio1
#define NEO_SM   0

static uint32_t g_style = STATUS_DATA;

void status_init(void) {
    uint off = pio_add_program(NEO_PIO, &ws2812_program);
    pio_gpio_init(NEO_PIO, GPIO_NEOPIXEL);
    pio_sm_set_consecutive_pindirs(NEO_PIO, NEO_SM, GPIO_NEOPIXEL, 1, true);

    pio_sm_config c = ws2812_program_get_default_config(off);
    sm_config_set_sideset_pins(&c, GPIO_NEOPIXEL);
    sm_config_set_out_shift(&c, false, true, 24);   // MSB first, autopull at 24
    sm_config_set_fifo_join(&c, PIO_FIFO_JOIN_TX);
    // Ten PIO cycles per bit at 800 kHz, from whatever the system clock is --
    // MPI_SYS_CLK_KHZ moves it, and a hardcoded divider would silently
    // detune the pixel on a 200 MHz build.
    sm_config_set_clkdiv(&c, (float)clock_get_hz(clk_sys) / (800000.0f * 10));

    pio_sm_init(NEO_PIO, NEO_SM, off, &c);
    pio_sm_set_enabled(NEO_PIO, NEO_SM, true);

    status_set(false);
}

void status_style(uint32_t grb) {
    g_style = grb;
}

void status_set(bool lit) {
    pio_sm_put_blocking(NEO_PIO, NEO_SM, (lit ? g_style : 0u) << 8u);
}

#endif  // BOARD_HAS_NEOPIXEL
