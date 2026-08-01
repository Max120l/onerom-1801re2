// 1801RE2 mask ROM emulation on a One ROM Fire 24 rev E.
//
// The 1801RE2 is a 4K x 16 mask ROM that sits directly on the MPI bus, the
// Soviet equivalent of DEC's Q-bus: address and data share sixteen inverted
// AD lines, and a transfer completes only when the slave pulls nRPLY low.
// Emulating it is therefore nothing like emulating a 2364.  There is no chip
// select and no fixed access time; instead there is a three-phase cycle
//
//     nSYNC asserts, address on AD  ->  nDIN asserts  ->  slave drives data
//     and nRPLY  ->  nDIN releases  ->  slave releases  ->  nSYNC releases
//
// and the host waits for nRPLY. Being late costs wait states, not corruption,
// which is what makes a CPU-in-the-loop design viable here.
//
// Pin-level timing lives in PIO (see mpi_rom.pio).  Core 1 does the address
// decode between the nSYNC and nDIN edges.

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "hardware/pio.h"

#include "board_fire24e.h"
#include "decode.h"
#include "mpi_rom.pio.h"
#include "rom_images.h"

#define SM_CAPTURE  0
#define SM_RESPOND  1

static PIO   g_pio = pio0;
static uint  g_off_capture, g_off_respond;

static mpi_decode_t g_dec;
static uint32_t g_window_store[MPI_MAX_WINDOWS][4096];

// Direction masks over the 24-bit GPIO field.
static uint32_t g_dirs_ad, g_dirs_ad_rply;

// ---------------------------------------------------------------------------
// Setup
// ---------------------------------------------------------------------------

static void build_pin_masks(void) {
    // The PIO assembler baked the control pin numbers into WAIT instructions,
    // so a mismatch with the board header would be silent and fatal.  These
    // names come from the file-scope `.define public`s in mpi_rom.pio.
    static_assert(GPIO_nSYNC == PIN_nSYNC, "nSYNC pin mismatch");
    static_assert(GPIO_nDIN  == PIN_nDIN,  "nDIN pin mismatch");
    static_assert(GPIO_nRPLY == PIN_nRPLY, "nRPLY pin mismatch");

    mpi_direction_masks(&g_dirs_ad, &g_dirs_ad_rply);

    // GPIO 8 and 9 are the X jumper pads.  A fitted jumper ties them to a rail,
    // so they must never become outputs.
    hard_assert((g_dirs_ad_rply & ((1u << GPIO_X1) | (1u << GPIO_X2))) == 0);
}

static void start_pio(void) {
    for (uint gpio = 0; gpio < 24; gpio++) {
        if (gpio == GPIO_X1 || gpio == GPIO_X2) {
            continue;                    // never hand the jumper pads to PIO
        }
        pio_gpio_init(g_pio, gpio);
        gpio_set_input_enabled(gpio, true);
        // 8 mA matches what One ROM uses for 5 V hosts; see docs/VOLTAGE-LEVELS.
        gpio_set_drive_strength(gpio, GPIO_DRIVE_STRENGTH_8MA);
    }
    // Socket pins 21 and 22 are not connected to anything in the host, so pull
    // them somewhere definite rather than leaving CMOS inputs floating.  They
    // feed no address bit, so their state cannot affect decoding either way.
    static const uint8_t unused[] = UNUSED_SOCKET_GPIOS;
    for (unsigned i = 0; i < count_of(unused); i++) {
        gpio_pull_down(unused[i]);
    }

#if GPIO_nSEL != 0xFF && !GPIO_nSEL_ACTIVE_HIGH
    // Pull CS to its asserted level, so a socket that does not drive it behaves
    // like the three UKNC sockets that strap it to ground.  This matters off
    // the machine: a rig that leaves pin 23 open would otherwise get silence
    // from us where a real chip could have answered by luck.  A weak internal
    // pull loses to the CGM's driver, so it changes nothing in the one socket
    // where CS is real.
    gpio_pull_down(GPIO_nSEL);
#endif

    // Everything starts released.  The bus must never see a driver until the
    // address has been decoded as ours.
    pio_sm_set_pindirs_with_mask(g_pio, SM_RESPOND, 0, g_dirs_ad_rply);

    g_off_capture = pio_add_program(g_pio, &mpi_capture_program);
    g_off_respond = pio_add_program(g_pio, &mpi_respond_program);
    mpi_capture_init(g_pio, SM_CAPTURE, g_off_capture);
    mpi_respond_init(g_pio, SM_RESPOND, g_off_respond, g_dirs_ad, g_dirs_ad_rply);
    pio_set_sm_mask_enabled(g_pio, (1u << SM_CAPTURE) | (1u << SM_RESPOND), true);
}

