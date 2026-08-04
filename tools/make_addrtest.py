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
B_ALIVE, B_DONE, B_DATA_FAIL, B_ADDR_FAIL, B_REG_FAIL = 0, 1, 2, 3, 4
B_A0 = 5                             # ...through B_A0 + 14, address bits 0..14
ADDR_BITS = 15                       # the plane index is 15 bits: 0..77777

# Phase markers, one per loop, lit on entry. These exist because the machine
# found a way to fail that no verdict can describe: it wedged partway through a
# pass and never reached the report at all, leaving the last good frame standing
# and the test looking healthy while the screen sat frozen.
#
# A test that dies before it can speak is indistinguishable from a test with
# nothing to say -- unless it says where it got to. Paired with a firmware built
# -DMPI_BEACON_PASS=ON, which clears the frame on every DONE, the phases stop
# advancing exactly where the PP stopped, and the last lit one names the loop.
B_PH_REG = B_A0 + ADDR_BITS          # 20
(B_PH_ZFILL, B_PH_ZCHK, B_PH_OFILL, B_PH_OCHK,
 B_PH_AFILL, B_PH_ACHK) = range(B_PH_REG + 1, B_PH_REG + 7)
BEACON_COUNT = B_PH_ACHK + 1         # 27

# B_REG_FAIL closes a gap that everything else in this file quietly assumed shut.
#
# The PP names a cell by writing port 177010, and no test here has ever checked
# that the register accepted what it was given. If 177010 itself drops bits, the
# array is asked for the wrong cell and dutifully returns it -- identical
# symptoms, and the DRAMs, their address lines and their multiplexer are all
# innocent. One extra pass, and it separates "the address never got out of the
# register" from "the address got out and the addressing mangled it".
#
# Read it cold first. If this pulse is lit on a machine that is otherwise
# reporting clean, 177010 is not a readback register on this hardware and the
# pulse means nothing -- there is no way to settle that from here.

# Bit 15 is never exercised as data by the address pass, because the index it
# writes only counts to 77777. That is not a gap: 15 bits is the whole plane.


def program(plane_words, stop_on_fail=False):
    return f"""
; ---- hold the central processor in reset for the whole test.  Bit 5 of 177716
;      is its DCLO pin; with it stopped, nothing but us touches the planes.
        mov #40, @#177716

; ---- park the stack somewhere identifiable.
;      This program never uses a stack, but a trap does: it pushes the old PC and
;      PSW before vectoring. Left at whatever the PP powers up with, those writes
;      land unpredictably -- possibly in the first few words of memory, where the
;      board is watching for the vector fetch that names the trap. High and out
;      of the way, they cannot be confused with it.
        mov #70000, sp

soak:   mov #{BEACON + 2 * B_ALIVE:o}, r1
        tst (r1)

; ---- pass 0: the plane address register itself.  Write an index, read it
;      straight back, accumulate the difference in r5. This touches no memory at
;      all, so it cannot be confused by anything downstream of the register --
;      and if it fails, nothing downstream has been tested, because every other
;      pass here asks for cells through this same register.
        clr r5

        mov #{BEACON + 2 * B_PH_REG:o}, r1
        tst (r1)
        clr r0
rg:     mov r0, @#177010
        mov @#177010, r4
        xor r0, r4
        bis r4, r5
;      Reported from inside the loop, not after it. The end-of-pass verdict is
;      lost if the loop never ends -- and this is the loop the machine stops in.
;      Once r5 is dirty this beacons every iteration, which roughly doubles the
;      loop time; that only happens after the answer has already been obtained.
        beq nreg
        mov #{BEACON + 2 * B_REG_FAIL:o}, r1
        tst (r1)
nreg:   inc r0
        cmp r0, #{plane_words:o}
        blo rg

; ---- verdict now, not at the end of the pass.
;      Everything here used to be reported together, after the index check --
;      which meant a pass that wedged partway through threw away every finding
;      that preceded it. On hardware that is not hypothetical: the PP hung in the
;      index fill with the register pass long since complete, and the frame could
;      not say whether 177010 had already been failing. A verdict withheld until
;      the end is a verdict lost to anything that stops before it.
        tst r5
        beq r5ok
        mov #{BEACON + 2 * B_REG_FAIL:o}, r1
        tst (r1)
r5ok:

; ---- pass 1: every cell zero.  r3 accumulates every bit that came back set,
;      which can only be a data line stuck high -- reading the wrong cell still
;      finds a zero in it.
        clr r3

        mov #{BEACON + 2 * B_PH_ZFILL:o}, r1
        tst (r1)
        clr r0
z1:     mov r0, @#177010
        clr @#177014
        inc r0
        cmp r0, #{plane_words:o}
        blo z1

        mov #{BEACON + 2 * B_PH_ZCHK:o}, r1
        tst (r1)
        clr r0
z2:     mov r0, @#177010
        mov @#177014, r4
        bis r4, r3
        inc r0
        cmp r0, #{plane_words:o}
        blo z2

        tst r3
        beq r3ok1
        mov #{BEACON + 2 * B_DATA_FAIL:o}, r1
        tst (r1)
r3ok1:

; ---- pass 2: every cell all ones.  Complement what comes back and accumulate,
;      so this catches the bits stuck low that pass 1 structurally cannot see.
        mov #{BEACON + 2 * B_PH_OFILL:o}, r1
        tst (r1)
        clr r0
o1:     mov r0, @#177010
        mov #177777, @#177014
        inc r0
        cmp r0, #{plane_words:o}
        blo o1

        mov #{BEACON + 2 * B_PH_OCHK:o}, r1
        tst (r1)
        clr r0
o2:     mov r0, @#177010
        mov @#177014, r4
        com r4
        bis r4, r3
        inc r0
        cmp r0, #{plane_words:o}
        blo o2

        tst r3
        beq r3ok2
        mov #{BEACON + 2 * B_DATA_FAIL:o}, r1
        tst (r1)
r3ok2:

; ---- pass 3: every cell holds its own index.  Now the data in a cell names the
;      cell, so what comes back names whichever cell was really selected, and the
;      XOR against what we asked for is the address error itself.
;
;      Note the fill runs to completion before the check starts. That is what
;      makes it an addressing test: if two indices select the same cell, the
;      second fill overwrites the first, and the check sees it.
        clr r2

        mov #{BEACON + 2 * B_PH_AFILL:o}, r1
        tst (r1)
        clr r0
a1:     mov r0, @#177010
        mov r0, @#177014
        inc r0
        cmp r0, #{plane_words:o}
        blo a1

        mov #{BEACON + 2 * B_PH_ACHK:o}, r1
        tst (r1)
        clr r0
a2:     mov r0, @#177010
        mov @#177014, r4
        xor r0, r4
        bis r4, r2
        inc r0
        cmp r0, #{plane_words:o}
        blo a2

; ---- an address bit is only reported if the same bit position survived both
;      constant passes. A data line stuck at 0 makes every read differ from its
;      index in that position too, and calling that an address fault would point
;      at the wrong half of the board.
        mov r2, r4
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
{_stop(stop_on_fail)}
        jmp @#soak

"""


