#include <string.h>

#include "board.h"
#include "decode.h"

static const uint8_t g_ad_gpio[16] = AD_GPIO;

uint32_t mpi_drive_pattern(uint16_t word) {
    uint32_t pattern = 0;
    for (int bit = 0; bit < 16; bit++) {
        if (!((word >> bit) & 1)) {     // inverted on the wire
            pattern |= 1u << g_ad_gpio[bit];
        }
    }
    return pattern;                      // nRPLY bit stays 0 == asserted low
}

void mpi_direction_masks(uint32_t *ad, uint32_t *ad_rply) {
    uint32_t mask = 0;
    for (int i = 0; i < 16; i++) {
        mask |= 1u << g_ad_gpio[i];
    }
    *ad = mask;
    *ad_rply = mask | (1u << GPIO_nRPLY);
}

static void build_gather(mpi_decode_t *d) {
    memset(d->gather, 0, sizeof(d->gather));
    for (int bit = 0; bit < 16; bit++) {
        unsigned gpio = g_ad_gpio[bit];
        unsigned byte = gpio / 8, pos = gpio % 8;
        for (int v = 0; v < 256; v++) {
            // Pins are inverted: a low pin is a logical 1.
            if (!((v >> pos) & 1)) {
                d->gather[byte][v] |= (uint16_t)(1u << bit);
            }
        }
    }
}

void mpi_decode_init(mpi_decode_t *d, const mpi_image_t *images, unsigned count,
                     uint32_t (*store)[4096], unsigned store_rows) {
    build_gather(d);
    memset(d->window, 0, sizeof(d->window));
    memset(d->window_words, 0, sizeof(d->window_words));

    for (unsigned i = 0; i < count && i < store_rows; i++) {
        const mpi_image_t *img = &images[i];
        for (unsigned w = 0; w < img->word_count; w++) {
            store[i][w] = mpi_drive_pattern(img->words[w]);
        }
        unsigned w_idx = mpi_window_index(img->code);
        d->window[w_idx] = store[i];
        d->window_words[w_idx] = img->word_count;
    }
}
