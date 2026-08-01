#!/usr/bin/env python3
"""Work out why a 1801RE2 dump looks wrong.

A reader that ignores RPLY cannot tell "the chip did not answer" from "the chip
answered with these bits".  When the chip stays silent the AD lines float and
settle wherever the rig's pull resistors put them, so a whole bit position reads
as one constant value across the entire dump -- which is exactly what a stuck
bit looks like.  Distinguishing the two is what this script is for.

The chip only answers when nAD13-nAD15 match its mask-programmed code, so
addressing the wrong 8 KB window silences it completely.  Codes are the ones'
complement of the top three address bits:

    code 000 -> 160000-177777      code 100 -> 060000-077777
    code 001 -> 140000-157777      code 101 -> 040000-057777
    code 010 -> 120000-137777      code 110 -> 020000-037777
    code 011 -> 100000-117777      code 111 -> 000000-017777

Analysis is invariant under the programmer-order transform: inverting every bit
cannot change whether a bit position is constant, and permuting word addresses
cannot either.  So it does not matter whether the dump has been through
re2_convert.py.
"""

import argparse
import struct
import sys
from collections import Counter
from pathlib import Path

from re2_convert import (CODE_TO_BASE, ROM_BYTES, convert, looks_like_pdp11,
                         split_dump, words)

NWORDS = ROM_BYTES // 2


def bit_profile(w):
    """Per-bit-position count of 1s, and the list of positions that never vary."""
    ones = [0] * 16
    for x in w:
        for b in range(16):
            if (x >> b) & 1:
                ones[b] += 1
    constant = [b for b in range(16) if ones[b] in (0, len(w))]
    return ones, constant


def address_echo(w):
    """Data bits that are tracking the address rather than the chip's output.

    On a multiplexed bus the reader drives the address onto the AD lines and
    then releases them for the chip to drive data. If a line never reaches the
    chip, the trace floats and holds the last thing the reader put on it -- so
    the "data" read back is the address bit that line just carried.

    A mask ROM cannot do this: its bits are metal and have no idea what address
    preceded them. A strong correlation here is proof the line is open, not that
    the chip is bad. Orientation-independent, since inverting word addresses
    only flips the sign of the correlation.
    """
    n = len(w)
    out = []
    for b in range(16):
        best = (0.0, None)
        for ab in range(12):
            agree = sum(1 for i in range(n) if ((w[i] >> b) & 1) == ((i >> ab) & 1))
            score = abs(agree / n - 0.5) * 2          # 0 = unrelated, 1 = exact
            if score > best[0]:
                best = (score, ab)
        if best[0] > 0.75:
            out.append((b, best[1], best[0]))
    return out


def longest_run(w):
    best = run = 1
    for i in range(1, len(w)):
        run = run + 1 if w[i] == w[i - 1] else 1
        best = max(best, run)
    return best


def report_one(name, w):
    ones, constant = bit_profile(w)
    distinct = len(set(w))
    common, count = Counter(w).most_common(1)[0]

    print(f"{name}:")
    print(f"  distinct words   {distinct} of {len(w)}")
    print(f"  most common word 0x{common:04X} x{count} "
          f"({100.0 * count / len(w):.1f}%)")
    print(f"  longest run of identical words {longest_run(w)}")
    print("  bit  " + " ".join(f"{b:>4}" for b in range(16)))
    print("  1s   " + " ".join(f"{ones[b]:>4}" for b in range(16)))
    if constant:
        print(f"  ** bit positions never changing: {constant}")
        for b in constant:
            print(f"     bit {b:<2} is always {1 if ones[b] else 0}")
    else:
        print("  no bit position is constant")
    return constant


def verdict(w, constant, echoes):
    """Interpret the shape of the damage."""
    print("\nreading:")
    if echoes:
        for b, ab, score in echoes:
            print(f"  ** bit {b} tracks address bit {ab} ({score * 100:.0f}%).")
        print("  Those lines are carrying back the address the reader drove,")
        print("  which a mask ROM cannot do -- its bits are metal and know")
        print("  nothing about the preceding address. They are open circuit")
        print("  somewhere between the chip and the reader.")
        if not constant:
            return
        print()
    if len(set(w)) == 1:
        print("  Every word is identical. The chip almost certainly never drove")
        print("  the bus at all -- wrong window addressed, CS not asserted, or")
        print("  no read strobe. This is not a chip fault.")
        return
    if len(constant) == 16:
        print("  All sixteen bits are constant. Same conclusion: no reply.")
        return
    if constant:
        print(f"  {len(constant)} bit position(s) constant, the rest varying.")
        print("  Consistent with either a dead bond wire / pad on those lines,")
        print("  or those AD lines not reaching the reader. Check continuity on")
        print("  the socket pins for those bits before condemning the chip.")
        return
    print("  No bit position is stuck and words vary throughout, so the chip")
    print("  was answering. If this dump still disagrees with a reference, the")
    print("  cause is addressing or timing, not silicon.")


