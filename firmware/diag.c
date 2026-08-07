// Diagnostics: watchpoints, coverage, beacons, and the LED frames.
//
// Split out of main.c, which had grown to the point where the ROM emulator --
// the thing this board is for -- was a minority of its own source file. Nothing
// here changed in the move.
//
// The whole file is inside #if MPI_WATCH. A plain build compiles it to nothing
// and the entry points in diag.h become empty inlines.

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>

#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "hardware/pio.h"

#include "board.h"
#include "decode.h"
#include "diag.h"
#include "rom_images.h"
#include "status.h"
#include "watch.h"

// The CMake options resolve these, but a hand-rolled -D would not, and the
// failure would be a firmware that compiles and reports nothing -- which is the
// exact shape of the MPI_BEACONS-without-MPI_WATCH bug, already made once.
#if (MPI_BEACON_LIVE || MPI_BEACON_PASS) && !MPI_BEACONS
#error "MPI_BEACON_LIVE/MPI_BEACON_PASS need MPI_BEACONS; use the CMake options"
#endif
#if MPI_BEACON_LIVE && MPI_BEACON_PASS
#error "MPI_BEACON_LIVE and MPI_BEACON_PASS are two readings of one LED"
#endif

#if MPI_WATCH

// What colour the frame marker is, on a board that has colours.
//
// Eleven build variants report through one light, and on rev E there is no way
// to tell from the board which of them is flashed -- a frame of sixteen pulses
// is the address instrument or the plane-0-bits instrument depending on a
// decision made at the keyboard some time ago.  Reading the wrong table over
// the right frame is a mistake this project has actually made.
//
// The marker carries no data, so spending it on identity costs nothing and
// cannot corrupt a reading.  Exactly one display loop is compiled into any
// build, so one constant covers the whole file.
#if MPI_SOAK_P0BITS
#define FRAME_MARK_COLOUR   STATUS_MARK_BITS
#elif MPI_BEACON_KILLADDR || MPI_BEACON_LASTADDR
#define FRAME_MARK_COLOUR   STATUS_MARK_ADDR
#elif MPI_BEACONS
#define FRAME_MARK_COLOUR   STATUS_MARK_BEACON
#else
#define FRAME_MARK_COLOUR   STATUS_MARK_WATCH
#endif

// Borrowed from the emulator at init: the decode tables, for the coverage
// counts, and the AD-line mask, so a readback is compared only against pins we
// drive.
static const mpi_decode_t *g_diag_dec;
static uint32_t g_diag_ad_mask;

// The pattern we most recently drove, kept for the next cycle's readback.
static uint32_t g_last_pattern;

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
#if MPI_SOAK_P0BITS
// Plane 0's failing bits, captured on core 1 the instant a pass completes.
//
// Core 0 cannot do this. It samples the pass counter once per frame, and a frame
// is about eight seconds while a soak pass is a few -- so it sees perhaps one
// pass in three. The window this is trying to catch is one pass wide: plane 0
// fails alone, and by the next pass the whole memory has gone and the evidence
// with it. An instrument that samples slower than the event it is looking for
// will simply never see it.
//
// On core 1 every completed pass is examined, so a window one pass wide is
// caught the first time it happens.
static volatile uint32_t g_p0_bits;
static volatile bool g_p0_have;
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
    unsigned want = g_diag_dec->window_words[w + MPI_COVERAGE_FIRST];
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
#if MPI_SOAK_P0BITS
            // Plane 0 failed and the CPU planes did not: the one informative
            // pass, and it may be the only one.
            if (!g_p0_have && (g_beacons & (1u << 2))
                && !(g_beacons & ((1u << 4) | (1u << 5)))) {
                g_p0_bits = (g_beacons >> 8) & 0xFF;
                g_p0_have = true;
            }
#endif
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
    // On rev F the marker is also where the frame says which instrument it
    // came from -- see FRAME_MARK_COLOUR.  The pulse is identical either way;
    // colour never carries data, only identity.
    status_style(FRAME_MARK_COLOUR);
    status_set(true);
    sleep_ms(2500);
    status_set(false);
    status_style(STATUS_DATA);
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
    status_set(true);
    // 7:1 -- long enough to be unmistakable without making a frame interminable.
    sleep_ms(lit ? 700 : 100);
    status_set(false);
    sleep_ms(((index + 1) % 5 == 0) ? 900 : 300);
}

// ---------------------------------------------------------------------------
// What the emulator calls
// ---------------------------------------------------------------------------

void diag_init(const mpi_decode_t *dec, uint32_t ad_mask) {
    g_diag_dec = dec;
    g_diag_ad_mask = ad_mask;
}

