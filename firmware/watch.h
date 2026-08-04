// Address watchpoints: ask the machine what it actually executed.
//
// The board is the ROM, so every instruction the peripheral processor fetches
// is a read it hands us. That makes a question like "did this machine ever
// execute the code that prints the boot menu?" answerable without a logic
// analyser, without a serial port, and without modifying the ROM contents at
// all -- we are already being told, we just have to keep score.
//
// Nothing here is on the reply path. Watchpoints are checked after the response
// has been queued, so a diagnostic build serves the bus identically to a normal
// one and cannot introduce the timing fault it is being used to rule out.
//
// Build with -DMPI_WATCH=ON; the status LED then blinks a frame per pass, one
// pulse per watchpoint, long for hit and short for miss. See README.
//
// ---------------------------------------------------------------------------
// Why a watchpoint is a *pair* of addresses
// ---------------------------------------------------------------------------
//
// The first version of this scored a single address and reported every
// watchpoint as hit on a machine that plainly had not executed most of them.
// The bus does not distinguish an instruction fetch from a data read, and the
// monitor's startup test checksums all four ROMs -- it reads 16127 of the
// 16128 words, every watchpoint among them, within the first moments of power
// on. "This address was read" is therefore true of the entire ROM and says
// nothing at all.
//
// What separates a fetch from a data read is the company it keeps. Executing
// straight-line code reads consecutive words: A, then A+2, back to back. The
// checksum walks *downwards* and interleaves three instruction fetches of its
// own loop body between every pair of data reads, so a data read of A is
// followed by 160434, never by A+2. The same is true of the loop at 173252
// that streams ROM into the video planes: it ascends, but its own loop fetches
// sit between consecutive data reads.
//
// So each watchpoint is two addresses: the address to score, and the address
// that must have been captured *immediately* before it. Pick the second word of
// a multi-word instruction and the predecessor is its first word, which holds
// however the instruction was reached -- fall-through or branch. No sequence
// any of the ROM-reading loops produces can forge that adjacency.
//
// This can only produce false negatives, never false positives: another master
// interleaving a cycle, or a write landing between the two fetches, breaks the
// pair and loses a hit. A short pulse means "not seen", not "did not happen".

#ifndef WATCH_H
#define WATCH_H

#include <stdbool.h>
#include <stdint.h>

