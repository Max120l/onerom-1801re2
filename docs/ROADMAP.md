# Roadmap

Each step with the question that decides how it gets built.

## 1. A replacement for D22 — moved to its own repository

**Answered, and now lives at
[Max120l/kr1801vp1-055](https://github.com/Max120l/kr1801vp1-055).**

The question that gated it was whether the enable logic is combinational or
stateful. Traced against the k1801 cell library, every cell in the
`370`/`373`/`428`/`429` block is a gate or an inverter — no latch, no clock, no
feedback — so it is **combinational** and no CPLD is needed. Two 74x**245**s and
three buffer sections reproduce the part exactly, proven by equivalence check
against a structural model of the die.

Note the part number: **'245, not '640**. The chip does not invert, which is the
opposite of what this roadmap and `D22-KR1801VP1-055.md` used to say.

Two things still to do before the iron comes out, and they belong here because
they are about this machine rather than about the chip:

- **Confirm the low-byte prediction.** V0–V7 has three gate delays against one
  for W0–W7, so the low byte of the AD bus should fail first. `make_regtest`
  already reports which bits fail. If the failure does not match the chip's own
  internal asymmetry, the diagnosis has a hole in it and it is much cheaper to
  find that now.
- **Socket D22 when removing it**, so the original can go back if the substitute
  disappoints.

## 2. Cartridge select on the DS4 socket

The aim: run software from the One ROM rather than only the system monitor.

DS4 is the only socket with a real chip select — the CGM drives it from CE0 —
and the 100000 window is switchable between the on-board ROM and a cartridge.
Serving cartridge content from the same board would make the One ROM a software
loader as well as a ROM.

**The question to answer first: does the socket see a read strobe at all when the
window is banked away from the on-board ROM?**

This matters because it may make the wiring unnecessary. The firmware already
reads the CS pin and already scopes that check to the 100000 window, so if the
cycles arrive, serving two images out of one socket — system ROM when CE0 says
on-board, cartridge content when it does not — is a firmware change and no
soldering at all.

But the evidence so far points the other way: the working assumption in
`main.c` is that the CGM *withholds* EDIN when the window is banked elsewhere,
which is why banking has always come for free. If that holds, the cycles never
reach us and the select and strobe do have to be brought over from the cartridge
slot.

The board can answer this itself, with machinery that already exists: bank the
window away, and watch whether any cycle in 100000–117777 is captured. That is
a coverage question, and coverage is already instrumented.

## 3. Getting images onto the board without rebuilding it

Today a ROM image is compiled into the firmware by `gen_rom_images.py` and the
whole thing is reflashed. For development that is fine; for someone who wants to
run a different program it is absurd.

The target: **the board appears as a USB drive, you drop `.bin` files on it, and
it serves them.** The RP2350 has the USB hardware and 2 MB of flash; TinyUSB mass
storage over a small filesystem is the standard way to do it.

Two things to design around:

- **Selecting between images.** Once several can be on the board at once,
  something has to choose. The two X jumper pads are already on the board and
  currently must never be driven — `build_pin_masks()` asserts exactly that —
  which makes them free as a two-bit image selector. Four images, no new
  hardware, no host software. Note that the *main* jumper block cannot do this
  job: only two of its four columns are readable at all, and one of those is the
  recovery jumper. [BOARD-NOTES.md](BOARD-NOTES.md) has the measurement, and an
  active detection scheme for the X pads that does not depend on internal
  pulls.
- **USB power while the board is in a live machine.** The board takes its power
  from the socket. Plugging a host in while the machine is running means two
  supplies on one rail, and that needs a deliberate answer — a diode, a jumper,
  or simply a documented rule that flashing happens out of circuit — rather than
  being discovered later.

This is also the step that makes the USB reporting in
[DIAGNOSTICS-AS-A-ROM.md](DIAGNOSTICS-AS-A-ROM.md) practically free: once the
USB stack is there for file transfer, a serial port for diagnostics costs
almost nothing extra, and the LED frames can go back to being a fallback.

## 4. An expansion-slot device — the MPI PicoMem

A second, fully working MS 0511 changes the constraint that shaped everything
above: there is now a machine that should never be opened. The cartridge slot
is the answer — it carries the same PP-side MPI bus this project already
speaks, and it is reachable from outside the case.

The idea: an RP2350 card in the cartridge slot. In order, each phase shippable
on its own:

1. **ROM in the slot.** What the ROM-socket firmware already does, moved to a
   proper edge connector, booted from menu item 2. Proves the pinout, the
   mechanicals, and the slot's select/strobe behaviour with zero new bus
   machinery.
2. **Floppy emulation.** The standard controller is four words at 177130, and
   ukncbtl's `emubase/Floppy.cpp` is a register-level model of it — the
   contract to satisfy, and the test bench to develop against. Satisfy it and
   menu item 1 boots stock OS from SD-card images with unmodified drivers.
3. **Hard drive.** Same method against `emubase/Hard.cpp`.
4. **Banked RAM.** The memory map is fixed, so "more RAM" honestly means
   serving the six switchable 8K banks behind port 177054 — a RAM-disk more
   than system memory. Legal on the bus (the slave paces via RPLY); utility
   real; timing unverified until phase 2 teaches us slot writes.

**The question to answer first: the cartridge slot pinout.** The MS 0511
schematic (the four-sheet PDF already in this project's hands — only sheet 1
has been read, for the ROM sockets) almost certainly draws it. Second
question, likely answered by the first: which strobes the slot carries — the
real floppy controller module lives on this connector and has writable
registers, which is strong evidence the slot sees full read AND write cycles,
but the drawing settles it.

This step supersedes step 2 (a real slot card obsoletes the DS4 bodge-wire
cartridge hack) and absorbs step 3 (USB/SD image loading is part of the
design rather than an afterthought). It also opens a door none of the above
could: with two machines, the network boot path in the ROM (menu item 3,
whose entire message set this project has already extracted) could someday
let the healthy machine serve the patient.
