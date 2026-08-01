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
#include "mpi_rom.pio.h"
#include "rom_images.h"

#define SM_CAPTURE  0
#define SM_RESPOND  1

static PIO   g_pio = pio0;
static uint  g_off_capture, g_off_respond;

static const uint8_t g_ad_gpio[16] = AD_GPIO;

// Direction masks over the 24-bit GPIO field.
static uint32_t g_dirs_ad, g_dirs_ad_rply;

// Snapshot byte -> partial logical address.  Three tables, one per byte of the
// 24-bit GPIO snapshot, each contributing the AD bits that live in that byte.
// Pin inversion is baked in: the tables yield the CPU-level address directly.
static uint16_t g_gather[3][256];

// One 4096-entry table per chip code, or NULL if this board does not answer
// for that 8 KB window.  Each entry is the finished 24-bit GPIO pattern: data
// inverted and scattered onto the AD pins, nRPLY bit low.  Nothing is computed
// per cycle.
static const uint32_t *g_window[8];
static uint16_t        g_window_words[8];   // 0 == window not served
static uint32_t g_window_store[MPI_MAX_WINDOWS][4096];

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

    g_dirs_ad = 0;
    for (int i = 0; i < 16; i++) {
        g_dirs_ad |= 1u << g_ad_gpio[i];
    }
    g_dirs_ad_rply = g_dirs_ad | (1u << GPIO_nRPLY);

    // GPIO 8 and 9 are the X jumper pads.  A fitted jumper ties them to a rail,
    // so they must never become outputs.
    hard_assert((g_dirs_ad_rply & ((1u << GPIO_X1) | (1u << GPIO_X2))) == 0);
}

static void build_gather_tables(void) {
    memset(g_gather, 0, sizeof(g_gather));
    for (int bit = 0; bit < 16; bit++) {
        uint32_t gpio = g_ad_gpio[bit];
        uint32_t byte = gpio / 8, pos = gpio % 8;
        for (int v = 0; v < 256; v++) {
            // Pins are inverted: a low pin is a logical 1.
            if (!((v >> pos) & 1)) {
                g_gather[byte][v] |= 1u << bit;
            }
        }
    }
}

// Turn a 16-bit ROM word into the GPIO pattern that presents it on the bus.
static uint32_t drive_pattern(uint16_t word) {
    uint32_t pattern = 0;
    for (int bit = 0; bit < 16; bit++) {
        if (!((word >> bit) & 1)) {     // inverted on the wire
            pattern |= 1u << g_ad_gpio[bit];
        }
    }
    return pattern;                      // nRPLY bit stays 0 == asserted low
}

static void build_windows(void) {
    memset(g_window, 0, sizeof(g_window));
    memset(g_window_words, 0, sizeof(g_window_words));
    for (unsigned i = 0; i < MPI_MAX_WINDOWS && i < mpi_image_count; i++) {
        const mpi_image_t *img = &mpi_images[i];
        for (int w = 0; w < img->word_count; w++) {
            g_window_store[i][w] = drive_pattern(img->words[w]);
        }
        g_window[img->code & 7] = g_window_store[i];
        g_window_words[img->code & 7] = img->word_count;
    }
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

// Is this chip currently selected by the host?  A mask ROM has no such logic
// of its own, so on any machine that can bank the window away -- the UKNC does,
// per window, via port 177054 -- the socket must be carrying an enable from
// external decode logic.  Set GPIO_nSEL to 0xFF only if you have confirmed
// there is no such signal.
static inline bool mpi_enabled(void) {
#if GPIO_nSEL == 0xFF
    return true;
#elif GPIO_nSEL_ACTIVE_HIGH
    return gpio_get(GPIO_nSEL);
#else
    return !gpio_get(GPIO_nSEL);
#endif
}

// A cycle we answered but that turned out not to be a read -- a write to our
// window, or a host that abandoned the transfer -- leaves the response machine
// holding a pattern it would wrongly apply to the next read.  Once nSYNC
// releases without nDIN having fired, drop it.
static void discard_stale_response(void) {
    if (pio_sm_is_tx_fifo_empty(g_pio, SM_RESPOND)) {
        return;
    }
    if (gpio_get(GPIO_nSYNC)) {          // high == cycle over
        pio_sm_clear_fifos(g_pio, SM_RESPOND);
        pio_sm_restart(g_pio, SM_RESPOND);
        pio_sm_exec(g_pio, SM_RESPOND, pio_encode_jmp(g_off_respond));
    }
}

static void __not_in_flash_func(serve_forever)(void) {
    while (true) {
        while (pio_sm_is_rx_fifo_empty(g_pio, SM_CAPTURE)) {
            discard_stale_response();
        }
        uint32_t snap = pio_sm_get(g_pio, SM_CAPTURE);

        uint32_t addr = g_gather[0][snap & 0xFF]
                      | g_gather[1][(snap >> 8) & 0xFF]
                      | g_gather[2][(snap >> 16) & 0xFF];

        unsigned code = (addr >> 13) & 7;
        // Word addressing: AD0 selects the byte and is not decoded here.
        unsigned word = (addr >> 1) & 0xFFF;

        // word_count stops the code 0 chip short of the I/O page.  Answering
        // there would put us in a driver fight with the machine's own
        // registers, so it is checked before anything is pushed.
        if (word >= g_window_words[code]) {
            continue;
        }
        // Host logic can bank RAM in over a ROM window (on the UKNC, via port
        // 177054).  Read the enable live rather than from the snapshot: it is
        // sampled later in the cycle, which is the safe side to be on if the
        // host derives it combinationally from the address.
        if (!mpi_enabled()) {
            continue;
        }
        pio_sm_put(g_pio, SM_RESPOND, g_window[code][word]);
    }
}

int main(void) {
    build_pin_masks();
    build_gather_tables();
    build_windows();
    start_pio();

    gpio_init(GPIO_STATUS_LED);
    gpio_set_dir(GPIO_STATUS_LED, GPIO_OUT);
    gpio_put(GPIO_STATUS_LED, 1);

    multicore_launch_core1(serve_forever);
    while (true) {
        tight_loop_contents();           // core 0 is free for USB, config, etc.
    }
}
