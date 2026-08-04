#!/usr/bin/env python3
"""Run the plane RAM test in a model of the PP and check what it beacons.

A diagnostic that has not been executed is a guess, and this one is going into a
machine to answer a question about that machine's memory -- so it had better not
be answering a question about its own bugs. This runs the real assembled image
against a model with 32 KB of PP RAM, three planes behind the 177010/177012/
177014 registers, and the ability to break one plane at one bit.

    python3 test/test_ramtest.py
"""

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "tools"))
sys.path.insert(0, str(Path(__file__).resolve().parent))

import make_ramtest                                      # noqa: E402
from test_testrom import PP                              # noqa: E402

ROM_BASE = make_ramtest.ROM_BASE
BEACON = make_ramtest.BEACON

# The same program over a small memory: the loop bounds are the only difference
# between this and the image that gets flashed, and a stuck bit does not care
# how many addresses it is asked about.
RAM_TOP = 0o2000
PLANE_WORDS = 0o1000


class PlanePP(PP):
    """The PP, plus the three plane registers and the memory behind them.

    broken is (plane, bit): that bit reads back low everywhere in that plane,
    which is what a dead RAM chip in one bank looks like from here.
    """

    def __init__(self, rom, broken=None):
        super().__init__(rom)
        self.plane = [bytearray(PLANE_WORDS) for _ in range(3)]
        self.paddr = 0
        self.broken = broken
        self.cpu_held = False

    def _plane_byte(self, p, addr):
        v = self.plane[p][addr]
        if self.broken and self.broken[0] == p:
            v &= ~(1 << self.broken[1]) & 0xFF
        return v

    def read(self, addr):
        addr &= 0xFFFE
        if addr == 0o177010:
            return self.paddr
        if addr == 0o177012:
            return self._plane_byte(0, self.paddr)
        if addr == 0o177014:
            return (self._plane_byte(1, self.paddr)
                    | (self._plane_byte(2, self.paddr) << 8))
        if addr == 0o177716:
            return 0o40 if self.cpu_held else 0
        return super().read(addr)

    def write(self, addr, value):
        addr &= 0xFFFE
        if addr == 0o177010:
            self.paddr = value % PLANE_WORDS
            return
        if addr == 0o177012:
            self.plane[0][self.paddr] = value & 0xFF
            return
        if addr == 0o177014:
            self.plane[1][self.paddr] = value & 0xFF
            self.plane[2][self.paddr] = (value >> 8) & 0xFF
            return
        if addr == 0o177716:
            self.cpu_held = bool(value & 0o40)
            return
        super().write(addr, value)

    # The test uses a few instructions the base model does not.
    def step(self):
        op = self.read(self.r[7])
        if op & 0o177700 == 0o005200:                   # INC
            self.r[7] = (self.r[7] + 2) & 0xFFFF
            dm, dr = (op >> 3) & 7, op & 7
            da = self.addr_of(dm, dr)
            v = ((self.r[dr] if da is None else self.read(da)) + 1) & 0xFFFF
            if da is None:
                self.r[dr] = v
            else:
                self.write(da, v)
            self.setnz(v)
            return True
        if op & 0o177000 == 0o074000:                   # XOR: dst ^= reg
            self.r[7] = (self.r[7] + 2) & 0xFFFF
            reg = (op >> 6) & 7
            dm, dr = (op >> 3) & 7, op & 7
            da = self.addr_of(dm, dr)
            dst = self.r[dr] if da is None else self.read(da)
            v = (dst ^ self.r[reg]) & 0xFFFF
            if da is None:
                self.r[dr] = v
            else:
                self.write(da, v)
            self.setnz(v)
            self.v = 0
            return True
        if (op >> 12) & 0o17 in (0o4, 0o5):             # BIC, BIS
            self.r[7] = (self.r[7] + 2) & 0xFFFF
            sm, sr = (op >> 9) & 7, (op >> 6) & 7
            dm, dr = (op >> 3) & 7, op & 7
            src, _ = self.get(sm, sr)
            da = self.addr_of(dm, dr)
            dst = self.r[dr] if da is None else self.read(da)
            v = (dst & ~src if (op >> 12) == 0o4 else dst | src) & 0xFFFF
            if da is None:
                self.r[dr] = v
            else:
                self.write(da, v)
            self.setnz(v)
            self.v = 0
            return True
        return super().step()

    def run(self, entry, psw_unused, limit=4_000_000):
        # The base loop parks on make_testrom's DONE beacon, which is a
        # different number here.
        self.r[7] = entry
        for _ in range(limit):
            if not self.step():
                return "halted"
            if (len(self.beacons) > 8
                    and self.beacons[-4:] == [make_ramtest.B_DONE] * 4):
                return "parked"
        return "ran out of steps"


def beacon_set(pp):
    return sorted(set(pp.beacons))


def status(pp):
    """The verdict the test leaves in PP RAM, for a debugger or ukncbtl."""
    a = make_ramtest.RESULT
    return pp.ram[a] | (pp.ram[a + 1] << 8)


def run(label, broken, want, want_status):
    rom, _ = make_ramtest.build(ram_top=RAM_TOP, plane_words=PLANE_WORDS)
    pp = PlanePP(rom, broken=broken)
    entry = rom[make_ramtest.VECTOR - ROM_BASE] | \
        (rom[make_ramtest.VECTOR - ROM_BASE + 1] << 8)
    state = pp.run(entry, None, limit=4_000_000)
    got, st = beacon_set(pp), status(pp)
    ok = got == want and pp.cpu_held and st == want_status
    print(f"  {label}")
    print(f"    {state}, beacons {got}, status {st:06o}, CPU held: {pp.cpu_held}")
    if not ok:
        print(f"    FAIL: expected {want}, status {want_status:06o}, CPU held")
    return not ok


def main() -> int:
    B = make_ramtest
    failures = 0
    print("plane RAM test:\n")
    failures += run("healthy machine", None,
                    [B.B_ALIVE, B.B_PP_RAM_PASS, B.B_PLANE_PASS, B.B_DONE],
                    B.RES_PP_RAM_OK | B.RES_DONE)
    failures += run("plane 1 bit 3 stuck low", (1, 3),
                    [B.B_ALIVE, B.B_PP_RAM_PASS, B.B_PLANE1_FAIL, B.B_DONE],
                    B.RES_PP_RAM_OK | B.RES_PLANE1_BAD | B.RES_DONE)
    failures += run("plane 2 bit 6 stuck low", (2, 6),
                    [B.B_ALIVE, B.B_PP_RAM_PASS, B.B_PLANE2_FAIL, B.B_DONE],
                    B.RES_PP_RAM_OK | B.RES_PLANE2_BAD | B.RES_DONE)
    print("\n" + ("all checks passed" if not failures else f"{failures} failure(s)"))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
