#!/usr/bin/env python3
"""Run the register pattern test in a model of the PP and check what it says.

The whole value of this test is one distinction: a bit that is stuck, versus two
lines that interfere. If the model of a coupled pair and the model of a stuck bit
produce the same frame, the test is worthless -- so that is what this checks.

    python3 test/test_regtest.py
"""

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "tools"))
sys.path.insert(0, str(Path(__file__).resolve().parent))

import make_regtest                                     # noqa: E402
from test_ramtest import PlanePP as _RamPlanePP         # noqa: E402

ROM_BASE = make_regtest.ROM_BASE
BEACON = make_regtest.BEACON


class RegPP(_RamPlanePP):
    """The PP with just the one register, and two ways for it to be wrong.

    stuck_low is the ordinary fault: a bit that will not hold a one.

    coupled is the fault this test exists to distinguish -- each odd bit takes
    the value of the even bit below it, which is what the machine's fingerprint
    of "bits 1,3,5,7,9,11,13 wrong and no even bit ever" actually describes.
    """

    def __init__(self, rom, stuck_low=None, coupled=False):
        super().__init__(rom)
        self.reg = 0
        self.stuck_low = stuck_low
        self.coupled = coupled
        self.cpu_held = False

    def _seen(self):
        v = self.reg
        if self.coupled:
            for odd in range(1, 16, 2):
                v &= ~(1 << odd)
                v |= ((self.reg >> (odd - 1)) & 1) << odd
        if self.stuck_low is not None:
            v &= ~(1 << self.stuck_low) & 0xFFFF
        return v

    def read(self, addr):
        addr &= 0xFFFE
        if BEACON <= addr < BEACON + 0o100:
            self.beacons.append((addr - BEACON) // 2)
            return 0
        if addr == 0o177010:
            return self._seen()
        if addr == 0o177716:
            return 0o40 if self.cpu_held else 0
        return super(_RamPlanePP, self).read(addr)

    def write(self, addr, value):
        addr &= 0xFFFE
        if addr == 0o177010:
            self.reg = value & 0xFFFF
            return
        if addr == 0o177716:
            self.cpu_held = bool(value & 0o40)
            return
        super(_RamPlanePP, self).write(addr, value)

    def run(self, entry, limit=200_000, passes=2):
        self.r[7] = entry
        done = 0
        for _ in range(limit):
            before = self.beacons.count(make_regtest.B_DONE)
            if not self.step():
                return "halted"
            if self.beacons.count(make_regtest.B_DONE) > before:
                done += 1
                if done >= passes:
                    return f"{done} passes"
        return "ran out of steps"


def run(label, want, **kw):
    rom, _ = make_regtest.build()
    pp = RegPP(rom, **kw)
    entry = rom[make_regtest.VECTOR - ROM_BASE] | \
        (rom[make_regtest.VECTOR - ROM_BASE + 1] << 8)
    state = pp.run(entry)
    got = sorted(set(pp.beacons))
    ok = got == sorted(want) and pp.cpu_held
    print(f"  {label}")
    print(f"    {state}, beacons {got}, CPU held: {pp.cpu_held}")
    if not ok:
        print(f"    FAIL: expected {sorted(want)}, CPU held")
    return not ok


def main() -> int:
    B = make_regtest
    print("plane address register pattern test:\n")
    failures = 0

    failures += run("healthy register", [B.B_ALIVE, B.B_DONE])

    # The machine's actual fingerprint. Alternating patterns catch it; the
    # constants cannot, by construction -- which is why every constant pass in
    # the address test has come back clean while the index pass failed.
    failures += run("odd bits follow their even neighbours",
                    [B.B_ALIVE, B.B_DONE, B.B_ALT_FAIL] +
                    [B.B_BIT0 + b for b in range(1, 16, 2)],
                    coupled=True)

    # And the fault it must not look like. A stuck bit shows in both, so the
    # constant pulse lights and the diagnosis is completely different.
    failures += run("bit 5 stuck low -- an ordinary dead line",
                    [B.B_ALIVE, B.B_DONE, B.B_ALT_FAIL, B.B_CONST_FAIL,
                     B.B_BIT0 + 5],
                    stuck_low=5)

    print("\n" + ("all checks passed" if not failures else f"{failures} failure(s)"))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
