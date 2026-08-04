#!/usr/bin/env python3
"""Build a test ROM that tests the *central processor's* RAM from the PP.

The machine's own startup test has been reporting "- ОШИБКА ОЗУ ЦП" -- central
processor RAM error. That is worth taking at face value, and it is worth
testing properly, which the stock test cannot do: it runs the check *on* the
central processor, using code that was itself copied into the RAM under test.
A memory fault there corrupts the tester before it can report on the testee.

This runs the test from the other side. The peripheral processor reaches the
central processor's memory through three registers, and can do so with the
central processor held in reset:

    177010  plane address register -- a byte index, latches all three planes
    177012  plane 0 data (byte)     -- the PP's own RAM / video plane 0
    177014  plane 1 & 2 data (word) -- low byte plane 1, high byte plane 2

The central processor's RAM *is* planes 1 and 2: its word at address A is
plane 1 byte A/2 in the low half and plane 2 byte A/2 in the high half. So one
word written to 177014 tests both planes at one address, and the two halves of
what comes back name which plane failed.

Writing 177010 latches the data registers from all three planes at once, so a
read is: put the address in 177010, then read 177014.

    ./make_ramtest.py -o ramtest.bin
    ./gen_rom_images.py --code 0 ramtest.bin -o ../firmware/rom_images.c

Results come back as beacons -- reads of reserved addresses in PP RAM, which
the board sees go past. Build the firmware with -DMPI_BEACONS=ON to have the
status LED blink them.
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

BEACON = 0o077700                    # in PP RAM, above anything the test touches
PLANE_WORDS = 0o100000               # 32768 byte addresses per plane

# Beacon numbers are the protocol; the firmware reads against this list.
(B_ALIVE, B_PP_RAM_PASS, B_PP_RAM_FAIL, B_PLANE_PASS,
 B_PLANE1_FAIL, B_PLANE2_FAIL, B_DONE) = range(7)

def program(ram_top, plane_words):
    return f"""
; ---- hold the central processor in reset for the whole test
;      Bit 5 of 177716 is its DCLO pin. With the CPU stopped, nothing else can
;      touch planes 1 and 2 while we are measuring them -- and if the machine's
;      trouble is a CPU running wild on corrupt code, this removes it as a
;      variable rather than reasoning about it.
        mov #40, @#177716

        mov #{BEACON + 2 * B_ALIVE:o}, r1
        tst (r1)

; ---- the PP's own RAM first, so a failure here is not mistaken for a plane
;      fault. Two passes: each word holds its address, then its complement, so
;      every bit takes both values at every location. One pass alone is a good
;      address-decode test and a weak stuck-bit test.
        clr r0
pf1:    mov r0, (r0)
        add #2, r0
        cmp r0, #{ram_top:o}
        blo pf1
        clr r0
pc1:    cmp r0, (r0)
        bne ppbad
        add #2, r0
        cmp r0, #{ram_top:o}
        blo pc1

        clr r0
pf2:    mov r0, r2
        com r2
        mov r2, (r0)
        add #2, r0
        cmp r0, #{ram_top:o}
        blo pf2
        clr r0
pc2:    mov r0, r2
        com r2
        cmp r2, (r0)
        bne ppbad
        add #2, r0
        cmp r0, #{ram_top:o}
        blo pc2

        mov #{BEACON + 2 * B_PP_RAM_PASS:o}, r1
        tst (r1)
        br planes

ppbad:  mov #{BEACON + 2 * B_PP_RAM_FAIL:o}, r1
        tst (r1)

; ---- planes 1 and 2: the central processor's RAM, two passes as above.
;      r3 accumulates the XOR of what came back against what was written, so
;      it holds exactly the bits that were ever wrong -- and nothing else. An
;      earlier version ORed the two values in instead of their difference, which
;      set bits from perfectly good data and reported both planes bad whenever
;      either was.
planes: clr r3

        clr r0
qf1:    mov r0, @#177010
        mov r0, @#177014
        inc r0
        cmp r0, #{plane_words:o}
        blo qf1

        clr r0
qc1:    mov r0, @#177010
        mov @#177014, r4
        xor r0, r4
        bis r4, r3
        inc r0
        cmp r0, #{plane_words:o}
        blo qc1

        clr r0
qf2:    mov r0, @#177010
        mov r0, r2
        com r2
        mov r2, @#177014
        inc r0
        cmp r0, #{plane_words:o}
        blo qf2

        clr r0
qc2:    mov r0, @#177010
        mov r0, r2
        com r2
        mov @#177014, r4
        xor r2, r4
        bis r4, r3
        inc r0
        cmp r0, #{plane_words:o}
        blo qc2

; ---- report. r3 is zero only if every word of both planes read back correctly.
        tst r3
        bne qbad
        mov #{BEACON + 2 * B_PLANE_PASS:o}, r1
        tst (r1)
        br finish

; ---- name the plane. The low byte of a plane word is plane 1 and the high byte
;      is plane 2, so the two halves of the accumulated difference say which
;      side of the central processor's memory is at fault -- and both beacons
;      appear if both are.
qbad:   mov r3, r2
        bic #177400, r2
        beq q1ok
        mov #{BEACON + 2 * B_PLANE1_FAIL:o}, r1
        tst (r1)
q1ok:   mov r3, r2
        bic #377, r2
        beq finish
        mov #{BEACON + 2 * B_PLANE2_FAIL:o}, r1
        tst (r1)

; ---- park, beaconing so the board can tell "finished" from "hung"
finish: mov #{BEACON + 2 * B_DONE:o}, r1
spin:   tst (r1)
        br spin
"""


def build(ram_top=BEACON, plane_words=PLANE_WORDS):
    """Assemble the test.  The two sizes are arguments so that the simulator in
    test/test_ramtest.py can exercise the same code over a small memory in
    seconds; the image that gets flashed uses the real ones."""
    rom = bytearray(b"\x00" * ROM_BYTES)

    def put(addr, words):
        off = addr - ROM_BASE
        if off < 0 or off + 2 * len(words) > ROM_BYTES:
            raise SystemExit(f"{addr:06o} is outside the ROM")
        rom[off:off + 2 * len(words)] = struct.pack(f"<{len(words)}H", *words)

    put(VECTOR, [ENTRY, 0o340])          # PC, PSW with interrupts masked
    _, code = assemble(program(ram_top, plane_words), ENTRY)
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
    print(f"  beacons at {BEACON:06o}: 0=alive 1=PP RAM ok 2=PP RAM bad "
          f"3=planes ok 4=plane 1 bad 5=plane 2 bad 6=done")
    return 0


if __name__ == "__main__":
    sys.exit(main())
