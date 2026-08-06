#!/usr/bin/env python3
"""Run the address-bus test in a model of the PP and check what it beacons.

The point of this test is a distinction -- addressing versus data -- so the thing
to demonstrate is that it actually distinguishes. A model with a broken address
line and a model with a broken data line must produce different frames, and
neither may produce the frame of a healthy machine.

    python3 test/test_addrtest.py
"""

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "tools"))
sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "tools" / "diag"))
sys.path.insert(0, str(Path(__file__).resolve().parent))

import make_addrtest                                    # noqa: E402
from test_testrom import PP                             # noqa: E402
from test_ramtest import PlanePP as _RamPlanePP         # noqa: E402

ROM_BASE = make_addrtest.ROM_BASE
BEACON = make_addrtest.BEACON

# Small enough to simulate in seconds. 512 words exercises address bits 0..8,
# which is all the model needs -- a broken line does not care how many of them
# there are.
PLANE_WORDS = 0o1000


class PlanePP(_RamPlanePP):
    """The PP with the three plane registers, and faults of both kinds.

    Derived from the soak's model rather than borrowing its step() as a plain
    function: that method uses a zero-argument super(), which is bound to the
    class it was written in and raises the moment it is attached to an unrelated
    one. Inheriting keeps one copy of the instruction set with the MRO intact.

    data_bit is a plane data line stuck low: the classic dead 4164.

    addr_bit is a plane *address* line stuck low, which is the fault this test
    exists to find. Both halves of an address pair then select the same cell, so
    the second write of the fill overwrites the first and the check finds the
    wrong index looking back at it.
    """

    def __init__(self, rom, data_bit=None, data_plane=1, addr_bit=None,
                 reg_bit=None, wedge_on_ones=False):
        super().__init__(rom)
        self.plane = [bytearray(PLANE_WORDS) for _ in range(3)]
        self.paddr = 0
        self.data_bit = data_bit
        self.data_plane = data_plane
        self.addr_bit = addr_bit
        # A bit the plane address register at 177010 will not hold. Unlike
        # addr_bit, this corrupts the address before anything sees it, so the
        # readback is wrong *and* the wrong cell is selected -- which is the
        # whole point: the two faults are indistinguishable downstream, and only
        # reading the register apart from the array separates them.
        self.reg_bit = reg_bit
        # The failure mode the phase markers exist for: the PP stops executing
        # partway through a pass -- hung on a cycle that never gets a reply, or
        # trapped through a vector holding garbage -- and never reaches the
        # report. Every verdict beacon stays dark, which is indistinguishable
        # from a clean machine unless something says how far it got.
        self.wedge_on_ones = wedge_on_ones
        self.wedged = False
        self.cpu_held = False

    def _cell(self):
        a = self.paddr
        if self.addr_bit is not None:
            a &= ~(1 << self.addr_bit)
        return a % PLANE_WORDS

    def _get(self, p):
        v = self.plane[p][self._cell()]
        if self.data_bit is not None and p == self.data_plane:
            v &= ~(1 << self.data_bit) & 0xFF
        return v

    def read(self, addr):
        addr &= 0xFFFE
        if BEACON <= addr < BEACON + 0o100:
            self.beacons.append((addr - BEACON) // 2)
            return 0
        if addr == 0o177010:
            return self.paddr
        if addr == 0o177012:
            return self._get(0)
        if addr == 0o177014:
            return self._get(1) | (self._get(2) << 8)
        if addr == 0o177716:
            return 0o40 if self.cpu_held else 0
        return super().read(addr)

    def write(self, addr, value):
        addr &= 0xFFFE
        if addr == 0o177010:
            if self.reg_bit is not None:
                value &= ~(1 << self.reg_bit)
            self.paddr = value % PLANE_WORDS
            return
        if addr == 0o177012:
            self.plane[0][self._cell()] = value & 0xFF
            return
        if addr == 0o177014:
            if self.wedge_on_ones and value == 0xFFFF:
                self.wedged = True
            self.plane[1][self._cell()] = value & 0xFF
            self.plane[2][self._cell()] = (value >> 8) & 0xFF
            return
        if addr == 0o177716:
            self.cpu_held = bool(value & 0o40)
            return
        super().write(addr, value)

    def run(self, entry, limit=4_000_000, passes=2):
        self.r[7] = entry
        done = 0
        for _ in range(limit):
            if self.wedged:
                return "wedged"
            before = self.beacons.count(make_addrtest.B_DONE)
            if not self.step():
                return "halted"
            if self.beacons.count(make_addrtest.B_DONE) > before:
                done += 1
                if done >= passes:
                    return f"{done} passes"
        return "ran out of steps"


PHASES = [make_addrtest.B_PH_REG, make_addrtest.B_PH_ZFILL,
          make_addrtest.B_PH_ZCHK, make_addrtest.B_PH_OFILL,
          make_addrtest.B_PH_OCHK, make_addrtest.B_PH_AFILL,
          make_addrtest.B_PH_ACHK]


def run(label, want, **kw):
    rom, _ = make_addrtest.build(plane_words=PLANE_WORDS)
    pp = PlanePP(rom, **kw)
    entry = rom[make_addrtest.VECTOR - ROM_BASE] | \
        (rom[make_addrtest.VECTOR - ROM_BASE + 1] << 8)
    state = pp.run(entry)
    got = sorted(set(pp.beacons))
    # Every complete pass lights all seven phase markers, so they belong in every
    # expectation rather than being repeated in each case. What makes them worth
    # having is a pass that does NOT complete -- see run_phases below.
    want = list(want) + PHASES
    ok = got == sorted(want) and pp.cpu_held
    print(f"  {label}")
    print(f"    {state}, beacons {got}, CPU held: {pp.cpu_held}")
    if not ok:
        print(f"    FAIL: expected {sorted(want)}, CPU held")
    return not ok


def run_stop(label, want_passes, **kw):
    """--stop-on-fail must stop, and only when something actually failed.

    The frame it leaves behind is only worth reading if the loop really halts at
    the first bad pass: one that kept going would go on accumulating, which is
    the saturation this mode exists to avoid. Counting DONE beacons measures
    exactly that -- one for a machine that stopped, more for one that did not.
    """
    rom, _ = make_addrtest.build(plane_words=PLANE_WORDS, stop_on_fail=True)
    pp = PlanePP(rom, **kw)
    entry = rom[make_addrtest.VECTOR - ROM_BASE] | \
        (rom[make_addrtest.VECTOR - ROM_BASE + 1] << 8)
    # Ask for three, so "stopped after one" and "ran to three" are both visible.
    pp.run(entry, limit=4_000_000, passes=3)
    got = pp.beacons.count(make_addrtest.B_DONE)
    ok = got == want_passes
    print(f"  {label}")
    print(f"    completed passes: {got}")
    if not ok:
        print(f"    FAIL: expected {want_passes}")
    return not ok


def main() -> int:
    B = make_addrtest
    print("address-bus test:\n")
    failures = 0

    failures += run("healthy machine",
                    [B.B_ALIVE, B.B_DONE])

    # The fault this exists for. Constant passes see nothing -- every cell holds
    # the same value, so selecting the wrong one is invisible to them -- and the
    # address pass names the line.
    failures += run("plane address line 3 stuck low",
                    [B.B_ALIVE, B.B_DONE, B.B_ADDR_FAIL, B.B_A0 + 3],
                    addr_bit=3)

    # And the fault it must NOT confuse with that one. A dead data bit shows up
    # in the address pass too, in the same bit position; the mask has to strip it
    # or this test would point at the address bus for every dead 4164.
    failures += run("plane 1 data bit 3 stuck low -- must not read as address",
                    [B.B_ALIVE, B.B_DONE, B.B_DATA_FAIL],
                    data_bit=3)

    # Plane 2's data is the high byte, so its bits land at 8..15 -- worth its own
    # case, since an off-by-a-byte in the report would still pass the case above.
    failures += run("plane 2 data bit 6 stuck low",
                    [B.B_ALIVE, B.B_DONE, B.B_DATA_FAIL],
                    data_bit=6, data_plane=2)

    # Both at once: the address line must still be named, and not swallowed by
    # the data mask, because they are in different bit positions.
    failures += run("address line 5 and data bit 1 together",
                    [B.B_ALIVE, B.B_DONE, B.B_DATA_FAIL, B.B_ADDR_FAIL,
                     B.B_A0 + 5],
                    addr_bit=5, data_bit=1)

    # The gap this pass exists to close. A register that drops a bit and an
    # address line that drops the same bit look identical from the array's side;
    # only reading 177010 back tells them apart, so the register case must light
    # a pulse the address case does not.
    failures += run("plane address register drops bit 2",
                    [B.B_ALIVE, B.B_DONE, B.B_ADDR_FAIL, B.B_REG_FAIL,
                     B.B_A0 + 2],
                    reg_bit=2)

    # The whole reason the phase markers exist. A PP that stops partway through
    # leaves every verdict beacon dark, so the frame of a wedged machine and the
    # frame of a healthy one are the same -- except for how far the phases got.
    print("\nphase markers -- a pass that never finishes:\n")
    rom, _ = B.build(plane_words=PLANE_WORDS)
    pp = PlanePP(rom, wedge_on_ones=True)
    entry = rom[B.VECTOR - ROM_BASE] | (rom[B.VECTOR - ROM_BASE + 1] << 8)
    state = pp.run(entry)
    got = sorted(set(pp.beacons))
    want = sorted([B.B_ALIVE, B.B_PH_REG, B.B_PH_ZFILL, B.B_PH_ZCHK,
                   B.B_PH_OFILL])
    print(f"  PP wedges during the all-ones fill")
    print(f"    {state}, beacons {got}")
    if got != want:
        print(f"    FAIL: expected {want}")
        failures += 1
    else:
        print(f"    last phase reached is {B.B_PH_OFILL + 1}, and DONE is dark")

    print("\nstop-on-fail -- freeze at the first failing pass:\n")
    failures += run_stop("healthy machine keeps looping", want_passes=3)
    failures += run_stop("a fault stops it dead after one pass", want_passes=1,
                         addr_bit=3)

    print("\n" + ("all checks passed" if not failures else f"{failures} failure(s)"))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
