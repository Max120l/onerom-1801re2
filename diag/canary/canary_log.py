#!/usr/bin/env python3
"""Decode CANARY.LOG written by the UKNC RAM retention canary.

Usage:  canary_log.py CANARY.LOG            (file extracted from the disk image)
        canary_log.py image.dsk             (finds CANARY.LOG in the RT-11 directory itself)

Log format (RT-11 blocks of 256 words, little-endian):
  block 0  header: "CANA", version, pass count (32b), error count (32b),
           ticks now (32b, hi word first), ticks at fill (32b), next free
           event block, test region [lolim, hilim)
  block 1+ 32 events of 8 words: pass_lo, address, expected, actual,
           tick_hi, tick_lo, pass_hi, marker 125125 (octal) when valid
UKNC clock: 50 ticks per second.
"""
import struct, sys
from collections import Counter

HZ = 50
MARK = 0o125125


RAD50 = " ABCDEFGHIJKLMNOPQRSTUVWXYZ$.%0123456789"


def rad50(w):
    return RAD50[w // 1600] + RAD50[w // 40 % 40] + RAD50[w % 40]


def find_file(img, wanted="CANARY.LOG"):
    """Walk the RT-11 directory of a .dsk image; return (start_block, length) of wanted."""
    def w(a):
        return img[a] | img[a + 1] << 8
    seg = 1
    seen = set()
    while seg and seg not in seen:
        seen.add(seg)
        base = (6 + (seg - 1) * 2) * 512          # directory segments start at block 6, 2 blocks each
        nextseg, extra, start = w(base + 2), w(base + 6), w(base + 8)
        p = base + 10
        while p + 14 <= base + 1024:
            status = w(p)
            if status & 0o4000:                    # end of segment
                break
            length = w(p + 8)
            if status & 0o2000:                    # permanent file
                name = (rad50(w(p + 2)) + rad50(w(p + 4))).strip() + "." + rad50(w(p + 6)).strip()
                if name == wanted:
                    return start, length
            start += length
            p += 14 + extra
        seg = nextseg
    return None


def load(path):
    if path.lower().endswith(".dsk"):
        img = open(path, "rb").read()
        hit = find_file(img)
        if not hit:
            sys.exit("no CANARY.LOG on that disk image")
        start, length = hit
        return img[start * 512:(start + length) * 512]
    return open(path, "rb").read()


def words(b):
    return struct.unpack("<%dH" % (len(b) // 2), b[: len(b) // 2 * 2])


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    d = load(sys.argv[1])
    h = words(d[:512])
    if h[0] != 0o40503 or h[1] != 0o40516:
        sys.exit("not a canary log (bad magic)")
    passes = h[3] | h[4] << 16
    errors = h[5] | h[6] << 16
    now = h[7] << 16 | h[8]
    t0 = h[9] << 16 | h[10]
    nextblk, lolim, hilim = h[11], h[12], h[13]
    up = (now - t0) & 0xFFFFFFFF
    stop = "stopped cleanly by key" if h[14] == 1 else "NO clean stop recorded: the machine froze, hung or was powered off while running"
    print(f"canary log v{h[2]}: region {lolim:06o}-{hilim:06o} octal "
          f"({(hilim - lolim) // 2} words), {passes} passes, {errors} decay events, "
          f"last heartbeat {up / HZ:.1f} s after fill "
          f"(clock {now // HZ // 3600:02d}:{now // HZ % 3600 // 60:02d}:{now // HZ % 60:02d}), "
          f"{passes / max(up / HZ, 1):.2f} passes/s")
    print(f"end of run: {stop}")

    events = []
    nblocks = len(d) // 512
    for blk in range(1, min(nextblk, nblocks)):
        w = words(d[blk * 512: blk * 512 + 512])
        for i in range(32):
            e = w[i * 8: i * 8 + 8]
            if e[7] != MARK:
                continue
            events.append(dict(pass_no=e[0] | e[6] << 16, addr=e[1], exp=e[2], act=e[3],
                               t=(e[4] << 16 | e[5]) - t0))
    buffered = errors - len(events)
    if not events:
        print("no decay events recorded" + (f" ({buffered} were still buffered at the last heartbeat)" if buffered else ""))
        return

    print(f"\n{len(events)} events on disk" + (f" (+{buffered} counted but not yet flushed)" if buffered > 0 else "") + ":")
    print(f"  {'time':>8}  {'pass':>6}  {'addr':>6}  {'expect':>6}  {'actual':>6}  bits")
    bits = Counter()
    direction = Counter()
    for e in events:
        diff = e["exp"] ^ e["act"]
        flipped = [b for b in range(16) if diff >> b & 1]
        for b in flipped:
            bits[b] += 1
            direction["1->0" if e["exp"] >> b & 1 else "0->1"] += 1
        print(f"  {e['t'] / HZ:8.1f}  {e['pass_no']:6d}  {e['addr']:06o}  {e['exp']:06o}  {e['act']:06o}  "
              + ",".join(str(b) for b in flipped))

    print("\nflipped-bit histogram (bit: count) — one dominant bit points at one DRAM chip:")
    for b in range(15, -1, -1):
        if bits[b]:
            print(f"  bit {b:2d}: {bits[b]:5d}  {'#' * min(60, bits[b])}")
    print("direction:", ", ".join(f"{k} x{v}" for k, v in direction.most_common()),
          "  (1->0 dominant = charge leaking away, the classic retention signature)")
    first = min(events, key=lambda e: e["t"])
    print(f"first decay at {first['t'] / HZ:.1f} s after fill, address {first['addr']:06o}")
    lo = min(e["addr"] for e in events)
    hi = max(e["addr"] for e in events)
    print(f"address spread {lo:06o}-{hi:06o}")


if __name__ == "__main__":
    main()
