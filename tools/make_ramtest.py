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

# Beacons live in ROM, not RAM, and the difference is not cosmetic.
#
# A beacon works by being an address on the bus that the board sees go past. In
# PP RAM that relies on the capture machine latching cycles for addresses we do
# not serve -- true if the socket's SYN is the raw bus strobe, and every result
# this project has gathered has been a ROM address, so it has never actually
# been demonstrated. In ROM it needs no such assumption: we answer the read
# ourselves, so we cannot fail to see it.
#
# 176700 is inside the last window, past the end of this program and past
# anything it touches. Reading ROM is harmless; only the address matters.
BEACON = 0o176700
PLANE_WORDS = 0o100000               # 32768 byte addresses per plane

# Where the PP's own RAM ends, for the walk below. Its own constant, and not
# derived from anything else: it used to default to BEACON, which was fine while
# the beacons lived in RAM and became a bug the moment they moved into ROM. The
# walk then ran off the end of PP RAM into ROM, where a write gets no reply, the
# PP traps through a vector the walk had just overwritten, and the machine
# wedges after the first beacon. Two constants that happen to be equal are not
# one constant.
PP_RAM_TOP = 0o077600

# The same verdict, left in memory rather than blinked.
#
# Beacons are addresses on the bus, which is what the board can see but is
# exactly what an emulator cannot show you without being modified. ukncbtl will
# happily run this ROM -- it loads uknc_rom.bin from its working directory, 32256
# bytes, the size of our images -- so the useful thing is a result you can read
# in its memory view. Written after every test has finished, so the RAM walk
# below cannot have clobbered it.
RESULT = 0o077660                    # status bits, see RES_* below
RESULT_MASK = 0o077662               # every plane bit that was ever wrong
# Powers of two, and written as such. An earlier version set RES_DONE to a
# Python 10 -- decimal ten, binary 1010 -- which overlapped RES_PLANE1_BAD, so a
# healthy machine and one with a dead plane 1 reported the identical status
# word. The test did not catch it because both sides of the comparison shared
# the error, which is how a diagnostic ends up lying with confidence.
RES_PP_RAM_OK, RES_PLANE1_BAD, RES_PLANE2_BAD, RES_DONE = 1, 2, 4, 8
RES_STUCK = 16
IMMEDIATE = 0o077664                 # bits that failed even on an instant reread

# Beacon numbers are the protocol; the firmware reads against this list.
(B_ALIVE, B_PP_RAM_PASS, B_PP_RAM_FAIL, B_PLANE_PASS,
 B_PLANE1_FAIL, B_PLANE2_FAIL, B_DONE, B_STUCK) = range(8)

# Beacons 8..15 are the bit positions that came back wrong, one per bit of the
# failing plane's byte. In a bank built from 1-bit-wide DRAM each bit is one
# chip, so this turns "plane 1 is bad" into a list of parts to unsolder.
B_BIT0 = 8
BEACON_COUNT = 16

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
        clr r5

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
        bis #{RES_PP_RAM_OK:o}, r5
        br planes

ppbad:  mov #{BEACON + 2 * B_PP_RAM_FAIL:o}, r1
        tst (r1)

; ---- planes 1 and 2: the central processor's RAM, two passes as above.
;      r3 accumulates the XOR of what came back against what was written, so
;      it holds exactly the bits that were ever wrong -- and nothing else. An
;      earlier version ORed the two values in instead of their difference, which
;      set bits from perfectly good data and reported both planes bad whenever
;      either was.
; ---- first, an immediate pass: write a word and read it straight back, before
;      anything else has had a chance to touch the array. A bit that is already
;      wrong here cannot hold the value at all; a bit that is right here and
;      wrong in the passes below held it and then lost it. Dead chip versus
;      leaky one, and the difference decides what to do about it.
;
;      Note the second write to 177010. Writing 177014 updates the register as
;      well as the array, so reading it straight back returns what was just
;      written and proves nothing; re-writing the address register re-latches
;      the data registers from the memory itself.
planes: clr r3

        clr r0
qi1:    mov r0, @#177010
        mov r0, @#177014
        mov r0, @#177010
        mov @#177014, r4
        xor r0, r4
        bis r4, r3
        inc r0
        cmp r0, #{plane_words:o}
        blo qi1

;      ...and again with the complement. Writing the address as the data leaves
;      the high byte only ever counting 0..127 over a 32768-word plane, so bit 7
;      of plane 2 is never once set. The delayed passes cover it because the
;      second of them writes the complement; the immediate pass needs the same
;      treatment or it silently cannot see half of plane 2.
        clr r0