def _stop(enabled):
    """Freeze the frame at the first pass that fails anything.

    Without this the frame is the union of every pass since power-on, which is
    right for "did this machine ever fail" and useless for "what does the fault
    look like when it arrives". On hardware it saturated: a partial failure of
    the odd address bits, left running, eventually collapsed completely, and the
    frame showed both at once with no way to tell which came first.

    Stopping costs nothing, because a clean pass lights only ALIVE and DONE --
    both lit anyway -- so the frozen frame is exactly the first failing pass and
    nothing else. The LED keeps repeating it forever; a frame that stops changing
    is the signal that this fired.
    """
    if not enabled:
        return ""
    return """
        mov r3, r0
        bis r2, r0
        bis r5, r0
        beq going
stop:   br stop
going:"""


def build(plane_words=PLANE_WORDS, stop_on_fail=False):
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
    _, code = assemble(program(plane_words, stop_on_fail), ENTRY)
    put(ENTRY, code)
    return bytes(rom), code


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("-o", "--output", type=Path, required=True)
    ap.add_argument("--stop-on-fail", action="store_true",
                    help="freeze the frame at the first failing pass, so it shows\n"
                         "what the fault looks like on arrival rather than "
                         "the union of everything that followed")
    args = ap.parse_args()

    rom, code = build(stop_on_fail=args.stop_on_fail)
    args.output.write_bytes(rom)
    print(f"wrote {args.output}: {len(rom)} bytes, {len(code)} words of code")
    print(f"  power-up vector at {VECTOR:06o} -> {ENTRY:06o}")
    print(f"  beacons at {BEACON:06o}, {BEACON_COUNT} of them:")
    print(f"    1 alive   2 done   3 data lines bad   4 addressing bad")
    print(f"    5 plane address register (177010) bad")
    print(f"    6..{5 + ADDR_BITS} = address bit 0..{ADDR_BITS - 1}")
    print(f"    {B_PH_REG + 1}..{B_PH_ACHK + 1} = phase reached: "
          f"register, fill 0, check 0, fill 1s, check 1s, fill addr, check addr")
    return 0


if __name__ == "__main__":
    sys.exit(main())
