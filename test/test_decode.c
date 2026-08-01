// Host test for the address decode and data preparation path.
//
// This walks the same arithmetic the firmware runs per bus cycle, using the
// real pin map out of board_fire24e.h, and checks it against an independently
// written model of the bus. It is the test that would have caught the chip
// code / address bit complement bug.
//
//   make -C test && test/build/test_decode
//
// Images here are synthetic, so the test is self-contained and carries no ROM
// dumps. What matters is the mapping, not the contents.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "board_fire24e.h"
#include "decode.h"

static const uint8_t ad_gpio[16] = AD_GPIO;

static int failures;

#define CHECK(cond, ...)                                                      \
    do {                                                                      \
        if (!(cond)) {                                                        \
            printf("  FAIL %s:%d: ", __FILE__, __LINE__);                     \
            printf(__VA_ARGS__);                                              \
            printf("\n");                                                     \
            if (++failures > 20) { printf("  (too many, stopping)\n");        \
                                   exit(1); }                                 \
        }                                                                     \
    } while (0)

// --- an independent model of the bus, written from the datasheet ------------

// What the 24-bit GPIO field looks like while the host presents `addr`.
// Everything on this bus is inverted, so a logical 1 pulls its pin low.
static uint32_t bus_present_address(uint16_t addr) {
    uint32_t pins = 0x00FFFFFF;                 // idle high
    for (int bit = 0; bit < 16; bit++) {
        if ((addr >> bit) & 1) {
            pins &= ~(1u << ad_gpio[bit]);      // logical 1 -> pin low
        }
    }
    return pins;
}

// Recover the 16-bit word a drive pattern would put on the bus.
static uint16_t bus_read_data(uint32_t pattern) {
    uint16_t word = 0;
    for (int bit = 0; bit < 16; bit++) {
        if (!((pattern >> ad_gpio[bit]) & 1)) { // pin low -> logical 1
            word |= (uint16_t)(1u << bit);
        }
    }
    return word;
}

// --- fixtures ---------------------------------------------------------------

#define IO_PAGE_WORDS ((0177000 - 0160000) / 2)     // 3840

static uint16_t img_words[4][4096];
static mpi_image_t images[4];
static uint32_t store[MPI_MAX_WINDOWS][4096];

// The four UKNC chips: codes 011, 010, 001, 000 for 205, 206, 207, 208.
static const uint8_t codes[4] = { 03, 02, 01, 00 };

static void build_fixtures(void) {
    for (int i = 0; i < 4; i++) {
        for (int w = 0; w < 4096; w++) {
            // Distinct per chip and per word, and exercises every bit.
            img_words[i][w] = (uint16_t)(w * 7 + i * 0x1111 + (w << 4));
        }
        images[i].name = "synthetic";
        images[i].code = codes[i];
        images[i].words = img_words[i];
        images[i].word_count = (codes[i] == 0) ? IO_PAGE_WORDS : 4096;
    }
}

// --- tests ------------------------------------------------------------------

static void test_pin_map(void) {
    printf("pin map\n");
    uint32_t seen = 0;
    for (int bit = 0; bit < 16; bit++) {
        unsigned g = ad_gpio[bit];
        CHECK(g < 24, "nAD%d on GPIO %u, outside the 24-bit field", bit, g);
        CHECK(g != GPIO_X1 && g != GPIO_X2,
              "nAD%d on GPIO %u, an X jumper pad", bit, g);
        CHECK((seen & (1u << g)) == 0, "GPIO %u assigned twice", g);
        seen |= 1u << g;
    }
    const unsigned ctrl[] = { GPIO_nSYNC, GPIO_nDIN, GPIO_nRPLY, GPIO_nSEL };
    for (unsigned i = 0; i < 4; i++) {
        CHECK(ctrl[i] < 24, "control pin on GPIO %u, outside the field", ctrl[i]);
        CHECK((seen & (1u << ctrl[i])) == 0,
              "control pin GPIO %u collides with an AD line", ctrl[i]);
        seen |= 1u << ctrl[i];
    }

    uint32_t ad, ad_rply;
    mpi_direction_masks(&ad, &ad_rply);
    CHECK(__builtin_popcount(ad) == 16, "AD direction mask covers %d pins",
          __builtin_popcount(ad));
    CHECK(ad_rply == (ad | (1u << GPIO_nRPLY)), "nRPLY missing from mask");
    CHECK((ad_rply & ((1u << GPIO_X1) | (1u << GPIO_X2))) == 0,
          "a direction mask would drive an X jumper pad");
}

// The chip code is the ones' complement of the top three address bits.
static void test_window_index(void) {
    printf("window index\n");
    static const unsigned base[8] = {           // per the k1801 code table
        0160000, 0140000, 0120000, 0100000,
        0060000, 0040000, 0020000, 0000000,
    };
    for (unsigned code = 0; code < 8; code++) {
        CHECK(mpi_window_index((uint8_t)code) == ((base[code] >> 13) & 7),
              "code %o maps to index %u, expected %u",
              code, mpi_window_index((uint8_t)code), (base[code] >> 13) & 7);
    }
}