qi2:    mov r0, r2
        com r2
        mov r0, @#177010
        mov r2, @#177014
        mov r0, @#177010
        mov @#177014, r4
        xor r2, r4
        bis r4, r3
        inc r0
        cmp r0, #{plane_words:o}
        blo qi2

        mov r3, @#{IMMEDIATE:o}
        clr r3

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
        bis #{RES_PLANE1_BAD:o}, r5
q1ok:   mov r3, r2
        bic #377, r2
        beq bits
        mov #{BEACON + 2 * B_PLANE2_FAIL:o}, r1
        tst (r1)
        bis #{RES_PLANE2_BAD:o}, r5

; ---- name the bits. Fold the two bytes together -- which plane they belong to
;      is already said above -- and beacon one address per bit still set. Each
;      bit of a plane is one column of the bank, so on 1-bit-wide DRAM this is
;      a list of chips rather than a diagnosis to interpret.
bits:   mov r3, r2
        mov r3, r4
        swab r4
        bis r4, r2
        bic #177400, r2
        mov #{BEACON + 2 * B_BIT0:o}, r1
        mov #10, r0
bloop:  bit #1, r2
        beq bskip
        tst (r1)
bskip:  add #2, r1
        asr r2
        dec r0
        bne bloop

; ---- was any of it already wrong the instant it was written?
finish: tst @#{IMMEDIATE:o}
        beq park
        mov #{BEACON + 2 * B_STUCK:o}, r1
        tst (r1)
        bis #{RES_STUCK:o}, r5

; ---- leave the verdict where a debugger or an emulator's memory view can read
;      it, then park, beaconing so the board can tell "finished" from "hung"
park:   bis #{RES_DONE:o}, r5
        mov r5, @#{RESULT:o}
        mov r3, @#{RESULT_MASK:o}
        mov #{BEACON + 2 * B_DONE:o}, r1
        clr r2

; ---- and then sit there driving every plane data line, for the scope.
;
;      Parking silently wasted the one thing a bench probe needs: a stimulus
;      where every bit is doing the same thing. The test's own pattern is the
;      opposite of that -- it writes the address as the data, so bit 0 of the
;      low byte toggles on every single write while bit 7 toggles once per 128.
;      A healthy bit 7 looks sluggish next to bit 0 for that reason alone, and
;      comparing them says nothing.
;
;      Writing all-zeros then all-ones to one address toggles all sixteen lines
;      at the same rate, so every bit becomes comparable with every other -- and
;      in particular bit 7 of plane 1 with bit 7 of plane 2, which is the same
;      position in the same kind of chip and the only fair comparison there is.
;      A weak or dead line stands out against its own twin.
;
;      The beacon read stays in the loop so the LED keeps reporting.
spin:   tst (r1)
        mov r2, @#177010
        clr @#177014
        mov #177777, @#177014
        br spin
"""


def build(ram_top=PP_RAM_TOP, plane_words=PLANE_WORDS):
    """Assemble the test.  The two sizes are arguments so that the simulator in
    test/test_ramtest.py can exercise the same code over a small memory in
    seconds; the image that gets flashed uses the real ones."""
    # The sizes are parameters, so check them rather than trusting the caller --
    # including the defaults, which are the ones that get flashed and the ones
    # the scaled-down simulator run never exercises.
    if ram_top > 0o100000:
        raise SystemExit(f"ram_top {ram_top:06o} runs past the end of PP RAM")
    if BEACON < 0o100000:
        raise SystemExit(f"beacons at {BEACON:06o} are not in ROM")
    if BEACON + 2 * BEACON_COUNT > 0o177000:
        raise SystemExit(f"beacons from {BEACON:06o} reach the I/O page")
    if RESULT >= ram_top or RESULT_MASK >= ram_top:
        pass          # the verdict is written after the walk, so overlap is fine
    if plane_words > 0o100000:
        raise SystemExit(f"plane_words {plane_words:06o} exceeds a plane")

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
          f"3=planes ok 4=plane 1 bad 5=plane 2 bad 6=done 7=stuck, "
          f"{B_BIT0}..{B_BIT0 + 7}=failing bit 0..7")
    print(f"  and in memory: {RESULT:06o} = status "
          f"(1 PP RAM ok, 2 plane 1 bad, 4 plane 2 bad, 10 done, octal), "
          f"{RESULT_MASK:06o} = bits ever wrong")
    return 0


if __name__ == "__main__":
    sys.exit(main())
