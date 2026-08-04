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
#define MPI_WATCH_PAIRS { \
    { 0160302, 0160300 }, \
    { 0160450, 0160446 }, \
    { 0160532, 0160530 }, \
    { 0172766, 0172764 }, \
    { 0174154, 0174152 }, \
    { 0101006, 0101004 }, \
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
    BUS_CS_DECLINED,        // a read in the CS-gated window went unanswered
    BUS_REPLY_UNTAKEN,      // a reply was prepared and the host never took it
    BUS_EVENT_COUNT
};

#endif // WATCH_H
