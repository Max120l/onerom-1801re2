#!/usr/bin/env python3
"""Run the test ROM in a small PDP-11 simulator and check what it beacons.

A test ROM that has not been executed is a guess. This runs the real assembled
image against a model of the PP -- 32 KB of RAM, the ROM mapped at 100000, the
power-up vector honoured -- and checks the beacon sequence, both when the RAM is
healthy and when a bit is broken.

    python3 test/test_testrom.py
"""

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "tools"))

import make_testrom                                     # noqa: E402

ROM_BASE = make_testrom.ROM_BASE
BEACON = make_testrom.BEACON


class PP:
    """Enough 1801VM2 to run the test ROM: the instructions it actually uses."""

    def __init__(self, rom, broken_bit=None, broken_at=None):
        self.rom = rom
        self.ram = bytearray(0o100000)
        self.r = [0] * 8
        self.n = self.z = self.v = self.c = 0
        self.beacons = []
        self.broken_bit = broken_bit
        self.broken_at = broken_at

    # --- memory -----------------------------------------------------------
    def read(self, addr):
        addr &= 0xFFFE
        if BEACON <= addr < BEACON + 0o100:
            self.beacons.append((addr - BEACON) // 2)
            return 0
        if addr >= ROM_BASE:
            off = addr - ROM_BASE
            if off + 2 > len(self.rom):
                raise RuntimeError(f"read outside ROM at {addr:06o}")
            return self.rom[off] | (self.rom[off + 1] << 8)
        v = self.ram[addr] | (self.ram[addr + 1] << 8)
        if self.broken_bit is not None and addr == self.broken_at:
            v &= ~(1 << self.broken_bit) & 0xFFFF     # a bit that will not set
        return v

    def write(self, addr, value):
        addr &= 0xFFFE
        if addr >= ROM_BASE:
            return                                     # ROM ignores writes
        self.ram[addr] = value & 0xFF
        self.ram[addr + 1] = (value >> 8) & 0xFF

    # --- operands ---------------------------------------------------------
    def fetch(self):
        w = self.read(self.r[7])
        self.r[7] = (self.r[7] + 2) & 0xFFFF
        return w

    def addr_of(self, mode, reg):
        """Return None for register-direct, else the effective address."""
        if mode == 0:
            return None
        if mode == 1:
            return self.r[reg]
        if mode == 2:
            a = self.r[reg]
            self.r[reg] = (self.r[reg] + 2) & 0xFFFF
            return a
        if mode == 3:
            # @(rn)+, which with rn = pc is the absolute form @#addr: the word
            # inline after the instruction is the address to use.
            a = self.read(self.r[reg])
            self.r[reg] = (self.r[reg] + 2) & 0xFFFF
            return a
        raise RuntimeError(f"addressing mode {mode} not modelled")

    def get(self, mode, reg):
        a = self.addr_of(mode, reg)
        return self.r[reg] if a is None else self.read(a), a

    # --- flags ------------------------------------------------------------
    def setnz(self, v):
        self.n = 1 if v & 0x8000 else 0
        self.z = 1 if v == 0 else 0

    def step(self):
        op = self.fetch()
        top = op >> 12
        if top in (1, 2, 6):                            # MOV, CMP, ADD
            sm, sr = (op >> 9) & 7, (op >> 6) & 7
            dm, dr = (op >> 3) & 7, op & 7
            src, _ = self.get(sm, sr)
            if top == 1:                                # MOV
                da = self.addr_of(dm, dr)
                if da is None:
                    self.r[dr] = src
                else:
                    self.write(da, src)
                self.setnz(src)
                self.v = 0
            elif top == 2:                              # CMP: src - dst
                dst, _ = self.get(dm, dr)
                res = (src - dst) & 0xFFFF
                self.setnz(res)
                self.c = 1 if src < dst else 0
                self.v = 0
            else:                                       # ADD
                da = self.addr_of(dm, dr)
                dst = self.r[dr] if da is None else self.read(da)
                res = (src + dst) & 0xFFFF
                if da is None:
                    self.r[dr] = res
                else:
                    self.write(da, res)
                self.setnz(res)
            return True
        if op & 0o177700 == 0o005100:                   # COM
            dm, dr = (op >> 3) & 7, op & 7
            da = self.addr_of(dm, dr)
            v = (~(self.r[dr] if da is None else self.read(da))) & 0xFFFF
            if da is None:
                self.r[dr] = v
            else:
                self.write(da, v)
            self.setnz(v)
            self.v = 0
            self.c = 1
            return True
        if op & 0o177700 == 0o005000:                   # CLR
            dm, dr = (op >> 3) & 7, op & 7
            da = self.addr_of(dm, dr)
            if da is None:
                self.r[dr] = 0
            else:
                self.write(da, 0)
            self.n = self.v = self.c = 0
            self.z = 1
            return True
        if op & 0o177700 == 0o005700:                   # TST
            dm, dr = (op >> 3) & 7, op & 7
            v, _ = self.get(dm, dr)
            self.setnz(v)
            self.v = self.c = 0
            return True
        base, off = op & 0o177400, op & 0xFF
        if off > 127:
            off -= 256
        taken = {0o000400: True, 0o001000: not self.z, 0o001400: self.z,
                 0o103400: self.c, 0o103000: not self.c,
                 0o100000: not self.n, 0o100400: self.n}.get(base)
        if taken is not None:
            if taken:
                self.r[7] = (self.r[7] + 2 * off) & 0xFFFF
            return True
        if op == 0:                                     # HALT
            return False
        raise RuntimeError(f"unmodelled opcode {op:06o} at {self.r[7] - 2:06o}")

    def run(self, entry, psw_unused, limit=4_000_000):
        self.r[7] = entry
        for _ in range(limit):
            if not self.step():
                return "halted"
            if len(self.beacons) > 6 and self.beacons[-4:] == [
                    make_testrom.B_DONE] * 4:
                return "parked"
        return "ran out of steps"


def main() -> int:
    rom, _ = make_testrom.build()
    entry = rom[make_testrom.VECTOR - ROM_BASE] | \
        (rom[make_testrom.VECTOR - ROM_BASE + 1] << 8)
    print(f"power-up vector points at {entry:06o}")
    assert entry == make_testrom.ENTRY

    failures = 0

    print("\nhealthy RAM:")
    pp = PP(rom)
    print(f"  {pp.run(entry, None)}")
    seq = []
    for b in pp.beacons:
        if not seq or seq[-1] != b:
            seq.append(b)
    print(f"  beacon sequence: {seq}")
    want = [make_testrom.B_ALIVE, make_testrom.B_RAM_PASS, make_testrom.B_DONE]
    if seq != want:
        print(f"  FAIL: expected {want}")
        failures += 1

    # Bit 5 at address 000100: that address does not have bit 5 set, so pass 1
    # never writes a 1 there and cannot see the fault. Only the complement pass
    # catches it -- which is the whole reason there are two passes.
    print("\nRAM with a bit that will not set, at an address that hides it from pass 1:")
    pp = PP(rom, broken_bit=5, broken_at=0o000100)
    print(f"  {pp.run(entry, None)}")
    seq = []
    for b in pp.beacons:
        if not seq or seq[-1] != b:
            seq.append(b)
    print(f"  beacon sequence: {seq}")
    want = [make_testrom.B_ALIVE, make_testrom.B_RAM_FAIL, make_testrom.B_DONE]
    if seq != want:
        print(f"  FAIL: expected {want}")
        failures += 1

    print("\n" + ("all checks passed" if not failures else f"{failures} failure(s)"))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
