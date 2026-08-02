#!/usr/bin/env python3
"""Run the UKNC monitor's own ROM checksum over the images we serve.

The monitor does not trust its ROMs. Five instructions into the startup test it
checksums all four of them, one window at a time, against sums mask-programmed
into the top four words of the last chip -- and prints "- ОШИБКА ПЗУ" if any
block disagrees. That check is the machine's own opinion of our images, so it is
worth being able to run it before flashing anything.

The algorithm is lifted from the routine at 160410 in the PP ROM:

    mov #4, r5              ; four blocks
    mov #176776, r1         ; sum downwards from just below the first checksum
    mov #7377, r2           ; 3839 words in the first block, 4096 after
  block:
    asl r0                  ; shift the failure mask up one
    clr r3
  word:
    add -(r1), r3
    adc r3                  ; end-around carry: a ones' complement sum
    sob r2, word
    asl r5
    cmp 176766(r5), r3      ; stored sum for this block
    beq ok
    inc r0                  ; this ROM failed
  ok:
    asr r5
    mov #10000, r2
    sob r5, block

r0 comes back as a bitmask of failed blocks, high block first, and the caller
shifts it into the error word that drives the message. The blocks turn out to be
exactly the four chip windows, so a failure names a chip:

    ./rom_checksum.py uknc_rom.bin
    ./rom_checksum.py --chips 205.bin 206.bin 207.bin 208.bin
"""

import argparse
import sys
from pathlib import Path

ROM_BASE = 0o100000
TOP = 0o176776                       # first checksum word; the sum starts below it
BLOCKS = [0o7377, 0o10000, 0o10000, 0o10000]

# Which chip covers which window, for naming a failure. Window index is the top
# three bits of the address; the chip's mask-programmed code is its complement.
# The 205 is the one in the DS4 socket -- the switchable window.
CHIP_OF_WINDOW = {4: "205 (DS4, banked window)", 5: "206", 6: "207", 7: "208"}


def checksum_blocks(image, base=ROM_BASE):
    """Yield (first, last, words, computed, stored) per block, top block first."""
    def word(addr):
        off = addr - base
        if off < 0 or off + 2 > len(image):
            raise SystemExit(f"address {addr:06o} is outside the image")
        return image[off] | (image[off + 1] << 8)

    r1, r5 = TOP, 4
    for count in BLOCKS:
        acc, first, last = 0, None, None
        for _ in range(count):
            r1 -= 2
            if first is None:
                first = r1
            last = r1
            acc += word(r1)
            if acc > 0xFFFF:                     # add, then adc: end-around carry
                acc = (acc & 0xFFFF) + 1
        r5 <<= 1
        yield first, last, count, acc, word(0o176766 + r5)
        r5 = (r5 >> 1) - 1


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("image", type=Path, nargs="?",
                    help="the whole 32256-byte logical ROM, based at 100000")
    ap.add_argument("--chips", type=Path, nargs=4, metavar=("205", "206", "207", "208"),
                    help="four 8192-byte window images instead, in address order")
    args = ap.parse_args()

    if args.chips:
        parts = []
        for p in args.chips:
            b = p.read_bytes()
            if len(b) not in (8192, 8190):
                print(f"{p}: expected 8192 bytes, got {len(b)}", file=sys.stderr)
                return 2
            parts.append(b)
        image = b"".join(parts)[:0o100000 - 0o1000]
    elif args.image:
        image = args.image.read_bytes()
    else:
        ap.error("give an image or --chips")

    if len(image) != 0o100000 - 0o1000:
        print(f"expected {0o100000 - 0o1000} bytes, got {len(image)}", file=sys.stderr)
        return 2

    failed = []
    for first, last, count, got, want in checksum_blocks(image):
        window = last >> 13
        chip = CHIP_OF_WINDOW.get(window, f"window {window}")
        ok = got == want
        if not ok:
            failed.append(chip)
        print(f"{last:06o}..{first:06o}  {count:5d} words  "
              f"computed {got:06o}  stored {want:06o}  "
              f"{'ok' if ok else 'MISMATCH'}   {chip}")

    if failed:
        print("\nthe machine would print '- ОШИБКА ПЗУ' for: " + ", ".join(failed))
        return 1
    print("\nall four blocks pass: the monitor's own ROM test is happy with these images")
    return 0


if __name__ == "__main__":
    sys.exit(main())
