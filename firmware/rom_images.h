#ifndef ROM_IMAGES_H
#define ROM_IMAGES_H

#include <stdint.h>

// Each 1801RE2 decodes a fixed 8 KB window of the 64 KB address space, chosen
// by its mask-programmed "code".  One board can answer for several windows at
// once -- there is no chip select to arbitrate, so this works as long as the
// real chips for those windows are out of their sockets.
#define MPI_MAX_WINDOWS 8

typedef struct {
    const char     *name;
    uint8_t         code;        // 0..7, selects the 8 KB window
    uint16_t        word_count;  // words actually served from the window start;
                                 // 4096 for a full window.  On the UKNC the
                                 // code 0 chip must stop at 3840 so it does not
                                 // fight the I/O page at 177000-177777.
    const uint16_t *words;       // CPU order -- run dumps through
                                 // tools/re2_convert.py first
} mpi_image_t;

extern const mpi_image_t mpi_images[];
extern const unsigned    mpi_image_count;

#endif // ROM_IMAGES_H
