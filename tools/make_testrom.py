#!/usr/bin/env python3
"""Build a test ROM that runs on the UKNC's peripheral processor instead of the
system monitor.

Because the board *is* the ROM, it owns the machine from reset. The PP takes its
power-up PC and PSW from a HALT-mode vector at 160000/160002, which lands at
offset 0 of the code 0 image -- so putting our own vector there means our code
runs before anything else does, with no monitor to work around.

Reporting results is the hard part: at this stage the PP has no screen it can
reach on its own. So the program signals by *reading* from reserved addresses.
Every address is on the AD lines at the strobe, and the board latches every
strobe, so the board sees each beacon go past. It is both the ROM under the
program and the instrument watching it, which no mask ROM could be.

Beacons sit in PP RAM near the top, so a read there disturbs nothing; only the
address matters, never the data.

    ./make_testrom.py -o testrom.bin
    ./gen_rom_images.py --raw testrom.bin -o ../firmware/rom_images.c

Test it in ukncbtl before it goes near hardware -- the emulator takes the same
32 KB image and costs nothing to be wrong in.
"""

import argparse
import struct
import sys
from pathlib import Path

from pdp11asm import assemble

ROM_BASE = 0o100000
ROM_BYTES = 0o100000 - 0o1000        # 32256: stops at the I/O page
VECTOR = 0o160000                    # PP power-up PC, then PSW at 160002
ENTRY = 0o160300                     # where the stock monitor starts too

BEACON = 0o077700                    # in PP RAM, above anything the tests touch
RAM_TOP = 0o077600                   # leave the beacons alone while testing RAM

# Beacon numbers are the protocol. Keep them stable; the firmware side and any
# logic analyser capture are read against this list.
B_ALIVE, B_RAM_PASS, B_RAM_FAIL, B_DONE = 0, 1, 2, 3

PROGRAM = f"""
; ---- we are executing, before anything else in the machine has run
        mov #{BEACON + 2 * B_ALIVE}, r1
        tst (r1)

; ---- PP RAM, pass 1: each word holds its own address.
;      This is the address-decode test: a word can only read back correctly if
;      its address decoded uniquely, so shorted or open address lines show up
;      as words landing on each other.
        clr r0
fill1:  mov r0, (r0)
        add #2, r0
        cmp r0, #{RAM_TOP}
        blo fill1

        clr r0
chk1:   cmp r0, (r0)
        bne ramfail
        add #2, r0
        cmp r0, #{RAM_TOP}
        blo chk1

; ---- PP RAM, pass 2: each word holds the complement of its address.
;      Pass 1 alone is weak on stuck bits: a location only proves bit N works
;      if its own address happens to have bit N set, so a bit stuck low at an
;      address that never sets it goes unseen. The complement makes every bit
;      take both values at every location, across the two passes.
        clr r0
fill2:  mov r0, r2
        com r2
        mov r2, (r0)
        add #2, r0
        cmp r0, #{RAM_TOP}
        blo fill2

        clr r0
chk2:   mov r0, r2
        com r2
        cmp r2, (r0)
        bne ramfail
        add #2, r0
        cmp r0, #{RAM_TOP}
        blo chk2

        mov #{BEACON + 2 * B_RAM_PASS}, r1
        tst (r1)
        br finish

ramfail:
        mov #{BEACON + 2 * B_RAM_FAIL}, r1
        tst (r1)

; ---- park, beaconing so the board can tell "finished" from "hung"
finish: mov #{BEACON + 2 * B_DONE}, r1
spin:   tst (r1)
        br spin
"""


def build():
    rom = bytearray(b"\x00" * ROM_BYTES)

    def put(addr, words):
        off = addr - ROM_BASE
        if off < 0 or off + 2 * len(words) > ROM_BYTES:
            raise SystemExit(f"{addr:06o} is outside the ROM")
        rom[off:off + 2 * len(words)] = struct.pack(f"<{len(words)}H", *words)

    put(VECTOR, [ENTRY, 0o340])          # PC, PSW with interrupts masked
    _, code = assemble(PROGRAM, ENTRY)
    put(ENTRY, code)
    return bytes(rom), code


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("-o", "--output", type=Path, required=True)
    ap.add_argument("--listing", action="store_true")
    args = ap.parse_args()

    rom, code = build()
    args.output.write_bytes(rom)
    print(f"wrote {args.output}: {len(rom)} bytes, {len(code)} words of code")
    print(f"  power-up vector at {VECTOR:06o} -> {ENTRY:06o}, PSW 000340")
    print(f"  beacons at {BEACON:06o}: "
          f"{B_ALIVE}=alive {B_RAM_PASS}=RAM ok {B_RAM_FAIL}=RAM bad {B_DONE}=done")
    if args.listing:
        print("\n listing:")
        for i, w in enumerate(code):
            print(f"  {ENTRY + 2 * i:06o}  {w:06o}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