// ---------------------------------------------------------------------------
// Serving
// ---------------------------------------------------------------------------

// Is this chip currently selected?  On the UKNC only the DS4 socket -- the
// 205, covering the switchable 100000 window -- has this wired to anything; the
// CGM drives it from CE0.  DS1 to DS3 have pin 23 strapped to ground, so this
// reads permanently selected there, which is correct.
static inline bool mpi_enabled(void) {
#if GPIO_nSEL == 0xFF
    return true;
#elif GPIO_nSEL_ACTIVE_HIGH
    return gpio_get(GPIO_nSEL);
#else
    return !gpio_get(GPIO_nSEL);
#endif
}

// Put the response machine back to a known idle before preparing a cycle.
//
// This is load-bearing, not tidiness.  We latch on the address strobe, which is
// asserted for every cycle on the bus, but the read strobe only arrives for a
// read the host has decided belongs to us -- on the UKNC it is EDIN, which the
// CGM withholds when the window is banked to RAM.  So a prepared cycle
// routinely ends without ever being served: writes, cycles for other devices,
// banked-out windows.
//
// Once the machine has executed its PULL, the pattern is in the OSR and the TX
// FIFO reads empty, so neither a FIFO check nor watching the address strobe can
// tell that it is sitting on a stale value.  It would then apply that value to
// whatever read came next -- wrong data, driven confidently.  Re-arming
// unconditionally costs a handful of register writes inside the strobe-to-
// strobe gap and makes the machine stateless across cycles.
static void __not_in_flash_func(rearm_respond)(void) {
    pio_sm_clear_fifos(g_pio, SM_RESPOND);
    pio_sm_restart(g_pio, SM_RESPOND);
    pio_sm_exec(g_pio, SM_RESPOND, pio_encode_jmp(g_off_respond));
}

static void __not_in_flash_func(serve_forever)(void) {
    while (true) {
        uint32_t snap = pio_sm_get_blocking(g_pio, SM_CAPTURE);
        rearm_respond();

        uint32_t addr = mpi_address(&g_dec, snap);
        uint32_t pattern;
        if (!mpi_lookup(&g_dec, addr, &pattern)) {
            continue;       // not ours, or past the end of our window
        }
        // Most window banking arrives for free: the CGM withholds the read
        // strobe, so we simply never hear a cycle we should not answer.  The
        // exception is the window whose socket carries a real CS, where the
        // strobe does arrive and CE alone says whether the on-board ROM or a
        // cartridge owns the access.  Scope the check to that window only --
        // applying it to all of them would let one deasserted CE silence
        // windows it has no authority over.  Read live rather than from the
        // snapshot: it is sampled later in the cycle, the safe side to be on.
        if (mpi_cs_gates((addr >> 13) & 7) && !mpi_enabled()) {
            continue;
        }
        pio_sm_put(g_pio, SM_RESPOND, pattern);
    }
}

int main(void) {
    build_pin_masks();
    mpi_decode_init(&g_dec, mpi_images, mpi_image_count,
                    g_window_store, MPI_MAX_WINDOWS);
    start_pio();

    gpio_init(GPIO_STATUS_LED);
    gpio_set_dir(GPIO_STATUS_LED, GPIO_OUT);
    gpio_put(GPIO_STATUS_LED, 1);

    multicore_launch_core1(serve_forever);
    while (true) {
        tight_loop_contents();           // core 0 is free for USB, config, etc.
    }
}
