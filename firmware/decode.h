// Address decode and data preparation for a 1801RE2 on the MPI bus.
//
// Everything here is pure arithmetic over the 24-bit GPIO field: no SDK calls,
// no hardware. That is deliberate -- it is the part that is easy to get subtly
// wrong (bit scrambles, two different inversions, a chip code that is the
// complement of the address bits it decodes) and it is the part worth testing
// on a host. See test/test_decode.c.

#ifndef DECODE_H
#define DECODE_H

#include <stdbool.h>
#include <stdint.h>

#include "rom_images.h"

typedef struct {
    // Snapshot byte -> partial logical address.  Three tables, one per byte of
    // the 24-bit GPIO snapshot, each contributing the AD bits living in that
    // byte.  Pin inversion is baked in, so these yield the CPU-level address.
    uint16_t gather[3][256];

    // One table of ready-made drive patterns per 8 KB window, or NULL.
    //
    // Indexed by the top three bits of the *logical* address, which is not the
    // chip code.  The code is what the decoder sees on the inverted nAD13..15
    // lines, so it is the ones' complement: code 000 answers for 160000-177777,
    // whose top bits are 111.  mpi_window_index() is the only place that
    // conversion happens.
    const uint32_t *window[8];
    uint16_t        window_words[8];    // 0 == window not served
} mpi_decode_t;

// The chip code is the ones' complement of the top three logical address bits.
static inline unsigned mpi_window_index(uint8_t chip_code) {
    return (~chip_code) & 7;
}

// GPIO pattern that presents one ROM word on the bus: data inverted and
// scattered onto the AD pins, with the nRPLY bit low so that enabling its
// direction asserts the reply.
uint32_t mpi_drive_pattern(uint16_t word);

// Direction mask covering the AD lines, and the same plus nRPLY.
void mpi_direction_masks(uint32_t *ad, uint32_t *ad_rply);

// Populate gather tables and window tables.  `store` is caller-owned backing
// for the per-window pattern tables, one row per image.
void mpi_decode_init(mpi_decode_t *d, const mpi_image_t *images, unsigned count,
                     uint32_t (*store)[4096], unsigned store_rows);

// Logical 16-bit address from a 24-bit GPIO snapshot taken at the address
// strobe.
static inline uint32_t mpi_address(const mpi_decode_t *d, uint32_t snap) {
    return d->gather[0][snap & 0xFF]
         | d->gather[1][(snap >> 8) & 0xFF]
         | d->gather[2][(snap >> 16) & 0xFF];
}

// Should we answer this address, and with what?  False means stay off the bus:
// the address is outside every window we serve, or past the end of one (the
// code 0 chip stops short of the I/O page).
static inline bool mpi_lookup(const mpi_decode_t *d, uint32_t addr,
                              uint32_t *pattern) {
    unsigned w_idx = (addr >> 13) & 7;
    unsigned word  = (addr >> 1) & 0xFFF;   // AD0 selects the byte, not decoded
    if (word >= d->window_words[w_idx]) {
        return false;
    }
    *pattern = d->window[w_idx][word];
    return true;
}

#endif // DECODE_H
