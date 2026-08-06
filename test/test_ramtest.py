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
sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "tools" / "diag"))
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

    broken is (plane, bit); high says which way it is stuck. Both directions
    exist -- the fault this was written for turned out to be a line stuck HIGH,
    scoped on the bench -- and a test that only ever saw stuck-low would be
    assuming the detection generalises rather than showing that it does.
    """

    def __init__(self, rom, broken=None, leaky=False, high=False, flaky=False,
                 pp_bit=None, pp_all=False):
        super().__init__(rom)
        self.plane = [bytearray(PLANE_WORDS) for _ in range(3)]
        self.paddr = 0
        self.broken = broken
        # A leaky bit holds its value only until something else is written --
        # right on an immediate reread, wrong by the time a second pass comes
        # round. A dead one is wrong straight away. The test has to tell them
        # apart, so the model has to be able to be either.
        self.leaky = leaky
        self.high = high
        # Present only during the second pass: a fault that comes and goes, the
        # shape of anything thermal. The soak has to report it without the clean
        # passes hiding it.
        self.flaky = flaky
        # A bit of the PP's own RAM -- plane 0, the bank on DG0..DG7, the one
        # the video tag list lives in. The PP reads words from it, and a failing
        # data line shows in both bytes of every word, so model it that way.
        self.pp_bit = pp_bit
        # The whole of PP RAM reading back as zero, which is what losing refresh
        # or the supply looks like from here -- as opposed to one weak cell.
        self.pp_all = pp_all
        self.last_written = None
        self.cpu_held = False

    def _plane_byte(self, p, addr):
        v = self.plane[p][addr]
        if self.broken and self.broken[0] == p:
            if self.flaky and self.beacons.count(make_ramtest.B_DONE) != 1:
                return v
            if not self.leaky or addr != self.last_written:
                if self.high:
                    v |= 1 << self.broken[1]
                else:
                    v &= ~(1 << self.broken[1]) & 0xFF
        return v

    def read(self, addr):
        addr &= 0xFFFE
        # This test's beacons live in ROM, not where test_testrom's base class
        # looks for them, so record them here before anything else claims the
        # address as an ordinary ROM word.
        if BEACON <= addr < BEACON + 0o100:
            self.beacons.append((addr - BEACON) // 2)
            return 0
        if addr == 0o177010:
            return self.paddr
        if addr == 0o177012:
            return self._plane_byte(0, self.paddr)
        if addr == 0o177014:
            return (self._plane_byte(1, self.paddr)
                    | (self._plane_byte(2, self.paddr) << 8))
        if addr == 0o177716:
            return 0o40 if self.cpu_held else 0
        v = super().read(addr)
        if self.pp_all and addr < 0o100000:
            return 0
        if self.pp_bit is not None and addr < 0o100000:
            v &= ~((1 << self.pp_bit) | (1 << (self.pp_bit + 8))) & 0xFFFF
        return v

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
            self.last_written = self.paddr
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
        if op & 0o177700 == 0o000100:                   # JMP
            self.r[7] = (self.r[7] + 2) & 0xFFFF
            dm, dr = (op >> 3) & 7, op & 7
            da = self.addr_of(dm, dr)
            if da is None:
                raise RuntimeError("jmp to a register is illegal")
            self.r[7] = da
            return True
        if op & 0o177700 == 0o000300:                   # SWAB
            self.r[7] = (self.r[7] + 2) & 0xFFFF
            dm, dr = (op >> 3) & 7, op & 7
            da = self.addr_of(dm, dr)
            v = self.r[dr] if da is None else self.read(da)
            v = ((v >> 8) | (v << 8)) & 0xFFFF
            if da is None:
                self.r[dr] = v
            else:
                self.write(da, v)
            self.setnz(v & 0xFF)
            return True
        if op & 0o177700 == 0o005300:                   # DEC
            self.r[7] = (self.r[7] + 2) & 0xFFFF
            dm, dr = (op >> 3) & 7, op & 7
            da = self.addr_of(dm, dr)
            v = ((self.r[dr] if da is None else self.read(da)) - 1) & 0xFFFF
            if da is None:
                self.r[dr] = v
            else:
                self.write(da, v)
            self.setnz(v)
            return True
        if op & 0o177700 == 0o006200:                   # ASR
            self.r[7] = (self.r[7] + 2) & 0xFFFF
            dm, dr = (op >> 3) & 7, op & 7
            da = self.addr_of(dm, dr)
            v = self.r[dr] if da is None else self.read(da)
            self.c = v & 1
            v = ((v >> 1) | (v & 0x8000)) & 0xFFFF
            if da is None:
                self.r[dr] = v
            else:
                self.write(da, v)
            self.setnz(v)
            return True
        if (op >> 12) & 0o17 == 0o3:                    # BIT: src & dst, no store
            self.r[7] = (self.r[7] + 2) & 0xFFFF
            sm, sr = (op >> 9) & 7, (op >> 6) & 7
            dm, dr = (op >> 3) & 7, op & 7
            src, _ = self.get(sm, sr)
            dst, _ = self.get(dm, dr)
            self.setnz(src & dst)
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

    def run(self, entry, psw_unused, limit=4_000_000, passes=3):
        # The test soaks rather than parking, so stop after a few complete
        # passes instead of waiting for it to settle. Running more than one is
        # the point: it is what proves the loop actually goes round and keeps
        # reporting, rather than reporting once and wedging.
        self.r[7] = entry
        done = 0
        for _ in range(limit):
            before = self.beacons.count(make_ramtest.B_DONE)
            if not self.step():
                return "halted"
            if self.beacons.count(make_ramtest.B_DONE) > before:
                done += 1
                if done >= passes:
                    return f"{done} passes"
        return "ran out of steps"


def beacon_set(pp):
    return sorted(set(pp.beacons))


def status(pp):
    """The verdict the test leaves in PP RAM, for a debugger or ukncbtl."""
    a = make_ramtest.RESULT
    return pp.ram[a] | (pp.ram[a + 1] << 8)


def run(label, broken, want, want_status, leaky=False, high=False, flaky=False,
        pp_bit=None, pp_all=False):
    rom, _ = make_ramtest.build(ram_top=RAM_TOP, plane_words=PLANE_WORDS)
    pp = PlanePP(rom, broken=broken, leaky=leaky, high=high, flaky=flaky,
                 pp_bit=pp_bit, pp_all=pp_all)
    entry = rom[make_ramtest.VECTOR - ROM_BASE] | \
        (rom[make_ramtest.VECTOR - ROM_BASE + 1] << 8)
    state = pp.run(entry, None, limit=4_000_000)
    got, st = beacon_set(pp), status(pp)
    want = sorted(want)          # beacon_set is sorted; the caller need not be
    ok = got == want and pp.cpu_held and st == want_status
    print(f"  {label}")
    print(f"    {state}, beacons {got}, status {st:06o}, CPU held: {pp.cpu_held}")
    if not ok:
        print(f"    FAIL: expected {want}, status {want_status:06o}, CPU held")
    return not ok


def passes_of(pp):
    """Split the beacon stream into one list per pass, on the DONE beacon."""
    out, cur = [], []
    for b in pp.beacons:
        cur.append(b)
        if b == make_ramtest.B_DONE:
            out.append(sorted(set(cur)))
            cur = []
    return out


def run_live(label, want_passes, want_status, **kw):
    """The --live image, checked pass by pass rather than in aggregate.

    Latching and live are the same program with the clears in a different place,
    and the whole value of the live one is a property the latching one is built
    not to have: a verdict that can go back to clean. Freeze spray depends on
    exactly that, so it is worth showing rather than assuming -- an accumulator
    left outside the loop by mistake would still pass every check above.
    """
    rom, _ = make_ramtest.build(ram_top=RAM_TOP, plane_words=PLANE_WORDS,
                                live=True)
    pp = PlanePP(rom, **kw)
    entry = rom[make_ramtest.VECTOR - ROM_BASE] | \
        (rom[make_ramtest.VECTOR - ROM_BASE + 1] << 8)
    state = pp.run(entry, None, limit=4_000_000)
    got = passes_of(pp)
    st = status(pp)
    want = [sorted(p) for p in want_passes]
    ok = got == want and pp.cpu_held and st == want_status
    print(f"  {label}")
    print(f"    {state}, status {st:06o}")
    for i, p in enumerate(got):
        print(f"      pass {i + 1}: {p}")
    if not ok:
        print(f"    FAIL: expected {want}, status {want_status:06o}")
    return not ok


def check_shipped_defaults():
    """The constants the flashed image actually uses.

    The runs below deliberately use small bounds so the simulation finishes in
    seconds -- which means they never touch the defaults, and a bad default
    ships silently. That is exactly what happened: the RAM walk's upper bound
    defaulted to the beacon address, and when the beacons moved into ROM the
    walk followed them off the end of PP RAM into a region where a write gets no
    reply. On hardware the machine wedged after the first beacon.
    """
    B = make_ramtest
    bad = []
    if B.PP_RAM_TOP > 0o100000:
        bad.append(f"PP_RAM_TOP {B.PP_RAM_TOP:06o} is past the end of PP RAM")
    if B.BEACON < 0o100000:
        bad.append(f"BEACON {B.BEACON:06o} is not in ROM, so we may not see it")
    if B.BEACON + 2 * B.BEACON_COUNT > 0o177000:
        bad.append(f"beacons from {B.BEACON:06o} reach the I/O page")
    if not (B.ENTRY < B.BEACON):
        bad.append("the program overlaps its own beacons")
    if B.PLANE_WORDS > 0o100000:
        bad.append(f"PLANE_WORDS {B.PLANE_WORDS:06o} exceeds one plane")

    print("shipped defaults:")
    print(f"  PP_RAM_TOP {B.PP_RAM_TOP:06o}  BEACON {B.BEACON:06o}  "
          f"PLANE_WORDS {B.PLANE_WORDS:06o}")
    for b in bad:
        print(f"      FAIL: {b}")
    print("      ok" if not bad else "")
    return bool(bad)


def main() -> int:
    B = make_ramtest
    failures = check_shipped_defaults()
    print("\nplane RAM test:\n")
    failures += run("healthy machine", None,
                    [B.B_ALIVE, B.B_PP_RAM_PASS, B.B_PLANE_PASS, B.B_DONE],
                    B.RES_PP_RAM_OK | B.RES_DONE)
    failures += run("plane 1 bit 3 stuck low", (1, 3),
                    [B.B_ALIVE, B.B_PP_RAM_PASS, B.B_PLANE1_FAIL, B.B_DONE,
                     B.B_STUCK, B.B_BIT0 + 3],
                    B.RES_PP_RAM_OK | B.RES_PLANE1_BAD | B.RES_DONE | B.RES_STUCK)
    failures += run("plane 1 bit 7 stuck HIGH -- the fault on the bench", (1, 7),
                    [B.B_ALIVE, B.B_PP_RAM_PASS, B.B_PLANE1_FAIL, B.B_DONE,
                     B.B_STUCK, B.B_BIT0 + 7],
                    B.RES_PP_RAM_OK | B.RES_PLANE1_BAD | B.RES_DONE | B.RES_STUCK,
                    high=True)
    failures += run("plane 1 bit 7 leaky: right at once, wrong later", (1, 7),
                    [B.B_ALIVE, B.B_PP_RAM_PASS, B.B_PLANE1_FAIL, B.B_DONE,
                     B.B_BIT0 + 7],
                    B.RES_PP_RAM_OK | B.RES_PLANE1_BAD | B.RES_DONE,
                    leaky=True)
    failures += run("plane 2 bit 6 stuck low", (2, 6),
                    [B.B_ALIVE, B.B_PP_RAM_PASS, B.B_PLANE2_FAIL, B.B_DONE,
                     B.B_STUCK, B.B_BIT0 + 6],
                    B.RES_PP_RAM_OK | B.RES_PLANE2_BAD | B.RES_DONE | B.RES_STUCK)
    failures += run("plane 1 bit 7 intermittent: only on one pass", (1, 7),
                    [B.B_ALIVE, B.B_PP_RAM_PASS, B.B_PLANE_PASS, B.B_PLANE1_FAIL,
                     B.B_DONE, B.B_STUCK, B.B_BIT0 + 7],
                    B.RES_PP_RAM_OK | B.RES_PLANE1_BAD | B.RES_DONE | B.RES_STUCK,
                    flaky=True)
    failures += run("plane 0 (PP RAM) bit 5 -- the tag-list bank", None,
                    [B.B_ALIVE, B.B_PP_RAM_FAIL, B.B_PLANE_PASS, B.B_DONE,
                     B.B_BIT0 + 5],
                    B.RES_DONE, pp_bit=5)
    failures += run("PP RAM gone entirely -- refresh or supply, not a chip", None,
                    [B.B_ALIVE, B.B_PP_RAM_FAIL, B.B_PLANE_PASS, B.B_DONE,
                     B.B_ALL_AT_ONCE] + [B.B_BIT0 + i for i in range(8)],
                    B.RES_DONE, pp_all=True)

    print("\nlive mode -- each pass reports only itself:\n")
    clean = [B.B_ALIVE, B.B_PP_RAM_PASS, B.B_PLANE_PASS, B.B_DONE]
    failures += run_live("healthy machine, three clean passes",
                         [clean, clean, clean],
                         B.RES_PP_RAM_OK | B.RES_DONE)
    # The one that matters. In latching mode this fault is lit for good from the
    # moment it first appears; here pass 3 has to come back clean, or cooling the
    # guilty chip would look exactly like cooling an innocent one.
    failures += run_live("plane 1 bit 7 on pass 2 only: the lamp must clear",
                         [clean,
                          [B.B_ALIVE, B.B_PP_RAM_PASS, B.B_PLANE1_FAIL,
                           B.B_STUCK, B.B_BIT0 + 7, B.B_DONE],
                          clean],
                         B.RES_PP_RAM_OK | B.RES_DONE,
                         broken=(1, 7), flaky=True)

    print("\n" + ("all checks passed" if not failures else f"{failures} failure(s)"))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
