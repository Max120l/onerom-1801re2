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
#include "hardware/clocks.h"
#include "pico/bootrom.h"
#include "pico/multicore.h"
#include "hardware/pio.h"

#include "board_fire24e.h"
#include "decode.h"
#include "mpi_rom.pio.h"
#include "rom_images.h"
#include "watch.h"

// The machine waits for our reply, so being slow costs wait states rather than
// data -- but only up to the point where the read strobe has come and gone
// before we answer, and then the cycle gets no reply at all.  The CPU path
// between the address strobe and the pattern reaching the response FIFO is
// what has to fit.
//
// 150 MHz is the RP2350's rated speed and an MS 0511 boots on it, so that is
// the default: running out of spec to buy margin against a deadline we are
// comfortably inside would be paying a real cost for an imagined one.
//
// The status LED is what justifies the choice rather than assumption -- it
// blinks when replies are prepared and not taken. If it ever blinks in normal
// use, raise this; 200000 works at stock voltage and takes about a third off
// the path.
#ifndef MPI_SYS_CLK_KHZ
#define MPI_SYS_CLK_KHZ  150000
#endif

#define SM_CAPTURE  0
#define SM_RESPOND  1

static PIO   g_pio = pio0;
static uint  g_off_capture, g_off_respond;

// Bumped on every cycle we answer.  Core 1 only writes, core 0 only reads, and
// a torn read costs nothing but a slightly wrong blink, so no synchronisation.
static volatile uint32_t g_served;

// Cycles we prepared a reply for that were never taken. Some are normal -- a
// write into our window, or a read the host banked elsewhere after the address
// strobe. But a reply we assembled too late to be sampled also lands here, and
// that is the one failure mode a bench reader cannot show, so it is worth
// counting rather than guessing at.
static volatile uint32_t g_missed;

static mpi_decode_t g_dec;
static uint32_t g_window_store[MPI_MAX_WINDOWS][4096];

#if MPI_WATCH
// One bit per watchpoint, set by core 1 and read by core 0.  Set-only, so the
// worst a race can do is delay a bit's appearance by one frame.
static volatile uint32_t g_watch_hits;

static const mpi_watch_t g_watch[] = MPI_WATCH_PAIRS;
static_assert(count_of(g_watch) <= MPI_WATCH_MAX, "too many watchpoints");

// Bus-side facts, in the same set-only style: see the enum in watch.h.
static volatile uint32_t g_bus_hits;

// Which boot each word of each window was last asked for in.  See watch.h.
//
// A generation tag rather than a bitmap that gets cleared, because the clearing
// is the hard part: the frame has to be reset the instant the machine restarts,
// and that instant is on core 1, between two bus cycles, where a 4 KB memset
// does not fit. Bumping a counter does. It also makes the per-cycle write a
// plain store instead of a read-modify-write.
static uint16_t g_seen_gen[MPI_COVERAGE_WINDOWS][4096];
// Starts at 1, not 0: the tag array is zero-initialised, so a generation of 0
// would make every word of every window read as already covered before the
// machine had asked for anything.
static volatile uint16_t g_boot = 1;

// Every AD line that has ever read back differently from what we drove.
static volatile uint32_t g_mismatch;

static const uint8_t g_ad_gpio[16] = AD_GPIO;

// Lowest-numbered AD line in a mismatch mask.  A wrong bit is worth naming: it
// points at one socket contact rather than at "the bus".
static unsigned mismatch_ad_line(uint32_t mask) {
    for (unsigned i = 0; i < 16; i++) {
        if (mask & (1u << g_ad_gpio[i])) {
            return i;
        }
    }
    return 0;
}

// Coverage is one pulse, not four: all four windows came back complete on
// hardware, so the reading that matters is now "still complete".  The last four
// are a binary number naming the offending AD line, and mean nothing unless the
// mismatch pulse is lit.
#define WATCH_PULSES  (count_of(g_watch) + BUS_EVENT_COUNT + 1 + 4)

static inline void __not_in_flash_func(coverage_note)(uint32_t addr) {
    unsigned w = ((addr >> 13) & 7) - MPI_COVERAGE_FIRST;
    if (w < MPI_COVERAGE_WINDOWS) {
        g_seen_gen[w][(addr >> 1) & 0xFFF] = g_boot;
    }
}

// Has every word this window serves been asked for during this boot?
static bool coverage_complete(unsigned w) {
    unsigned want = g_dec.window_words[w + MPI_COVERAGE_FIRST];
    if (want == 0) {
        return false;               // a window we do not serve cannot be covered
    }
    uint16_t boot = g_boot;
    unsigned have = 0;
    for (unsigned i = 0; i < want; i++) {
        have += (g_seen_gen[w][i] == boot);
    }
    return have >= want;
}

