#!/usr/bin/env python3
"""Check the watchpoint pairs in firmware/watch.h against the real ROM.

A watchpoint is a claim about the stock monitor: that a particular address is
fetched immediately after another one, and that no ROM-reading loop produces
that adjacency by accident. Both halves are checkable offline, and both have
already been got wrong once -- the first version of the watch build scored a
single address and reported every watchpoint as hit, because the monitor's
startup checksum reads all 16127 ROM words as data.

    python3 test/test_watchpoints.py path/to/uknc_rom.bin
"""

import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "tools"))

import pdp11dis                                          # noqa: E402

WATCH_H = Path(__file__).resolve().parent.parent / "firmware" / "watch.h"
ROM_BASE = 0o100000

# What the disassembly is expected to show at each predecessor. Recording it
# here means a misaligned reading of the ROM fails the test rather than becoming
# a wrong answer blinked out on an LED.
EXPECTED = {
    0o160300: "mov @#172660, r4",
    0o160446: "beq 160452",
    0o160530: "bis #2, r0",
    0o172764: "jsr r5, 117204",
    0o174152: "mov #2000, sp",
    0o101004: "mov #4, r0",
}

# Single-word instructions that reach the next word with no bus cycle in
# between: no memory operand, so nothing can land between the two fetches.
SAFE_SINGLE_WORD = ("br", "bne", "beq", "bge", "blt", "bgt", "ble", "bpl",
                    "bmi", "bhi", "blos", "bvc", "bvs", "bcc", "bcs", "nop")


def parse_pairs():
    text = WATCH_H.read_text()
    block = re.search(r"#define MPI_WATCH_PAIRS \{(.*?)\n\}", text, re.S)
    if not block:
        raise SystemExit("could not find MPI_WATCH_PAIRS in watch.h")
    return [(int(a, 8), int(p, 8))
            for a, p in re.findall(r"\{\s*0(\d+),\s*0(\d+)\s*\}", block.group(1))]


def decode_at(image, addr):
    s = pdp11dis.Stream(image, ROM_BASE, addr)
    _, words, text = pdp11dis.decode(s)
    return len(words), text


def checksum_reads():
    """Bus addresses the ROM checksum loop at 160432 puts out, in order.

    Per iteration: fetch the add, read the word, fetch the adc, fetch the sob.
    The data reads descend, and three fetches sit between any two of them.
    """
    seq = []
    addr = 0o176774
    for _ in range(16127):                # 3839 + 3 * 4096: the whole ROM
        seq += [0o160432, addr, 0o160434, 0o160436]
        addr -= 2
    return seq


def plane_copy_reads():
    """The loop at 173270 that streams ROM into the video planes.

    Ascending this time, which is the interesting direction: if anything could
    forge an A, A+2 adjacency from data reads it would be this. Its own loop
    fetches still sit in between.
    """
    seq = []
    addr = 0o160000
    for _ in range(0o5305):               # the count the monitor uses
        seq += [0o173270, 0o173272, addr, 0o177014,
                0o173274, 0o173276, 0o177010, 0o173300]
        addr += 2
    return seq


def main() -> int:
    if len(sys.argv) != 2:
        print(__doc__)
        return 2
    image = Path(sys.argv[1]).read_bytes()
    pairs = parse_pairs()
    failures = 0

    print(f"{len(pairs)} watchpoints from {WATCH_H.name}\n")
    for addr, prev in pairs:
        length, text = decode_at(image, prev)
        note = []
        ok = True

        if addr != prev + 2:
            note.append(f"FAIL: {addr:06o} is not {prev:06o}+2")
            ok = False
        if prev in EXPECTED and text != EXPECTED[prev]:
            note.append(f"FAIL: expected {EXPECTED[prev]!r}, disassembles as {text!r}")
            ok = False
        if length < 2:
            mnemonic = text.split()[0]
            if mnemonic not in SAFE_SINGLE_WORD:
                note.append(f"FAIL: {mnemonic!r} is one word and may touch memory, "
                            "so a cycle could land between the two fetches")
                ok = False
            else:
                note.append("(single word, falls through with no bus cycle)")

        print(f"  {prev:06o} -> {addr:06o}  {text:<24} "
              f"{length} word{'s' if length > 1 else ''}  {'ok' if ok else ''}")
        for n in note:
            print(f"      {n}")
        failures += not ok

    print("\nno ROM-reading loop forges these adjacencies:")
    for name, seq in (("checksum (descending)", checksum_reads()),
                      ("plane copy (ascending)", plane_copy_reads())):
        seen = {(seq[i - 1], seq[i]) for i in range(1, len(seq))}
        clash = [f"{p:06o}->{a:06o}" for a, p in pairs if (p, a) in seen]
        print(f"  {name:<24} {'FAIL: ' + ', '.join(clash) if clash else 'clear'}")
        failures += bool(clash)

    # The bug this file exists to prevent: under the old rule -- score any read
    # of the address -- the checksum alone lights every watchpoint.
    reads = set(checksum_reads())
    would_hit = [a for a, _ in pairs if a in reads]
    print(f"\nunder the old address-only rule the checksum alone would light "
          f"{len(would_hit)} of {len(pairs)} watchpoints")

    print("\n" + ("all checks passed" if not failures else f"{failures} failure(s)"))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
