#!/usr/bin/env python3
"""Build a test ROM that exercises one register with patterns, and nothing else.

Every test in this project so far has gone through memory to ask its question,
which means every answer has been entangled with the DRAM, its address
multiplexer and its strobes. All three have now measured clean, and the fault
has narrowed to something that does not need them: port 177010 reads back a
different value from the one written.

So take the memory out. This writes patterns to the plane address register and
reads them straight back. No cell is ever addressed, nothing is stored, and a
whole pass is a few dozen instructions -- so passes complete thousands of times
a second and the LED reflects the machine's state now rather than ten seconds
ago.

The patterns are chosen to separate two different faults:

    000000, 177777      every bit the same. Blind to any interaction *between*
                        lines -- can only find a bit stuck high or stuck low.
    125252, 052525      every bit the opposite of both its neighbours. Maximal
                        stress on adjacent-line coupling, and the only patterns
                        here that can see it.

That distinction is the whole point, because the machine's fingerprint is that
address bits 1, 3, 5, 7, 9, 11, 13 come back wrong while 0, 2, 4, 6, 8, 10, 12,
14 are clean, over and over, unchanged across three board reworks. Every odd bit
and no even bit is what you get when each odd bit takes its even neighbour's
value -- and a model that says "adjacent lines interfere" also predicts that
constant fills stay clean, which is exactly what the constant passes have done
all along.

    alternating fails, constants clean  ->  lines interfering, not bits failing
    constants fail                      ->  an ordinary stuck bit, and which one

    ./tools/diag/make_regtest.py -o regtest.bin
    ./tools/rom/gen_rom_images.py --logical regtest.bin -o firmware/rom_images.c
    cmake -S firmware -B firmware/build-reg -G Ninja \
        -DMPI_BEACON_PASS=ON -DMPI_BEACON_COUNT=21 -DMPI_BEACON_DONE=1
"""

import argparse
import struct
import sys
from pathlib import Path

# pdp11asm.py sits one level up, in tools/, because it is shared: the ROM-side
# tooling reads code with it too.  Only the instruments live in this directory.
sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

from pdp11asm import assemble

ROM_BASE = 0o100000
ROM_BYTES = 0o100000 - 0o1000
VECTOR = 0o160000
ENTRY = 0o160300

BEACON = 0o176700

B_ALIVE, B_DONE, B_ALT_FAIL, B_CONST_FAIL = 0, 1, 2, 3
B_BIT0 = 4                           # ...through B_BIT0 + 15
B_LANDED = B_BIT0 + 16               # 20: the PP arrived here from nowhere
BEACON_COUNT = B_LANDED + 1          # 21

# Unused ROM is filled with NOPs, and that is a correction rather than a detail.
#
# It was filled with zeros, and a zero word is HALT. So any stray jump into our
# 32 KB of ROM landed in a field of halts and the machine stopped dead -- while
# the stock ROM it replaces has real code at every address and would have carried
# on. The instrument was making excursions fatal that the real machine survives,
# and then reporting the death as though it were the machine's.
#
# A NOP fill turns the whole image into a slide: land anywhere below the program
# and execution walks up to it and re-enters the test. The machine recovers by
# itself, keeps testing, and a derail becomes a countable event rather than the
# end of the run.
NOP = 0o000240

# The slide passes through here on its way in; the power-up vector jumps straight
# to ENTRY and skips it. So this beacon means "arrived by falling through ROM",
# which no healthy start can produce.
LANDING = ENTRY - 6

# Both halves of each pair, because a coupling fault is not symmetric: a line
# dragged toward its neighbour shows up only when the two disagree in one
# particular direction, and testing 125252 alone would find half of them.
ALTERNATING = (0o125252, 0o052525)
CONSTANTS = (0o000000, 0o177777)


def _probe(value, acc):
    """Write one pattern to 177010, read it back, accumulate the difference."""
    return f"""
        mov #{value:o}, r2
        mov r2, @#177010
        mov @#177010, r4
        xor r2, r4
        bis r4, {acc}
"""


def program():
    body = "".join(_probe(v, "r5") for v in ALTERNATING)
    body += "".join(_probe(v, "r3") for v in CONSTANTS)
    return f"""
; ---- hold the central processor in reset, as every test here does: with it
;      stopped, nothing else can be writing the register we are measuring.
        mov #40, @#177716
        mov #70000, sp

soak:   mov #{BEACON + 2 * B_ALIVE:o}, r1
        tst (r1)
        clr r5
        clr r3
{body}
; ---- report. Alternating first, because it is the interesting one: a fault
;      only these patterns can see is a fault between lines rather than in one.
        tst r5
        beq noalt
        mov #{BEACON + 2 * B_ALT_FAIL:o}, r1
        tst (r1)

noalt:  tst r3
        beq noconst
        mov #{BEACON + 2 * B_CONST_FAIL:o}, r1
        tst (r1)

; ---- and which bits, from both accumulators together. All sixteen, not the
;      fifteen the plane index has: this register is being driven as a whole
;      word here and bit 15 is as much a line as any other.
noconst: mov r5, r2
        bis r3, r2
        mov #{BEACON + 2 * B_BIT0:o}, r1
        mov #20, r0
bloop:  bit #1, r2
        beq bskip
        tst (r1)
bskip:  add #2, r1
        asr r2
        dec r0
        bne bloop

        mov #{BEACON + 2 * B_DONE:o}, r1
        tst (r1)
        jmp @#soak

"""


def build():
    if BEACON + 2 * BEACON_COUNT > 0o177000:
        raise SystemExit(f"beacons from {BEACON:06o} reach the I/O page")

    words = ROM_BYTES // 2
    rom = bytearray(struct.pack(f"<{words}H", *([NOP] * words)))

    def put(addr, words):
        off = addr - ROM_BASE
        if off < 0 or off + 2 * len(words) > ROM_BYTES:
            raise SystemExit(f"{addr:06o} is outside the ROM")
        rom[off:off + 2 * len(words)] = struct.pack(f"<{len(words)}H", *words)

    put(VECTOR, [ENTRY, 0o340])
    landing = "        mov #%o, r1\n        tst (r1)\n" % (BEACON + 2 * B_LANDED)
    _, land = assemble(landing, LANDING)
    put(LANDING, land)
    _, code = assemble(program(), ENTRY)
    put(ENTRY, code)
    return bytes(rom), code


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("-o", "--output", type=Path, required=True)
    args = ap.parse_args()

    rom, code = build()
    args.output.write_bytes(rom)
    print(f"wrote {args.output}: {len(rom)} bytes, {len(code)} words of code")
    print(f"  beacons at {BEACON:06o}, {BEACON_COUNT} of them:")
    print(f"    1 alive   2 pass finished")
    print(f"    3 alternating patterns failed -- lines interfering")
    print(f"    4 constant patterns failed    -- a bit stuck")
    print(f"    5..{4 + 16} = bit 0..15")
    print(f"    {B_LANDED + 1} = the PP fell into ROM and slid back in")
    return 0


if __name__ == "__main__":
    sys.exit(main())