// { address to score, address that must immediately precede it }
//
// Addresses are in the logical PP address space, i.e. what the disassembly
// shows (tools/pdp11dis.py). Each pair is checked against the ROM image by
// test/test_watchpoints.py, which is what stops a misread disassembly from
// quietly becoming a wrong answer.
//
//   160302 after 160300  the monitor's entry, "mov @#172660, r4". Must hit: if
//                        this is short the PP is not starting from our vector
//                        and nothing below it means anything.
//   160450 after 160446  "inc r0" in the ROM checksum loop, reached only by the
//                        beq at 160446 falling through -- a block's sum did not
//                        match the value programmed into the last chip. A hit
//                        is the machine saying one of our images is wrong.
//   160532 after 160530  "bis #2, r0": the PP RAM test found a fault.
//   172766 after 172764  the jsr into the routine that prints "- ОШИБКА ..."
//                        lines. Worth knowing separately from whether the
//                        message is legible on screen.
//   174154 after 174152  the PP task dispatcher: the startup test completed.
//   101006 after 101004  "mov #4, r0", the instruction the EMT at 101000
//                        returns to -- and 101000 is the emt whose inline
//                        argument points at the ЗАГРУЗКА string. A hit means
//                        the boot menu header was printed.
// Trimmed to the three that are still open. The PP RAM test (160532 after
// 160530), the error printer (172766 after 172764) and the end of the startup
// test (174154 after 174152) have all answered consistently -- RAM clean, no
// error line, startup completes -- and every pulse spent re-confirming them is
// a pulse the reader has to count past.
// 160342 and 160374 are the central processor's entire reset. It has none of
// its own: CMotherboard::Reset touches only the PP's DCLO and ACLO pins, and
// the CPU's are driven exclusively by the PP writing port 177716. So the CPU
// starts if and only if the PP executes, out of our ROM:
//
//   160332  mov #40, @#177716      hold it: DCLO asserted
//   160340  jsr pc, 173252         load its memory through the plane ports
//   160360  clr @#177716           release DCLO
//   160364  mov #100, r0 / sob     settle
//   160372  mov #100000, @#177716  release ACLO -- the edge that starts it
//
// A 1801 starts on the falling edge of ACLO with DCLO already low, so that last
// write *is* the CPU's power-on. Watching the jsr and the ACLO release
// separates "the CPU was never started" from "the CPU was started and failed",
// which is otherwise guesswork -- and both are instructions read from us.
//
// 174170 is the PP's idle loop, and it exists for the frozen-menu case. The
// monitor's dispatcher scans a task queue:
//
//   174164  mov #7060, r0
//   174170  tst (r0)+
//   174172  beq 174170     <- back round while the entry is empty
//
// The cursor blink and the keyboard scan are tasks. A menu drawn on screen with
// no blinking cursor and no response to keys means the PP is not dispatching,
// and this pulse says which kind of not-dispatching it is: lit means the PP is
// alive and scanning but nothing ever becomes ready, which points at interrupts
// or the timer; dark means it never reached the dispatcher or has left it, which
// points at a wild jump or a trap.
//
// Note the pair runs backwards -- the branch at 174172 followed by its own
// target at 174170. Taking a branch produces no memory cycle, so the two fetches
// are adjacent; the forward direction is not, because "tst (r0)+" reads memory
// between them.
#define MPI_WATCH_PAIRS { \
    { 0160302, 0160300 }, \
    { 0160450, 0160446 }, \
    { 0160342, 0160340 }, \
    { 0160374, 0160372 }, \
    { 0101006, 0101004 }, \
    { 0174170, 0174172 }, \
}
#define MPI_WATCH_MAX   8

typedef struct {
    uint32_t addr;      // scored when seen
    uint32_t prev;      // ...and only if this was the cycle before it
} mpi_watch_t;

// ---------------------------------------------------------------------------
// Bus events
// ---------------------------------------------------------------------------
//
// The watchpoints above say what the machine executed. These say what the board
// did, and they exist because the two can disagree: the monitor reported a ROM
// block failing its checksum on hardware while the same images pass that same
// checksum offline. Something is between our correct data and the processor's
// wrong sum, and there are only two candidates -- reads we declined to answer,
// and replies we assembled that were never taken.
//
// They are appended to the LED frame after the code watchpoints, so the pulse
// positions of the watchpoints do not move when these are added or removed.
enum {
    BUS_REPLY_UNTAKEN,      // a reply was prepared and the host never took it

    // Did the bus carry what we drove?
    //
    // Coverage came back complete on all four windows, no reply went untaken,
    // and the machine still reported a bad block. The one thing left that no
    // counter of cycles can see is the data itself: we drive sixteen lines and
    // have never once checked what is actually on them.
    //
    // So look. The response machine samples the AD lines at the moment the host
    // releases the read strobe -- the closest instant to when the host latched
    // them, with our drivers still on -- and the CPU compares that against the
    // pattern it asked for. A mismatch means something between our pads and the
    // processor is not carrying what we sent, which is the last board-side
    // explanation standing.
    BUS_DATA_MISMATCH,

    // Were we awake before the machine started asking?
    //
    // The board takes its power from the socket, so it and the machine come out
    // of reset together -- except that we have an RP2350 bootrom to run, a
    // clock to set and two state machines to load first, while the PP starts
    // fetching as soon as its own reset releases. Lose that race and the PP's
    // first reads meet a ROM that is not answering yet.
    //
    // The PP's first read after reset is its power-up vector at 160000. So if
    // the very first cycle we ever capture is 160000 we were in time, and if it
    // is anything else we came up mid-stream and the machine has already been
    // asking questions nobody answered.
    BUS_FIRST_IS_VECTOR,

