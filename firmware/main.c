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

// The CMake option resolves this, but a hand-rolled -D would not, and the
// failure would be a firmware that compiles and reports nothing -- which is the
// exact shape of the MPI_BEACONS-without-MPI_WATCH bug, already made once.
#if (MPI_BEACON_LIVE || MPI_BEACON_PASS) && !MPI_BEACONS
#error "MPI_BEACON_LIVE/MPI_BEACON_PASS need MPI_BEACONS; use the CMake options"
#endif
#if MPI_BEACON_LIVE && MPI_BEACON_PASS
#error "MPI_BEACON_LIVE and MPI_BEACON_PASS are two readings of one LED"
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

// The address of the cycle immediately before the last restart.  See watch_note.
static volatile uint32_t g_kill_addr;

// ...and the most recent address, restart or no restart.
//
// The restart version answers only one of the three ways this machine stops. It
// can also wedge on a cycle that never completes, or run off into RAM, and both
// leave that frame dark -- an instrument reporting nothing about a machine that
// has plainly failed.
//
// This one covers all three, because the capture machine latches on the address
// strobe: a PP waiting forever for a reply has already put its address on the
// bus, so the last thing we saw IS the cycle it is stuck on. While the machine
// runs the value churns and the frame differs every time; the moment it stops,
// the frame freezes on the answer. "Has it stopped" and "where" become the same
// reading.
static volatile uint32_t g_last_addr;

// Which block the checksum was comparing when it last found a mismatch, and the
// stored sum most recently read.  See CHK_CMP_EXT in watch.h.
static volatile uint32_t g_last_sum = CHK_SUM_LOW;
static volatile unsigned g_fail_block;

// Coverage is one pulse, not four: all four windows came back complete on
// hardware, so the reading that matters is now "still complete".  The last two
// name the block whose checksum failed, and mean nothing unless pulse 2 is lit.
//
// The AD-line nibble that used to sit here is gone: the mismatch pulse has been
// dark on every reading, so four pulses were being spent decoding a number that
// was always zero. It comes back if that ever changes.
#if MPI_BEACONS
// Beacon mode: the ROM we are serving is our own diagnostic, so the frame is
// its report rather than the stock monitor's behaviour.  One pulse per beacon.
static volatile uint32_t g_beacons;
#if MPI_BEACON_PASS
#define WATCH_PULSES  (PP_BEACON_COUNT + 9)   // restarted, PP RAM, vector, drive
#else
#define WATCH_PULSES  PP_BEACON_COUNT
#endif
#if MPI_BEACON_LIVE || MPI_BEACON_PASS
// The last completed pass, and a counter so core 0 can tell a new verdict from
// a repeat of the old one.  See PP_BEACON_DONE in watch.h.
static volatile uint32_t g_beacons_last;
static volatile uint32_t g_pass;
#endif
#if MPI_BEACON_PASS
// Did the PP touch PP RAM?
//
// The address-bus test never does. It runs entirely out of ROM and speaks only
// to 177010, 177014, 177716 and its own beacons -- every one of those above
// 0100000. So a single cycle below that line means the PP is no longer running
// our program: it trapped, most likely on a bus error, through a vector in RAM
// that holds garbage, and is off executing whatever it found.
//
// That is otherwise invisible. A derailed PP keeps the bus busy, so "there is
// activity" says nothing, and it emits no beacons, so the frame simply stops --
// which looks identical to a clean hang.
static volatile bool g_saw_pp_ram;

