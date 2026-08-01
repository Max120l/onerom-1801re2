#!/usr/bin/env python3
"""Check a dump taken from a board flashed with --selftest patterns.

Accepts a raw .bin, or an "ADDR : DATA" log straight off a serial terminal. A
log is worth more: the address is explicit, so every window is checked at once
and a window answering out of turn is visible.

Because every word carries its own index and window number, a bad dump can be
diagnosed rather than merely detected:

  never answered          every bit stuck at one level
  wrong window answered   valid words, but claiming a different window
  data line wrong         a bit position never changes
  address line wrong      valid words at permuted indices; the XOR names the bits
  bit-shifted             the word is the right data displaced by one position,
                          which is a serial-read artifact rather than a bus fault

    ./check_selftest.py selftest.log
    ./check_selftest.py dumped.bin --window 3
"""

import argparse
import sys
from collections import Counter
from pathlib import Path

import selftest

IO_PAGE_WORDS = (0o177000 - 0o160000) // 2      # 3840


def collect(pairs, kind, only_window=None, fixed_code=None):
    """[(addr, code, index, got, expected)] for every address that should answer.

    A log carries addresses, so each word's window follows from where it was
    read. A raw .bin carries none: it is one window's worth of data with no
    record of which, so the caller has to say (or we infer it from the data,
    which the pattern makes possible).
    """
    out = []
    for addr in sorted(pairs):
        if kind == "log":
            code = selftest.window_of(addr)
            index = (addr >> 1) & 0xFFF
        else:
            code = fixed_code
            index = addr >> 1
        if only_window is not None and code != only_window:
            continue
        if index >= (IO_PAGE_WORDS if code == 0 else selftest.WORDS):
            continue
        out.append((addr, code, index, pairs[addr], selftest.word(code, index)))
    return out


def infer_code(pairs):
    """Which window's pattern does this data look like?  Majority vote."""
    votes = Counter(selftest.identify(w, addr >> 1) for addr, w in pairs.items())
    return votes.most_common(1)[0][0]


def shift_signature(bad):
    """How many bad words are the right data displaced by one bit position?"""
    right = sum(1 for _, _, _, g, e in bad if (g & 0xFFF) == ((e >> 1) & 0xFFF))
    left = sum(1 for _, _, _, g, e in bad if (g & 0xFFE) == ((e << 1) & 0xFFE))
    return right, left


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("dump", type=Path)
    ap.add_argument("--window", type=int,
                    help="check only this chip code; default is every window "
                         "present in the dump")
    args = ap.parse_args()

    pairs, kind = selftest.load_dump(args.dump)
    if not pairs:
        print("no data found in that file", file=sys.stderr)
        return 1
    print(f"{args.dump.name}: {len(pairs)} words, read as a {kind}")

    fixed = None
    if kind == "bin":
        fixed = args.window if args.window is not None else infer_code(pairs)
        how = "given" if args.window is not None else "inferred from the data"
        print(f"no addresses in a .bin, so treating it as window {fixed} ({how})")

    # Per-window summary -- this is the window decode, checked end to end.
    print("\n code   range (octal)      answered   correct")
    for code in range(8):
        rows = collect(pairs, kind, code, fixed)
        if not rows:
            continue
        bad = [r for r in rows if r[3] != r[4]]
        lo = ((~code) & 7) << 13
        print(f"  {code:o}    {lo:06o}-{lo + (len(rows) * 2) - 2:06o}   "
              f"{len(rows):>6}   {len(rows) - len(bad):>6}"
              f"   {100.0 * (len(rows) - len(bad)) / len(rows):5.1f}%")

    rows = collect(pairs, kind, args.window if kind == "log" else fixed, fixed)
    bad = [r for r in rows if r[3] != r[4]]
    if not bad:
        print(f"\nexact match over {len(rows)} words -- the whole path works:")
        print("address capture, window decode, data drive and bus turnaround.")
        return 0

    print(f"\n{len(bad)} of {len(rows)} words wrong ({100.0 * len(bad) / len(rows):.2f}%)")

    # Stuck data lines.
    got = [r[3] for r in rows]
    ones = [sum((g >> b) & 1 for g in got) for b in range(16)]
    stuck = [(b, 1 if ones[b] else 0) for b in range(16) if ones[b] in (0, len(got))]
    if len(stuck) == 16:
        print(f"\nEvery bit stuck at {stuck[0][1]} -- the board never drove the bus.")
        return 1
    for b, v in stuck:
        print(f"  nAD{b} never changes (always {v}) -- not driven, or not reaching"
              " the reader")

    # A one-position displacement is mechanically distinct from a bus fault.
    right, left = shift_signature(bad)
    if right == len(bad) or left == len(bad):
        way = "right" if right == len(bad) else "left"
        print(f"\n  ** every wrong word is the correct data shifted one bit {way}.")
        print("  The board selected and drove the right word; only its alignment")
        print("  is off. A parallel bus cannot displace bits like this, so the")
        print("  shift belongs to whatever samples the lines -- a serial read")
        print("  gaining or losing a clock, not an address or data line fault.")
        return 1
    if max(right, left) > len(bad) // 20:      # ignore chance coincidences
        print(f"\n  {right} wrong words are the correct data shifted one bit right,")
        print(f"  {left} shifted one bit left, out of {len(bad)}.")

    # Address lines: valid pattern words sitting at the wrong index.
    stuck_mask = sum(1 << b for b, _ in stuck)
    misplaced = Counter()
    for _, code, index, g, _ in bad:
        claim = selftest.recover(code, g)
        if claim is not None and (claim ^ index) & ~stuck_mask:
            misplaced[claim ^ index] += 1
    if misplaced:
        print("\naddress lines:")
        for delta, count in misplaced.most_common(4):
            bits = [b for b in range(12) if (delta >> b) & 1]
            print(f"    {delta:#05x} x{count}  -> index bits {bits}, "
                  f"i.e. nAD{[b + 1 for b in bits]}")
    return 1


if __name__ == "__main__":
    sys.exit(main())
