#!/usr/bin/env python3
"""Build a test ROM that asks which *address* bits the CPU's RAM gets wrong.

The plane test in make_ramtest.py reports which data bits came back wrong, which
is the right question when one 1-bit-wide DRAM has died: each bit is one chip, so
the answer is a part number. It is the wrong question when every bit of every
plane fails at once, which is what this machine does once it is warm. Nothing
that is true of eight independent chips in two separate banks is true of all of
them simultaneously; what they share is the address bus and the strobes.

So ask about the address instead. Three passes over planes 1 and 2:

    fill every cell with 0,              read it all back
    fill every cell with 177777,         read it all back
    fill every cell with its own index,  read it all back

The first two are *blind to addressing*. Every cell holds the same value, so a
read that lands on the wrong cell still returns the right answer -- they can only
report a data-line fault. The third is sensitive to both, and it carries more
information than a pass/fail: since each cell holds its own address, whatever
comes back IS the address of the cell that actually got selected. XOR it with the
address we asked for and the difference names the address bits that went wrong.

    constant passes clean, address pass fails  ->  addressing, and here are the bits
    constant passes fail                       ->  data lines, as before

Bits that the constant passes implicated are removed from the address report,
because a stuck data bit shows up in the address pass too and would otherwise be
read as an address line. What survives is addressing and nothing else.

    ./make_addrtest.py -o addrtest.bin
    ./gen_rom_images.py --logical addrtest.bin -o ../firmware/rom_images.c
    cmake -S ../firmware -B ../firmware/build-addr -G Ninja \
        -DMPI_BEACONS=ON -DMPI_BEACON_COUNT=20

Only planes 1 and 2 are tested, and only through the plane ports. That is
deliberate: the ports are the one path that reaches the array with the central
processor held in reset, and the question here is about the shared address path,
which all three planes are on. Adding plane 0 would double the pass time to
re-answer a question the soak already answers.
"""

import argparse
import struct
import sys
from pathlib import Path

from pdp11asm import assemble

ROM_BASE = 0o100000
ROM_BYTES = 0o100000 - 0o1000        # 32256: stops at the I/O page
VECTOR = 0o160000                    # PP power-up PC, then PSW at 160002
ENTRY = 0o160300

# In ROM, for the same reason as make_ramtest: we answer those reads ourselves,
# so seeing a beacon needs no assumption about what the capture machine latches.
BEACON = 0o176700
PLANE_WORDS = 0o100000               # 32768 byte addresses per plane

# The beacon map. Deliberately not the soak's -- a frame that means something
# different should not look the same.
B_ALIVE, B_DONE, B_DATA_FAIL, B_ADDR_FAIL = 0, 1, 2, 3
B_A0 = 4                             # ...through B_A0 + 14, address bits 0..14
ADDR_BITS = 15                       # the plane index is 15 bits: 0..77777
BEACON_COUNT = B_A0 + ADDR_BITS      # 19

# Bit 15 is never exercised as data by the address pass, because the index it
# writes only counts to 77777. That is not a gap: 15 bits is the whole plane.


def program(plane_words):
    return f"""
; ---- hold the central processor in reset for the whole test.  Bit 5 of 177716
;      is its DCLO pin; with it stopped, nothing but us touches the planes.
        mov #40, @#177716

soak:   mov #{BEACON + 2 * B_ALIVE:o}, r1
        tst (r1)

; ---- pass 1: every cell zero.  r3 accumulates every bit that came back set,
;      which can only be a data line stuck high -- reading the wrong cell still
;      finds a zero in it.
        clr r3

        clr r0
z1:     mov r0, @#177010
        clr @#177014
        inc r0
        cmp r0, #{plane_words:o}
        blo z1

        clr r0
z2:     mov r0, @#177010
        mov @#177014, r4
        bis r4, r3
        inc r0
        cmp r0, #{plane_words:o}
        blo z2

; ---- pass 2: every cell all ones.  Complement what comes back and accumulate,
;      so this catches the bits stuck low that pass 1 structurally cannot see.
        clr r0
o1:     mov r0, @#177010
        mov #177777, @#177014
        inc r0
        cmp r0, #{plane_words:o}
        blo o1

        clr r0
o2:     mov r0, @#177010
        mov @#177014, r4
        com r4
        bis r4, r3
        inc r0
        cmp r0, #{plane_words:o}
        blo o2

; ---- pass 3: every cell holds its own index.  Now the data in a cell names the
;      cell, so what comes back names whichever cell was really selected, and the
;      XOR against what we asked for is the address error itself.
;
;      Note the fill runs to completion before the check starts. That is what
;      makes it an addressing test: if two indices select the same cell, the
;      second fill overwrites the first, and the check sees it.
        clr r2

        clr r0
a1:     mov r0, @#177010
        mov r0, @#177014
        inc r0
        cmp r0, #{plane_words:o}
        blo a1

        clr r0
a2:     mov r0, @#177010
        mov @#177014, r4
        xor r0, r4
        bis r4, r2
        inc r0
        cmp r0, #{plane_words:o}
        blo a2

; ---- report.  Data first, because it qualifies the address answer.
        tst r3
        beq nodata
        mov #{BEACON + 2 * B_DATA_FAIL:o}, r1
        tst (r1)

; ---- an address bit is only reported if the same bit position survived both
;      constant passes. A data line stuck at 0 makes every read differ from its
;      index in that position too, and calling that an address fault would point
;      at the wrong half of the board.
nodata: mov r2, r4
        bic r3, r4
        beq noaddr
        mov #{BEACON + 2 * B_ADDR_FAIL:o}, r1
        tst (r1)

noaddr: mov #{BEACON + 2 * B_A0:o}, r1
        mov #{ADDR_BITS:o}, r0
bloop:  bit #1, r4
        beq bskip
        tst (r1)
bskip:  add #2, r1
        asr r4
        dec r0
        bne bloop

        mov #{BEACON + 2 * B_DONE:o}, r1
        tst (r1)
        jmp @#soak

"""


def build(plane_words=PLANE_WORDS):
    """Assemble the test.  plane_words is a parameter so the simulator can run
    the real code over a small memory; the flashed image uses the real size."""
    if plane_words > 0o100000:
        raise SystemExit(f"plane_words {plane_words:06o} exceeds a plane")
    if BEACON < 0o100000:
        raise SystemExit(f"beacons at {BEACON:06o} are not in ROM")
    if BEACON + 2 * BEACON_COUNT > 0o177000:
        raise SystemExit(f"beacons from {BEACON:06o} reach the I/O page")

    rom = bytearray(b"\x00" * ROM_BYTES)

    def put(addr, words):
        off = addr - ROM_BASE
        if off < 0 or off + 2 * len(words) > ROM_BYTES:
            raise SystemExit(f"{addr:06o} is outside the ROM")
        rom[off:off + 2 * len(words)] = struct.pack(f"<{len(words)}H", *words)

    put(VECTOR, [ENTRY, 0o340])          # PC, PSW with interrupts masked
    _, code = assemble(program(plane_words), ENTRY)
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
    print(f"  power-up vector at {VECTOR:06o} -> {ENTRY:06o}")
    print(f"  beacons at {BEACON:06o}, {BEACON_COUNT} of them:")
    print(f"    1 alive   2 done   3 data lines bad   4 addressing bad")
    print(f"    5..{4 + ADDR_BITS} = address bit 0..{ADDR_BITS - 1}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