// Called after the reply is queued, never before: a diagnostic build must not
// change the timing of the thing it is measuring.
//
// A watchpoint scores only when its address arrives directly behind its
// predecessor. That adjacency is what distinguishes executing the instruction
// from the checksum reading it, and it is the whole reason this is useful --
// see watch.h.
static inline void __not_in_flash_func(watch_note)(uint32_t addr) {
    static uint32_t prev = 0xFFFFFFFF;
    static bool first = true;

    if (first) {
        first = false;
        if (addr == PP_POWERUP_VECTOR) {
            g_bus_hits |= 1u << BUS_FIRST_IS_VECTOR;
        }
    } else if (addr == PP_RESTART_ADDR && prev == PP_POWERUP_VECTOR) {
        // PC then PSW: the machine has restarted.  Reset the frame here and
        // now, two cycles into the new boot -- deferring it to core 0 meant it
        // landed at the top of the next frame, up to ten seconds later, wiping
        // the whole startup sequence it was supposed to be reporting on.
        g_boot++;
        g_watch_hits = 0;
        g_mismatch = 0;
        g_bus_hits = 1u << BUS_FIRST_IS_VECTOR;   // we saw this boot begin
    }
    for (unsigned i = 0; i < count_of(g_watch); i++) {
        if (addr == g_watch[i].addr && prev == g_watch[i].prev) {
            g_watch_hits |= 1u << i;
        }
    }
    prev = addr;
}
#endif

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
#if MPI_RPLY_ASSIST
    g_off_respond = pio_add_program(g_pio, &mpi_respond_assist_program);
#else
    g_off_respond = pio_add_program(g_pio, &mpi_respond_program);
#endif
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

#define IRQ_SERVED 0

static void __not_in_flash_func(serve_forever)(void) {
    bool armed = false;
#if MPI_WATCH
    uint32_t last_pattern = 0;
#endif
    while (true) {
        uint32_t snap = pio_sm_get_blocking(g_pio, SM_CAPTURE);

        // The response machine raises IRQ_SERVED once the host has taken the
        // data, so a cycle that completed leaves it idle at its PULL with
        // nothing stale to carry forward -- no re-arm needed, which keeps the
        // three register writes off the path that has to finish before the read
        // strobe arrives. Only an abandoned cycle needs clearing up.
        if (pio_interrupt_get(g_pio, IRQ_SERVED)) {
            pio_interrupt_clear(g_pio, IRQ_SERVED);
            armed = false;
        } else if (armed) {
            g_missed++;
#if MPI_WATCH
            g_bus_hits |= 1u << BUS_REPLY_UNTAKEN;
#endif
            rearm_respond();
            armed = false;
        }

        uint32_t addr = mpi_address(&g_dec, snap);
        uint32_t pattern;
        // Most window banking arrives for free: the CGM withholds the read
        // strobe, so we simply never hear a cycle we should not answer.  The
        // exception is the window whose socket carries a real CS, where the
        // strobe does arrive and CE alone says whether the on-board ROM or a
        // cartridge owns the access.  Scope the check to that window only --
        // applying it to all of them would let one deasserted CE silence
        // windows it has no authority over.  Read live rather than from the
        // snapshot: it is sampled later in the cycle, the safe side to be on.
        if (mpi_lookup(&g_dec, addr, &pattern)) {   // ours, and inside the window
            if (!(mpi_cs_gates((addr >> 13) & 7) && !mpi_enabled())) {
                pio_sm_put(g_pio, SM_RESPOND, pattern);
                armed = true;
                g_served++;
#if MPI_WATCH
                coverage_note(addr);    // after the put: never on the reply path

                // The previous cycle's readback: what was actually on the AD
                // lines when the host released the strobe, against what we
                // asked the response machine to drive. Draining it every
                // iteration is not optional -- autopush would stall the state
                // machine on a full FIFO.
                if (!pio_sm_is_rx_fifo_empty(g_pio, SM_RESPOND)) {
                    uint32_t saw = pio_sm_get(g_pio, SM_RESPOND);
                    uint32_t bad = (saw ^ last_pattern) & g_dirs_ad;
                    if (bad) {
                        g_bus_hits |= 1u << BUS_DATA_MISMATCH;
                        g_mismatch |= bad;
                    }
                }
                last_pattern = pattern;
#endif
            }
        }

#if MPI_WATCH
        // Deliberately last: the reply is already on its way, so scoring costs
        // the bus nothing.  Every cycle is offered, including ones we declined
        // to answer -- an address we did not serve is exactly the kind of thing
        // worth being able to see.
        watch_note(addr);
#endif
    }
}