    // Has the machine stopped asking?
    //
    // The other half of the frozen-menu question. A PP spinning in its idle
    // dispatcher still fetches, so the bus stays busy; a PP that has halted, or
    // is waiting forever on a reply nobody will give, produces nothing at all.
    // Set when a whole second passes with no cycle served, which no running
    // machine does.
    BUS_WENT_QUIET,

    BUS_EVENT_COUNT
};

#define PP_POWERUP_VECTOR  0160000

// A frame describes one boot, not a power session.
//
// Hits were originally never cleared, on the reasoning that "did this ever
// happen" is the useful question. It is not, once the machine is being reset
// repeatedly: the frame becomes the union of every boot since the board was
// powered, and a checksum failure from the third attempt sits in the same frame
// as a screen drawn on the fifth. That ambiguity showed up the moment a boot
// printed a CPU error with no ROM error while the checksum pulse was still lit
// from an earlier try.
//
// So the frame clears when the machine restarts. The PP takes PC then PSW from
// its power-up vector, so a fetch of 160002 directly behind 160000 means a
// restart -- and, being a pair, it cannot be forged by the checksum reading
// those same two words as data on its way down.
#define PP_RESTART_ADDR    (PP_POWERUP_VECTOR + 2)

// ---------------------------------------------------------------------------
// Window coverage
// ---------------------------------------------------------------------------
//
// The instrument has a blind spot, and it is exactly the size of the remaining
// question. "We declined a read" and "a reply went untaken" both came back
// negative, so we answer every read we are asked and the host takes every
// answer -- and the machine still computes a bad checksum. That leaves two
// possibilities, and one of them is invisible to any counter of cycles we saw:
//
//   the data is corrupted electrically between our pins and the processor, or
//   some reads never reached us at all.
//
// The second is not far-fetched here. The read strobe on pin 1 is EDIN, which
// the CGM withholds when a window is banked to RAM or a cartridge, so a read
// directed elsewhere produces no strobe and no cycle for us to count. We would
// see nothing and report nothing, while the processor happily summed whatever
// did answer.
//
// Coverage closes it. The monitor's startup checksum reads every word of every
// window exactly once, so after a completed startup each window we serve should
// have had all of its words asked for. One bit per word, set as we serve it:
//
//   all four windows fully covered  ->  every word came from us, so the
//                                       corruption is electrical
//   a window short of its count     ->  reads for it went somewhere else, and
//                                       that window is the failing block
//
// 4 KB of bitmap and three instructions off the reply path to answer a question
// no amount of staring at the ROM contents can.
// Reported as a single pulse: all four windows came back complete on hardware,
// so the interesting reading is now "still complete" rather than which one.
#define MPI_COVERAGE_WINDOWS  4         // the four the UKNC's ROMs occupy
#define MPI_COVERAGE_FIRST    4         // window index of 100000