def compare(sus, ref, sus_name, ref_name):
    diffs = [i for i in range(NWORDS) if sus[i] != ref[i]]
    print(f"\ncompared with {ref_name}:")
    if not diffs:
        print("  identical -- the dump is good")
        return 0
    print(f"  {len(diffs)} of {NWORDS} words differ "
          f"({100.0 * len(diffs) / NWORDS:.1f}%)")
    print(f"  first differing word index {diffs[0]}, last {diffs[-1]}")

    changed = 0
    for i in diffs:
        changed |= sus[i] ^ ref[i]
    bits = [b for b in range(16) if (changed >> b) & 1]
    print(f"  bit positions involved in any difference: {bits}")

    # If every difference is one direction on a fixed set of bits, that is a
    # stuck bit rather than scattered corruption.
    stuck = []
    for b in bits:
        vals = {(sus[i] >> b) & 1 for i in diffs if ((sus[i] ^ ref[i]) >> b) & 1}
        if len(vals) == 1:
            stuck.append((b, vals.pop()))
    echo_bits = {b for b, _, _ in address_echo(sus)}
    if echo_bits & set(bits):
        print(f"  bits {sorted(echo_bits & set(bits))} are echoing the address, "
              f"so they are open lines")
    if len(stuck) == len(bits) and len(bits) > 8:
        levels = {v for _, v in stuck}
        towards = f"all to {levels.pop()}" if len(levels) == 1 else "each one way"
        print(f"  {len(bits)} bits all forced one way ({towards})")
        print("  -> that is the whole bus sitting at its idle level, i.e. the")
        print("     chip never answered. Not a stuck bit.")
    elif len(stuck) == len(bits):
        desc = ", ".join(f"bit {b} forced to {v}" for b, v in stuck)
        print(f"  every difference is one-directional: {desc}")
        print("  -> consistent with a stuck bit or an open AD line")
    elif echo_bits & set(bits):
        print("  -> the remaining differences are on those open lines; nothing")
        print("     here implicates the chip")
    else:
        print("  differences go both directions on at least one bit")
        print("  -> not a simple stuck bit; suspect addressing or timing")
    return len(diffs)


def load(path):
    body, code = split_dump(path.read_bytes())
    return body, code


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("dump", type=Path, help="the suspect dump")
    ap.add_argument("-r", "--reference", type=Path,
                    help="a known-good dump of the same chip, e.g. from the "
                         "k1801 archive")
    args = ap.parse_args()

    body, code = load(args.dump)
    w = words(body)

    if code is not None:
        base = CODE_TO_BASE[code & 7]
        print(f"trailer says code {code & 7:o}, window "
              f"{base:06o}-{base + 0o17777:06o} octal\n")

    constant = report_one(args.dump.name, w)
    echoes = address_echo(w)

    conv = looks_like_pdp11(convert(body))
    raw = looks_like_pdp11(body)
    best = conv if conv["rts_pc"] >= raw["rts_pc"] else raw
    which = "converted" if conv["rts_pc"] >= raw["rts_pc"] else "as read"
    print(f"\n  PDP-11 plausibility ({which}): "
          f"RTS PC={best['rts_pc']}, JSR PC={best['jsr_pc']}")
    if best["rts_pc"] < 5:
        print("  -> this does not look like PDP-11 code in either orientation")

    verdict(w, constant, echoes)

    rc = 0
    if args.reference:
        rbody, _ = load(args.reference)
        # Compare in whichever orientation matches better, so the caller does
        # not have to care which order either file is in.
        r = words(rbody)
        rc_direct = sum(1 for i in range(NWORDS) if w[i] != r[i])
        rconv = words(convert(rbody))
        rc_conv = sum(1 for i in range(NWORDS) if w[i] != rconv[i])
        ref = r if rc_direct <= rc_conv else rconv
        note = "" if rc_direct <= rc_conv else " (converted to match)"
        rc = compare(w, ref, args.dump.name, args.reference.name + note)
    return 1 if rc else 0


if __name__ == "__main__":
    sys.exit(main())
