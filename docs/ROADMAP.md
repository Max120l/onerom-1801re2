# Roadmap

Three things, in order, each with the question that decides how it gets built.

## 1. A replacement for D22

The thermal fault is traced to D22, a КР1801ВП1-055 bus transceiver
([details](D22-KR1801VP1-055.md)). It is a gate array and cannot be bought, but
its function is ordinary and a substitute from catalogue parts is a weekend's
work — two 74x640s and three sections of a '244.

**The question to answer first: is the enable logic combinational, or does it
have state?**

The diagram's control block is a network of cells labelled `373`, `428`, `429`
and `370` driven from pin 20 (direction) and pin 41 (/OE). **Those numbers are
the gate array's internal cell library, not 74-series part numbers** — `373`
here is not an octal latch unless the connectivity says so. Trace it.

- purely combinational from pins 20 and 41 → two '640s drop straight in
- anything stateful → a 5 V CPLD, an ATF1504AS or similar, which is one chip
  rather than several and can reproduce the control exactly

Two things before the iron comes out:

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