// Is a jumper fitted, whichever rail it ties to?
//
// A floating pin follows whichever internal pull is applied; a pin something
// else is driving does not.  Comparing the two reads therefore detects a fitted
// jumper without needing to know its sense, which is worth having when the
// consequence of getting it backwards is a board that either never runs or
// cannot be recovered.
static bool jumper_fitted(unsigned gpio) {
    gpio_init(gpio);
    gpio_set_dir(gpio, GPIO_IN);
    gpio_pull_down(gpio);
    busy_wait_us(50);
    bool with_pulldown = gpio_get(gpio);
    gpio_pull_up(gpio);
    busy_wait_us(50);
    bool with_pullup = gpio_get(gpio);
    gpio_disable_pulls(gpio);
    return with_pulldown == with_pullup;
}

int main(void) {
    // Before anything else, and before a single socket pin is touched: if the
    // recovery jumper is fitted, hand straight back to the bootrom.  This is
    // the only way back to a flashable board, so it must work even when the
    // rest of this firmware does not.
    if (jumper_fitted(GPIO_RECOVERY_JUMPER)) {
        reset_usb_boot(0, 0);
    }

    set_sys_clock_khz(MPI_SYS_CLK_KHZ, true);

    build_pin_masks();
    mpi_decode_init(&g_dec, mpi_images, mpi_image_count,
                    g_window_store, MPI_MAX_WINDOWS);
    start_pio();

    gpio_init(GPIO_STATUS_LED);
    gpio_set_dir(GPIO_STATUS_LED, GPIO_OUT);

    multicore_launch_core1(serve_forever);

#if MPI_WATCH
    // Blink the watchpoint results out, one frame per pass: a long dark gap to
    // mark the start, then one pulse per watchpoint in table order -- long for
    // hit, short for miss.  Every watchpoint gets a pulse whether or not it hit,
    // so positions cannot be miscounted, which is the failure mode of any
    // scheme that blinks only the hits.
    //
    // Nothing is ever cleared: these are "did this ever happen since power-on"
    // facts, and a frame that changes between passes is itself informative.
    while (true) {
        gpio_put(GPIO_STATUS_LED, STATUS_LED_OFF);
        sleep_ms(1500);                                  // frame marker
        // The frame is reset by core 1 the moment the machine restarts; here we
        // only read it.  See PP_RESTART_ADDR in watch.h.
        uint32_t hits = g_watch_hits | (g_bus_hits << count_of(g_watch));
        bool covered = true;
        for (unsigned w = 0; w < MPI_COVERAGE_WINDOWS; w++) {
            covered = covered && coverage_complete(w);
        }
        if (covered) {
            hits |= 1u << (count_of(g_watch) + BUS_EVENT_COUNT);
        }
        // Trailing nibble: which AD line disagreed, most significant bit first.
        unsigned line = mismatch_ad_line(g_mismatch);
        for (unsigned b = 0; b < 4; b++) {
            if (line & (1u << (3 - b))) {
                hits |= 1u << (count_of(g_watch) + BUS_EVENT_COUNT + 1 + b);
            }
        }
        for (unsigned i = 0; i < WATCH_PULSES; i++) {
            gpio_put(GPIO_STATUS_LED, STATUS_LED_ON);
            // 10:1. An earlier 5:1 against a 400 ms gap read as one steady
            // blink; a pulse you have to time against its neighbours is not a
            // measurement, it is a guess.
            sleep_ms((hits & (1u << i)) ? 1000 : 100);
            gpio_put(GPIO_STATUS_LED, STATUS_LED_OFF);
            sleep_ms(400);
        }
    }
#endif

    // Core 0 turns the served-cycle count into something visible.  Installed in
    // a machine that will not boot, the useful question is whether the board is
    // being asked for anything at all, and a steady light cannot answer it:
    //
    //   dark          nothing is reaching us -- no address strobe, or no power
    //   fast flicker  serving normally
    //   slow blink    a handful of cycles then nothing, i.e. the machine gave up
    uint32_t last_served = 0, last_missed = 0;
    bool phase = false;
    while (true) {
        uint32_t served = g_served, missed = g_missed;
        bool active  = served != last_served;
        bool missing = missed != last_missed;
        phase = !phase;

        // Lit means answering. Blinking means answering but dropping some,
        // which is the interesting failure: replies assembled too late to be
        // taken. Dark means nothing is asking.
        bool on = active && (!missing || phase);
        gpio_put(GPIO_STATUS_LED, on ? STATUS_LED_ON : STATUS_LED_OFF);

        last_served = served;
        last_missed = missed;
        sleep_ms(100);
    }
}