// ...and which trap took it there.
//
// A trapping PP reads its new PC from a vector in the first few words of memory,
// and the vector names the fault: 4 is a bus timeout -- a cycle nobody answered
// -- while 10 is an illegal instruction, which would mean it was already
// executing rubbish before it got here. Those are different diagnoses.
//
// Only the vector page counts. The trap also pushes the old PC and PSW, and
// those writes are cycles below 0100000 too; the ROM parks the stack pointer
// high so they cannot be mistaken for a vector fetch.
#define PP_VECTOR_PAGE  01000
// Latched for the life of the power session, not the frame.
//
// Cleared per frame, this named an arbitrary trap out of the storm that follows
// the first one -- and hardware duly reported vector 14, which may be what
// started the trouble or may be the noise afterwards. A processor already
// executing garbage takes traps constantly; only the first one is evidence.
static volatile uint32_t g_trap_vec;
// ...with its own flag, because 0 is a real answer. Testing g_trap_vec for
// emptiness made a vector fetch of 0 indistinguishable from no fetch at all, so
// the next address overwrote it -- exactly the case that matters, since a PC
// gone to zero walks 0, 2, 4 in order and the first of those is the finding.
static volatile bool g_trap_vec_seen;
#endif
#else
#define WATCH_PULSES  (count_of(g_watch) + BUS_EVENT_COUNT + 1 + 2)
#endif

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
    static uint32_t prev2 = 0xFFFFFFFF;
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
        // The cycle before the restart is the one that killed it. A 1801 that
        // meets a bus condition it cannot survive re-enters through its power-up
        // vector with no ACLO or DCLO involved, so from outside the only trace
        // of what went wrong is the address it was working on when it went.
        //
        // Except at power-on, where the restart is real but there is no history
        // behind it -- the vector fetch is the first thing the PP ever does. The
        // sentinel then got blinked out as 177777, an address that looks like a
        // finding and is not. A machine that has not faulted yet must produce a
        // dark frame, or the instrument answers before the question is asked.
        if (prev2 != 0xFFFFFFFF) {
            g_kill_addr = prev2;
        }
        g_boot++;
        g_watch_hits = 0;
        g_mismatch = 0;
#if MPI_BEACONS
        g_beacons = 0;      // the test ROM re-runs from the top; so does its report
#endif
        g_fail_block = 0;
        g_last_sum = CHK_SUM_LOW;
        g_bus_hits = 1u << BUS_FIRST_IS_VECTOR;   // we saw this boot begin
    }
#if MPI_BEACONS
    // A beacon is a read of a reserved PP RAM address.  We never serve those
    // addresses -- they are below our windows -- but the capture machine sees
    // every strobe on the bus, so the read is visible anyway.
#if MPI_BEACON_PASS
    if (addr < 0100000) {
        if (addr < PP_VECTOR_PAGE && !g_trap_vec_seen) {
            g_trap_vec = addr;      // first vector fetch of this frame
            g_trap_vec_seen = true;
        }
        g_saw_pp_ram = true;
    }
#endif
    uint32_t off = addr - PP_BEACON_BASE;
    if (off < 2 * PP_BEACON_COUNT) {
        unsigned b = off >> 1;
        g_beacons |= 1u << b;
#if MPI_BEACON_LIVE || MPI_BEACON_PASS
        // End of a pass: publish it and start the next one empty. Done here
        // rather than on core 0 so the snapshot and the clear cannot be split
        // by a pass boundary, which would drop a whole pass's verdict.
        if (b == PP_BEACON_DONE) {
            g_beacons_last = g_beacons;
            g_beacons = 0;
            g_pass++;
        }
#endif
    }
#endif

    // The checksum's compare reads a different stored sum per block, directly
    // behind the fetch of the compare's second word.  Remember which.
    if (prev == CHK_CMP_EXT && addr >= CHK_SUM_LOW && addr <= CHK_SUM_LOW + 6) {
        g_last_sum = addr;
    } else if (addr == CHK_FAIL_ADDR && prev == CHK_FAIL_PREV) {
        g_fail_block = (g_last_sum - CHK_SUM_LOW) >> 1;
    }

    for (unsigned i = 0; i < count_of(g_watch); i++) {
        if (addr == g_watch[i].addr && prev == g_watch[i].prev) {
            g_watch_hits |= 1u << i;
        }
    }
    g_last_addr = addr;
    prev2 = prev;
    prev = addr;
}
#endif

// The start of a frame, made unmistakable.
//
// It used to be a long dark gap, and once the pulses were grouped -- also by
// dark gaps -- there were three lengths of darkness in the frame and no way to
// tell which one was the start. A frame whose beginning you cannot find is
// worse than an ungrouped one, which is what grouping was meant to fix.
//
// So the marker is *lit*. Everything structural here is dark and no data pulse
// exceeds 700 ms, so two and a half seconds of solid LED is the one event in a
// frame that cannot be mistaken for anything else in it.
static void __not_in_flash_func(frame_marker)(void) {
    gpio_put(GPIO_STATUS_LED, STATUS_LED_ON);
    sleep_ms(2500);
    gpio_put(GPIO_STATUS_LED, STATUS_LED_OFF);
    sleep_ms(800);
}