void __not_in_flash_func(diag_note_readback)(uint32_t saw) {
    uint32_t bad = (saw ^ g_last_pattern) & g_diag_ad_mask;
    if (bad) {
        g_bus_hits |= 1u << BUS_DATA_MISMATCH;
        g_mismatch |= bad;
    }
}

void __not_in_flash_func(diag_note_untaken)(void) {
    g_bus_hits |= 1u << BUS_REPLY_UNTAKEN;
}

void __not_in_flash_func(diag_note_served)(uint32_t addr, uint32_t pattern) {
    coverage_note(addr);        // after the put: never on the reply path
    g_last_pattern = pattern;
}

void __not_in_flash_func(diag_note_cycle)(uint32_t addr) {
    watch_note(addr);
}

void diag_display(void) {
#if MPI_SOAK_P0BITS
    // Eight pulses: which bits of plane 0 failed, from the first pass in which
    // plane 0 failed *alone*.
    //
    // Latched, and the latching is the point. Plane 0 goes first and by itself
    // for a pass or two, then the CPU planes follow and everything reads bad --
    // so the informative pass is over in seconds and any frame that keeps
    // updating shows the collapse instead of the cause. The first plane-0-only
    // pass is the evidence; nothing after it is.
    //
    // On a bank of 1-bit-wide DRAM each bit is one chip, so this is a list of
    // parts. Dark frame means it has not happened yet.
    while (true) {
        bool have = g_p0_have;
        uint32_t latched = g_p0_bits;
        frame_marker();
        for (unsigned i = 0; i < 8; i++) {
            frame_pulse(have && (latched & (1u << i)), i);
        }
    }
#endif

#if MPI_SOAK_DIGIT
    // One digit, counted on the fingers of one hand.
    //
    // The soak's frame is twenty-seven pulses, and a reader at a bench cannot
    // hold that. Reported from the bench, more than once, in exactly those
    // words -- and a frame nobody can read is not a measurement however much
    // information it theoretically contains.
    //
    // The open question is one thing: did plane 0 fail, and did it fail alone?
    // That is four states. So blink the answer as one, two, three or four
    // flashes with a long pause between repeats, and let the twenty-seven-pulse
    // frame exist for when someone actually wants the bit positions.
    //
    //   1  clean -- PP RAM and both CPU planes passed
    //   2  plane 0 (PP RAM) failed, CPU planes clean
    //   3  CPU planes failed, plane 0 clean
    //   4  both failed
    //   fast flicker  no pass has completed in fifteen seconds
    //
    // Hardcoded to the soak's beacon map, and only correct for that ROM.
    {
        uint32_t last_pass = 0;
        unsigned stale_ms = 0, tick = 0;
        unsigned code = 1;
        while (true) {
            uint32_t pass = g_pass;
            tick++;
            if (pass != last_pass) {
                last_pass = pass;
                stale_ms = 0;
                uint32_t b = g_beacons_last;
                bool p0 = b & (1u << 2);                    // PP RAM failed
                bool p12 = b & ((1u << 4) | (1u << 5));     // plane 1 or 2
                code = 1 + (p0 ? 1 : 0) + (p12 ? 2 : 0);
            } else if (stale_ms < 60000) {
                stale_ms += 25;
            }

            if (stale_ms >= 15000) {
                status_style(STATUS_STUCK);
                status_set((tick / 4) & 1);
                sleep_ms(25);
                continue;
            }
            // The digit is still the answer; on rev F its colour repeats it,
            // so a miscounted flash cannot turn "clean" into "both failed".
            status_style(code == 1 ? STATUS_GOOD : STATUS_BAD);
            for (unsigned i = 0; i < code; i++) {
                status_set(true);
                sleep_ms(250);
                status_set(false);
                sleep_ms(350);
            }
            sleep_ms(2500);         // long enough that the count cannot run on
        }
    }
#endif

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
                    status_style(STATUS_GOOD);
                    status_set(true);
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
                status_style(STATUS_STUCK);
                status_set((tick / 4) & 1);
            } else {
                // Rev E: steady means the last pass failed, dark means it was
                // clean -- a convention that reads backwards to everyone who
                // meets it, and has to, because darkness is all a plain LED has
                // left.  Rev F just says red or green and needs no convention.
#if BOARD_HAS_NEOPIXEL
                status_style(faulty ? STATUS_BAD : STATUS_GOOD);
                status_set(true);
#else
                status_set(faulty);
#endif
            }
            sleep_ms(25);
        }
    }
#endif

#if MPI_WATCH && !MPI_BEACON_LIVE && !MPI_BEACON_KILLADDR && !MPI_BEACON_LASTADDR && !MPI_SOAK_DIGIT \
    && !MPI_SOAK_P0BITS
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

}

#endif  // MPI_WATCH
