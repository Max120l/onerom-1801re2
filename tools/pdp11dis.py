#!/usr/bin/env python3
"""A small PDP-11 disassembler, for reading the UKNC's own ROM.

The counterpart to pdp11asm.py. Its job is not to produce reassemblable source
but to make the monitor readable when a question is "which way does this branch
go" -- so it prints the raw octal alongside, and prints an unknown word as
.word rather than guessing.

    ./pdp11dis.py uknc_rom.bin --base 0o100000 --from 0o160300 --count 40
"""

import argparse
import sys
from pathlib import Path

DOUBLE = {0o01: "mov", 0o02: "cmp", 0o03: "bit", 0o04: "bic", 0o05: "bis",
          0o06: "add", 0o11: "movb", 0o12: "cmpb", 0o13: "bitb", 0o14: "bicb",
          0o15: "bisb", 0o16: "sub"}
SINGLE = {0o000100: "jmp", 0o000300: "swab", 0o005000: "clr", 0o005100: "com",
          0o005200: "inc", 0o005300: "dec", 0o005400: "neg", 0o005500: "adc",
          0o005600: "sbc", 0o005700: "tst", 0o006000: "ror", 0o006100: "rol",
          0o006200: "asr", 0o006300: "asl", 0o006400: "mark", 0o006500: "mfpi",
          0o006600: "mtpi", 0o006700: "sxt", 0o105000: "clrb", 0o105100: "comb",
          0o105200: "incb", 0o105300: "decb", 0o105400: "negb", 0o105700: "tstb",
          0o106000: "rorb", 0o106100: "rolb", 0o106200: "asrb", 0o106300: "aslb"}
BRANCH = {0o000400: "br", 0o001000: "bne", 0o001400: "beq", 0o002000: "bge",
          0o002400: "blt", 0o003000: "bgt", 0o003400: "ble", 0o100000: "bpl",
          0o100400: "bmi", 0o101000: "bhi", 0o101400: "blos", 0o102000: "bvc",
          0o102400: "bvs", 0o103000: "bcc", 0o103400: "bcs"}
REG_OPS = {0o004000: "jsr", 0o070000: "mul", 0o071000: "div", 0o072000: "ash",
           0o073000: "ashc", 0o074000: "xor", 0o077000: "sob"}
SIMPLE = {0o000000: "halt", 0o000001: "wait", 0o000002: "rti", 0o000003: "bpt",
          0o000004: "iot", 0o000005: "reset", 0o000006: "rtt", 0o000240: "nop"}
REGNAME = ["r0", "r1", "r2", "r3", "r4", "r5", "sp", "pc"]


class Stream:
    def __init__(self, image, base, addr):
        self.image, self.base, self.addr = image, base, addr
        self.words = []

    def next(self):
        off = self.addr - self.base
        if off < 0 or off + 2 > len(self.image):
            raise IndexError(f"{self.addr:06o} outside the image")
        w = self.image[off] | (self.image[off + 1] << 8)
        self.addr += 2
        self.words.append(w)
        return w


def operand(s, field):
    """Decode one 6-bit operand field, consuming extra words as needed."""
    mode, reg = (field >> 3) & 7, field & 7
    r = REGNAME[reg]
    if reg == 7 and mode in (2, 3, 6, 7):          # PC-relative forms
        x = s.next()
        if mode == 2:
            return f"#{x:o}"
        if mode == 3:
            return f"@#{x:o}"
        target = (s.addr + x) & 0xFFFF
        return f"{target:06o}" if mode == 6 else f"@{target:06o}"
    if mode == 0:
        return r
    if mode == 1:
        return f"({r})"
    if mode == 2:
        return f"({r})+"
    if mode == 3:
        return f"@({r})+"
    if mode == 4:
        return f"-({r})"
    if mode == 5:
        return f"@-({r})"
    x = s.next()
    return (f"{x:o}({r})" if mode == 6 else f"@{x:o}({r})")


def decode(s):
    start = s.addr
    op = s.next()
    if op in SIMPLE:
        text = SIMPLE[op]
    elif (op >> 12) & 0o17 in DOUBLE and (op >> 12) != 0o07:
        name = DOUBLE[(op >> 12) & 0o17]
        src = operand(s, (op >> 6) & 0o77)
        dst = operand(s, op & 0o77)
        text = f"{name} {src}, {dst}"
    elif op & 0o177400 in BRANCH:
        off = op & 0xFF
        if off > 127:
            off -= 256
        text = f"{BRANCH[op & 0o177400]} {(start + 2 + 2 * off) & 0xFFFF:06o}"
    elif op & 0o177000 in REG_OPS:
        name = REG_OPS[op & 0o177000]
        reg = REGNAME[(op >> 6) & 7]
        if name == "sob":
            text = f"sob {reg}, {(start + 2 - 2 * (op & 0o77)) & 0xFFFF:06o}"
        else:
            text = f"{name} {reg}, {operand(s, op & 0o77)}"
    elif op & 0o177700 in SINGLE:
        text = f"{SINGLE[op & 0o177700]} {operand(s, op & 0o77)}"
    elif op & 0o177770 == 0o000200:
        text = f"rts {REGNAME[op & 7]}"
    elif 0o104000 <= op < 0o104400:
        text = f"emt {op & 0xFF:o}"
    elif 0o104400 <= op < 0o105000:
        text = f"trap {op & 0xFF:o}"
    else:
        text = f".word {op:06o}"
    return start, s.words, text


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("image", type=Path)
    ap.add_argument("--base", type=lambda v: int(v, 0), default=0o100000,
                    help="address the first byte of the image lives at")
    ap.add_argument("--from", dest="start", type=lambda v: int(v, 0), required=True)
    ap.add_argument("--count", type=int, default=32, help="instructions")
    args = ap.parse_args()

    image = args.image.read_bytes()
    addr = args.start
    for _ in range(args.count):
        s = Stream(image, args.base, addr)
        try:
            at, words, text = decode(s)
        except IndexError as e:
            print(e)
            return 1
        octal = " ".join(f"{w:06o}" for w in words)
        print(f"{at:06o}  {octal:<21} {text}")
        addr = s.addr
    return 0


if __name__ == "__main__":
    sys.exit(main())