// One pulse of a frame, with the counting made possible.
//
// Frames reached thirty pulses at a uniform cadence, which is not readable by a
// human being: the reader has to hold a running count across half a minute and a
// single miscount silently renames every pulse after it. On hardware that is not
// a theoretical risk -- a frame was read as 22/24/26 when it was 22/23/24/26,
// and the two decode to different faults.
//
// So group them in fives with a longer gap between groups, the way anyone reads
// a long number. "Group four, pulse two" needs no running count and survives
// looking away.
static void __not_in_flash_func(frame_pulse)(bool lit, unsigned index) {
    gpio_put(GPIO_STATUS_LED, STATUS_LED_ON);
    // 7:1 -- long enough to be unmistakable without making a frame interminable.
    sleep_ms(lit ? 700 : 100);
    gpio_put(GPIO_STATUS_LED, STATUS_LED_OFF);
    sleep_ms(((index + 1) % 5 == 0) ? 900 : 300);
}

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

        // Drain the previous cycle's readback, unconditionally.
        //
        // The response program samples the bus on every served cycle and
        // autopushes it, so something has to empty that FIFO whether or not
        // anyone is looking at the contents. This drain used to sit inside
        // #if MPI_WATCH, which meant the plain build never emptied it: four
        // served cycles filled it, autopush stalled the state machine at the
        // "in", and the board stopped answering the bus altogether. The comment
        // right here said it was not optional, and it was behind a conditional.
        //
        // Order matters too. The bus is sampled early -- at the moment the reply
        // is asserted, not at the end of the cycle -- so a readback taken after
        // the put below could be this cycle's, compared against last cycle's
        // pattern, and report a mismatch that never happened. Draining before
        // the machine has been given anything to drive makes that impossible.
        if (!pio_sm_is_rx_fifo_empty(g_pio, SM_RESPOND)) {
            uint32_t saw = pio_sm_get(g_pio, SM_RESPOND);
#if MPI_WATCH
            uint32_t bad = (saw ^ last_pattern) & g_dirs_ad;
            if (bad) {
                g_bus_hits |= 1u << BUS_DATA_MISMATCH;
                g_mismatch |= bad;
            }
#else
            (void)saw;
#endif
        }

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

#if MPI_BEACON_KILLADDR || MPI_BEACON_LASTADDR
    // Sixteen pulses, and they are one number: the address the PP was working on
    // when it last restarted itself, or -- with MPI_BEACON_LASTADDR -- the last
    // address it put on the bus at all.
    //
    // Everything else in this file reports what a test found. This reports what
    // the machine was doing at the instant it stopped being able to run, which
    // is the only question left once the test is clean and the machine restarts
    // anyway. Dark frame means no restart since the last one -- self-indicating,
    // so no pulse is spent saying whether the reading is valid.
    while (true) {
        frame_marker();
#if MPI_BEACON_LASTADDR
        uint32_t addr = g_last_addr;
#else
        uint32_t addr = g_kill_addr;
#endif
        for (unsigned b = 0; b < 16; b++) {
            frame_pulse(addr & (1u << b), b);
        }
    }
#endif

#if MPI_BEACON_LIVE
    // A lamp rather than a frame: what the *last* pass found, not what has ever
    // been found.  Seventeen pulses take a quarter of a minute to read, which is
    // fine for a verdict and useless for freeze spray, where the question is
    // whether the machine recovered in the two seconds since the chip got cold.
    //
    // See PP_BEACON_DONE in watch.h for the reading.
    {
        uint32_t last_pass = 0;
        unsigned stale_ms = 0, tick = 0;
        bool faulty = false;
        while (true) {
            uint32_t pass = g_pass;
            tick++;
            if (pass != last_pass) {
                last_pass = pass;
                stale_ms = 0;
                faulty = (g_beacons_last & PP_BEACON_FAIL_MASK) != 0;
                if (!faulty) {
                    // A clean pass still has to look like something, or "all
                    // well" and "board dead" are the same dark LED.
                    gpio_put(GPIO_STATUS_LED, STATUS_LED_ON);
                    sleep_ms(60);
                }
            } else if (stale_ms < 60000) {
                stale_ms += 25;
            }

            if (stale_ms >= 15000) {
                // Nothing has finished a pass in fifteen seconds. A pass takes
                // a few, so this is the PP hung or the bus gone -- worth its own
                // signal, because otherwise it reads as a very clean machine.
                // Phase off a free-running tick, not off stale_ms: that is
                // clamped so it cannot overflow, and a clamped counter makes the
                // flicker stop dead -- leaving a hung machine showing a steady
                // LED, which is one of the two readings it must not look like.
                gpio_put(GPIO_STATUS_LED,
                         (tick / 4) & 1 ? STATUS_LED_ON : STATUS_LED_OFF);
            } else {
                gpio_put(GPIO_STATUS_LED, faulty ? STATUS_LED_ON : STATUS_LED_OFF);
            }
            sleep_ms(25);
        }
    }
