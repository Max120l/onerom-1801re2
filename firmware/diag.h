// The diagnostics, kept out of the emulator.
//
// Everything in this header and in diag.c exists to answer questions *about* a
// machine, and none of it is needed to be a ROM. A board left in a working
// computer should be a ROM and nothing else, so the split is not cosmetic: with
// MPI_WATCH off every entry point below is an empty inline and the emulator
// compiles as though the diagnostics had never been written.
//
// What the emulator has to do is call these at the right moments. The contract
// is that all of them are cheap and none is on the reply path -- the notes are
// taken after the reply has been queued, which is what lets a diagnostic build
// serve the bus identically to a plain one.

#ifndef DIAG_H
#define DIAG_H

#include <stdbool.h>
#include <stdint.h>

#include "decode.h"

// Owned by the emulator, read by the diagnostics: served and abandoned cycle
// counts.  See main.c.
extern volatile uint32_t g_served;
extern volatile uint32_t g_missed;

#if MPI_WATCH

// dec is borrowed for the life of the program; ad_mask is the AD-line field, so
// a readback can be compared only against the pins we actually drive.
void diag_init(const mpi_decode_t *dec, uint32_t ad_mask);

// The previous cycle's readback, drained before this cycle is prepared.
void diag_note_readback(uint32_t saw);

// A reply was assembled and the host never took it.
void diag_note_untaken(void);

// We answered this address with this pattern.
void diag_note_served(uint32_t addr, uint32_t pattern);

// Every address strobe on the bus, answered or not.
void diag_note_cycle(uint32_t addr);

// Blink whatever this build was configured to report.  Never returns if a
// display mode is selected; returns immediately if none is, so the caller falls
// through to the ordinary status LED.
void diag_display(void);

#else

static inline void diag_init(const mpi_decode_t *dec, uint32_t ad_mask) {
    (void)dec; (void)ad_mask;
}
static inline void diag_note_readback(uint32_t saw) { (void)saw; }
static inline void diag_note_untaken(void) { }
static inline void diag_note_served(uint32_t addr, uint32_t pattern) {
    (void)addr; (void)pattern;
}
static inline void diag_note_cycle(uint32_t addr) { (void)addr; }
static inline void diag_display(void) { }

#endif  // MPI_WATCH
#endif  // DIAG_H