// Present every address in every served window and check the byte that comes
// back is the one in the image.
static void test_round_trip(void) {
    printf("round trip over all four windows\n");
    mpi_decode_t d;
    mpi_decode_init(&d, images, 4, store, MPI_MAX_WINDOWS);

    unsigned served = 0;
    for (int i = 0; i < 4; i++) {
        unsigned top = mpi_window_index(codes[i]);
        for (unsigned w = 0; w < 4096; w++) {
            uint16_t addr = (uint16_t)((top << 13) | (w << 1));

            uint32_t decoded = mpi_address(&d, bus_present_address(addr));
            CHECK(decoded == addr, "address %06o decoded as %06o",
                  addr, decoded);

            uint32_t pattern;
            bool answer = mpi_lookup(&d, decoded, &pattern);
            bool expect = w < images[i].word_count;
            CHECK(answer == expect, "%06o: answered %d, expected %d",
                  addr, answer, expect);
            if (!answer) {
                continue;
            }
            CHECK(((pattern >> GPIO_nRPLY) & 1) == 0,
                  "%06o: nRPLY bit high, reply would never assert", addr);
            uint16_t got = bus_read_data(pattern);
            CHECK(got == img_words[i][w], "%06o: read 0x%04X, expected 0x%04X",
                  addr, got, img_words[i][w]);
            served++;
        }
    }
    printf("  %u addresses served correctly\n", served);
    CHECK(served == 3 * 4096 + IO_PAGE_WORDS, "served %u words", served);
}

// Anything outside the configured windows must leave the bus alone.
static void test_silence_elsewhere(void) {
    printf("silence outside the served windows\n");
    mpi_decode_t d;
    mpi_decode_init(&d, images, 4, store, MPI_MAX_WINDOWS);

    unsigned quiet = 0;
    for (unsigned top = 0; top < 4; top++) {        // 000000-077777, all unserved
        for (unsigned w = 0; w < 4096; w++) {
            uint16_t addr = (uint16_t)((top << 13) | (w << 1));
            uint32_t pattern;
            CHECK(!mpi_lookup(&d, mpi_address(&d, bus_present_address(addr)),
                              &pattern),
                  "%06o: answered for a window we do not serve", addr);
            quiet++;
        }
    }
    // And the I/O page, which the code 0 chip must stop short of.
    for (unsigned w = IO_PAGE_WORDS; w < 4096; w++) {
        uint16_t addr = (uint16_t)((7u << 13) | (w << 1));
        uint32_t pattern;
        CHECK(!mpi_lookup(&d, mpi_address(&d, bus_present_address(addr)),
                          &pattern),
              "%06o: answered inside the I/O page", addr);
        quiet++;
    }
    printf("  %u addresses correctly left alone\n", quiet);
}

// AD0 picks the byte within a word and must not affect which word is served.
static void test_byte_select_ignored(void) {
    printf("AD0 does not change the word selected\n");
    mpi_decode_t d;
    mpi_decode_init(&d, images, 4, store, MPI_MAX_WINDOWS);

    for (unsigned w = 0; w < 4096; w += 97) {
        uint16_t even = (uint16_t)((4u << 13) | (w << 1));   // 100000 window
        uint32_t a, b;
        bool ra = mpi_lookup(&d, mpi_address(&d, bus_present_address(even)), &a);
        bool rb = mpi_lookup(&d, mpi_address(&d, bus_present_address(even | 1)), &b);
        CHECK(ra && rb && a == b, "%06o and its odd byte differ", even);
    }
}

// CS in a socket speaks only for that socket's own window.
static void test_cs_scope(void) {
    printf("CS gates only its own window\n");
#if SOCKET_CS_CODE == 0xFF
    for (unsigned i = 0; i < 8; i++) {
        CHECK(!mpi_cs_gates(i), "window %u gated, but this socket grounds CS", i);
    }
#else
    unsigned owned = mpi_window_index(SOCKET_CS_CODE);
    unsigned gated = 0;
    for (unsigned i = 0; i < 8; i++) {
        if (mpi_cs_gates(i)) {
            gated++;
            CHECK(i == owned, "window %u gated by a CS that speaks for %u",
                  i, owned);
        }
    }
    CHECK(gated == 1, "%u windows gated by CS, expected exactly 1", gated);
    printf("  socket CS is code %o, gating window index %u only\n",
           SOCKET_CS_CODE, owned);
#endif
}

int main(void) {
    build_fixtures();
    test_pin_map();
    test_window_index();
    test_round_trip();
    test_silence_elsewhere();
    test_byte_select_ignored();
    test_cs_scope();

    if (failures) {
        printf("\n%d failure(s)\n", failures);
        return 1;
    }
    printf("\nall checks passed\n");
    return 0;
}