#endif

#if MPI_WATCH && !MPI_BEACON_LIVE && !MPI_BEACON_KILLADDR && !MPI_BEACON_LASTADDR
    // Blink the watchpoint results out, one frame per pass: a long dark gap to
    // mark the start, then one pulse per watchpoint in table order -- long for
    // hit, short for miss.  Every watchpoint gets a pulse whether or not it hit,
    // so positions cannot be miscounted, which is the failure mode of any
    // scheme that blinks only the hits.
    //
    // Nothing is ever cleared: these are "did this ever happen since power-on"
    // facts, and a frame that changes between passes is itself informative.
    while (true) {
        frame_marker();

        // A second of dark bus. Core 1 cannot notice this -- it is blocked in
        // pio_sm_get_blocking waiting for a strobe that is not coming -- so the
        // absence has to be spotted from out here.
        static uint32_t seen_served;
        if (g_served == seen_served) {
            g_bus_hits |= 1u << BUS_WENT_QUIET;
        }
        seen_served = g_served;
#if MPI_BEACONS
#if MPI_BEACON_PASS
        // Show the last *complete* pass while passes keep completing, and the
        // partial one only once they stop.
        //
        // Blinking g_beacons directly was a sampling bug wearing a diagnostic's
        // clothes: the frame is one instantaneous read, a pass is shorter than a
        // frame, so a healthy machine produced a different arbitrary subset of
        // its own phases every time. "Alive and phase one" and "nothing at all"
        // are both ordinary snapshots of a machine working perfectly, and both
        // were read as evidence of a wedge.
        //
        // A completed pass is a whole statement, so prefer it. Only when nothing
        // completes between frames is the partial pass the interesting one, and
        // then it says exactly how far the PP got before it stopped.
        static uint32_t last_pass_seen;
        uint32_t pass = g_pass;
        bool completing = pass != last_pass_seen;
        last_pass_seen = pass;
        uint32_t hits = completing ? g_beacons_last : g_beacons;

        // Two pulses the ROM cannot emit, because both mean it has stopped
        // running. Appended after the beacons so no beacon position moves.
        static uint16_t last_boot;
        uint16_t boot = g_boot;
        if (boot != last_boot) {
            hits |= 1u << PP_BEACON_COUNT;          // the PP restarted
            last_boot = boot;
        }
        if (g_saw_pp_ram) {
            hits |= 1u << (PP_BEACON_COUNT + 1);    // ...or left for PP RAM
            g_saw_pp_ram = false;                   // per frame, not cumulative
        }
        // The trap vector, bits 1..6 -- enough for every vector in the page,
        // and six pulses rather than sixteen because the top ten are always 0.
        uint32_t vec = g_trap_vec;      // latched since power-on; see above

        // Did the bus carry what we drove?
        //
        // The response machine samples the AD lines at the instant it asserts
        // the reply and compares them against the pattern it was given, so this
        // is the one question that separates our drive from everything past our
        // pads. It has been computed on every served cycle since the checksum
        // investigation and never once put in front of anyone in beacon mode --
        // which is where it belongs now that the PP is plainly reading things we
        // did not send.
        //
        // Lit: the lines are not carrying what we drive, so it is electrical and
        // on our side of the PP. Dark while the PP still misreads: it is being
        // handed correct data and getting it wrong, which is the PP.
        if (g_bus_hits & (1u << BUS_DATA_MISMATCH)) {
            hits |= 1u << (PP_BEACON_COUNT + 8);
            g_bus_hits &= ~(1u << BUS_DATA_MISMATCH);
            g_mismatch = 0;
        }
        for (unsigned b = 0; b < 6; b++) {
            if (vec & (2u << b)) {
                hits |= 1u << (PP_BEACON_COUNT + 2 + b);
            }
        }
#else
        uint32_t hits = g_beacons;
#endif
#else
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
        // Trailing pair: which block failed, most significant bit first.
        unsigned block = g_fail_block & 3;
        for (unsigned b = 0; b < 2; b++) {
            if (block & (1u << (1 - b))) {
                hits |= 1u << (count_of(g_watch) + BUS_EVENT_COUNT + 1 + b);
            }
        }
#endif
        for (unsigned i = 0; i < WATCH_PULSES; i++) {
            frame_pulse(hits & (1u << i), i);
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