// ---------------------------------------------------------------------------
// Which block failed its checksum
// ---------------------------------------------------------------------------
//
// The checksum failure is intermittent -- boots that print a CPU or CPU-RAM
// error with no "- ОШИБКА ПЗУ" beside them are boots where all four blocks
// verified -- and every theory about why has now been wrong twice. So stop
// theorising and ask which block.
//
// The loop body is the same code for all four, so the fetch addresses cannot
// tell them apart. The data does: the comparison is
//
//     160442  026503 176766   cmp 176766(r5), r3
//
// with r5 taking 8, 6, 4, 2 across the four blocks, so it reads its stored sum
// from 176776, 176774, 176772 and 176770 in turn. That read lands immediately
// behind the fetch of the instruction's second word at 160444 -- a pair, so the
// summing loop cannot forge it while reading those same words as data on its
// way down. Remember the last one seen, and a failure names its block.
//
//   176770 -> the 205, 100000-117777      176774 -> the 207, 140000-157777
//   176772 -> the 206, 120000-137777      176776 -> the 208, 160000-176777
// ---------------------------------------------------------------------------
// Beacons, for when the ROM we serve is our own test program
// ---------------------------------------------------------------------------
//
// A test ROM running on the PP has no console at reset, so it reports by
// *reading* reserved addresses and letting us watch them go past. The beacons
// sit at 077700 in PP RAM -- below our windows, so we never answer those reads,
// but the capture machine latches every address strobe on the bus whether or
// not the cycle is ours, so we see them anyway. Being the ROM and the
// instrument at the same time is the one thing a mask ROM could never do.
//
// The beacons sit in ROM rather than RAM so that seeing them needs no
// assumption at all: we answer those reads ourselves. In PP RAM it would depend
// on the capture machine latching cycles for addresses we do not serve, which
// is probably true and has never been demonstrated -- every address this
// project has confirmed seeing has been one of ours.
//
// -DMPI_BEACONS=ON replaces the monitor watchpoints in the LED frame with one
// pulse per beacon. Keep in step with tools/make_ramtest.py.
#define PP_BEACON_BASE   0176700

// How many beacons the frame blinks. The soak in tools/make_ramtest.py uses 17;
// the address-bus test in tools/make_addrtest.py has a different map and a
// different count, so this is overridable from CMake (-DMPI_BEACON_COUNT=19)
// rather than being a number two ROMs have to agree on by luck.
//
// It only controls how many pulses come out. Getting it too small truncates the
// frame; too large adds dark pulses at the end. Neither corrupts anything, but
// both make the frame lie about which pulse is which, so set it to match the ROM
// being flashed.
#ifndef PP_BEACON_COUNT
#define PP_BEACON_COUNT  17
#endif

// ---------------------------------------------------------------------------
// Live mode, for freeze spray
// ---------------------------------------------------------------------------
//
// The frame above latches: g_beacons is set-only, and the only thing that ever
// clears it is a restart. That is right for "did this machine ever fail", and
// exactly wrong for the workflow of cooling one chip at a time and watching for
// the machine to come back -- a fault that has been lit since pass 400 stays lit
// however cold the guilty part gets, so the instrument says nothing about what
// the freeze spray just did.
//
// -DMPI_BEACON_LIVE=ON turns the LED into a lamp for the *last completed pass*
// instead. The test ROM's DONE beacon marks the end of a pass, so that is the
// natural boundary: on DONE, core 1 snapshots the beacons and clears them.
//
//   steady on            the last pass failed
//   dark, blipping       the last pass was clean (the blip is one pass ending)
//   fast flicker         no pass has finished in fifteen seconds -- the PP is
//                        stuck, which is itself a symptom
//
// This is the soak's beacon map, not a universal one: the address-bus test puts
// DONE at beacon 1. Live mode belongs to the soak; the address test wants the
// ordinary latching frame, which is a verdict rather than a lamp.
//
// Pair it with an image from `make_ramtest.py --live`, which clears the ROM's
// own accumulators at the top of each pass. Either half alone still latches:
// the firmware would clear a mask the ROM keeps re-asserting, or the ROM would
// forget a fault the firmware keeps displaying.
#define PP_BEACON_DONE   6

// Which beacons mean "this pass was not clean". Everything that is not one of
// the four positive reports: PP RAM bad, either plane bad, stuck, all-bits-at-
// once, and the eight per-bit pulses.
#define PP_BEACON_OK_MASK   ((1u << 0) | (1u << 1) | (1u << 3) | (1u << 6))
#define PP_BEACON_FAIL_MASK (((1u << PP_BEACON_COUNT) - 1u) & ~PP_BEACON_OK_MASK)

#define CHK_CMP_EXT     0160444     // second word of the compare
#define CHK_SUM_LOW     0176770     // lowest of the four stored sums
#define CHK_FAIL_ADDR   0160450     // "inc r0": this block did not match
#define CHK_FAIL_PREV   0160446     // ...reached by the beq falling through

#endif // WATCH_H
