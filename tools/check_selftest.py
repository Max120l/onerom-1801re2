#!/usr/bin/env python3
"""Check a dump taken from a board flashed with --selftest patterns.

Because every word carries its own index and window number, a bad dump can be
diagnosed rather than merely detected. The three failures that look identical in
a dump of real code are separable here:

  wrong window answered   every word is valid, but claims a different window
  address line wrong      words are valid but land at permuted indices; the
                          XOR between claimed and actual index names the bits
  data line wrong         a bit position never changes across the dump

    ./check_selftest.py dumped.bin
    ./check_selftest.py dumped.bin --window 3
"""

import argparse
import struct
import sys
from collections import Counter
from pathlib import Path

import selftest
from re2_convert import split_dump


def load(path: Path):
    raw = path.read_bytes()
    try:
        body, _ = split_dump(raw)
    except ValueError:
        body = raw
    if len(body) % 2:
        body = body[:-1]
    return list(struct.unpack(f"<{len(body) // 2}H", body))


def guess_window(w):
    """Which window most of the dump claims to belong to."""
    votes = Counter(selftest.identify(x, i) for i, x in enumerate(w))
    return votes.most_common(1)[0]


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("dump", type=Path)
    ap.add_argument("--window", type=int,
                    help="the window you addressed; inferred if omitted")
    args = ap.parse_args()

    w = load(args.dump)
    print(f"{args.dump.name}: {len(w)} words")

    # Stuck data lines first: they corrupt the index encoded in the low twelve
    # bits, so finding them is a prerequisite for reading anything else here.
    ones = [0] * 16
    for x in w:
        for b in range(16):
            ones[b] += (x >> b) & 1
    stuck = [(b, 1 if ones[b] else 0) for b in range(16)
             if ones[b] in (0, len(w))]
    stuck_mask = sum(1 << b for b, _ in stuck)

    if len(stuck) == 16:
        level = stuck[0][1]
        print(f"\nEvery bit is stuck at {level}. The board never drove the bus:")
        print("  no read strobe reaching pin 1, CS not asserted, the wrong")
        print("  window addressed, or the firmware not running.")
        return 1

    guess, votes = guess_window(w)
    window = args.window if args.window is not None else guess
    print(f"dump claims window {guess} for {votes}/{len(w)} words"
          f"{'' if args.window is None else f', checking against {window}'}")

    expected = selftest.image(window)
    n = min(len(w), len(expected))
    bad = [i for i in range(n) if w[i] != expected[i]]
    if not bad:
        print(f"\nexact match over {n} words -- the whole path works: address")
        print("capture, window decode, data drive and bus turnaround.")
        return 0

    if (args.window is not None and guess != args.window
            and votes > len(w) // 2 and not stuck):
        print(f"\nYou addressed window {args.window}, but the data is a clean")
        print(f"window {guess} pattern. The window decode is wrong, not the")
        print("data path -- check the chip code against the address bits, and")
        print("remember the code is their ones' complement.")
        return 1

    print(f"\n{len(bad)} of {n} words differ")

    if stuck:
        print("\ndata lines:")
        for b, v in stuck:
            print(f"  nAD{b} never changes (always {v}) -- not driven, or not")
            print("     reaching the reader")

    # Address lines: valid words sitting at the wrong index.  A stuck data bit
    # below bit 12 also perturbs the index a word claims, so a discrepancy
    # confined to stuck bits says nothing about the address lines.
    misplaced = Counter()
    invalid = 0
    for i in bad:
        claim = selftest.recover(window, w[i])
        if claim is None:
            invalid += 1
        elif (claim ^ i) & ~stuck_mask:
            misplaced[claim ^ i] += 1
    if misplaced:
        print("\naddress lines:")
        print(f"  {sum(misplaced.values())} words are valid pattern words that")
        print("  landed at the wrong index. XOR of claimed vs actual index:")
        for delta, count in misplaced.most_common(4):
            bits = [b for b in range(12) if (delta >> b) & 1]
            print(f"    {delta:#05x} x{count}  -> index bits {bits}, "
                  f"i.e. nAD{[b + 1 for b in bits]}")
        print("  A single dominant value means those lines are swapped, shorted")
        print("  or stuck between the reader and the board.")
    elif stuck:
        print("\n  Index discrepancies are all explained by the stuck data")
        print("  line(s) above -- no evidence of an address line fault.")
    if invalid:
        print(f"\n  {invalid} words are not valid pattern words in any position.")
    return 1


if __name__ == "__main__":
    sys.exit(main())
