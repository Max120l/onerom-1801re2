# Emulating a 1801RE2 on a One ROM Fire 24 rev E

Firmware that makes an RP2350-based One ROM board behave like a Soviet 1801RE2
mask ROM — a chip that sits on the MPI bus, the domestic equivalent of DEC's
Q-bus, where address and data share one set of sixteen lines.

Target machine: **Elektronika MS 0511 (UKNC)**, which uses four of them.

**Status: working. An Elektronika MS 0511 boots with all four of its 1801RE2
mask ROMs replaced by a single One ROM Fire 24 in the DS4 socket.**
The image conversion tooling is finished and tested. Read [Before you plug anything in](#before-you-plug-anything-in)
first, and [Prior art](#prior-art) before deciding this is the right project at
all — someone has already built a purpose-made board for this job.

## Why this is not a One ROM configuration

One ROM is built around one shape of chip: address lines in, data lines out,
chip select gating the drivers. The v0.7 firmware makes that explicit — serving
is decomposed into three families of PIO algorithm (`CS*`, `ADDR*`, `DATA*`),
selected per ROM type by the Rust pre-processor, and every one of them assumes
address and data occupy separate pins. `ADDR0` reads a block of address pins and
uses the value to index a table; `DATA0` puts a word from a FIFO onto the data
pins. There is nothing to configure that makes those two sets of pins the same
set of pins.

The plugin API is not a way in either. Plugins run on the CPU cores alongside
PIO serving, and the API (`firmware/ora/api.h`) exposes logging, memory, USB,
IRQs, ROM slot access and an address monitor — but no way to claim the socket
GPIOs or replace the serving state machines. A plugin can watch the bus; it
cannot drive it.

So this is custom firmware that treats the Fire 24 as a well-documented RP2350
carrier board with a known socket-to-GPIO map, not an extension of One ROM.
Upstreaming it later as a fourth algorithm family would be reasonable; starting
there would not.

## The chip

The 1801RE2 is a 4K × 16 mask ROM. Internally it latches a 12-bit word address
from nAD1–nAD12 and compares nAD13–nAD15 against a mask-programmed three-bit
value — the "code" — that fixes which 8 KB window of the 64 KB address space it
answers for. It drives data and asserts nRPLY only on a match. Everything on the
bus is inverted, hence the `n` prefixes.

| Code | Window (octal)  |
|------|-----------------|
| 000  | 160000–177777   |
| 001  | 140000–157777   |
| 010  | 120000–137777   |
| 011  | 100000–117777   |
| 100  | 060000–077777   |
| 101  | 040000–057777   |
| 110  | 020000–037777   |
| 111  | 000000–017777   |

A read cycle runs: the host puts the address on the AD lines and asserts nSYNC;
the slave latches it; the host releases the AD lines and asserts nDIN; the slave
drives data and pulls nRPLY low; the host takes the data and releases nDIN; the
slave releases the bus; nSYNC releases.

### Pinout

From the KR1801RE2 datasheet, table 11.26 and figure 11.30. Power follows JEDEC
and matches what the Fire 24 hard-wires, so the board drops in unmodified.

| pin | signal | | pin | signal | | pin | signal |
|-----|--------|-|-----|--------|-|-----|--------|
| 1 | RD (nDIN) | | 9 | AD9 | | 17 | AD12 |
| 2 | AN (nRPLY) | | 10 | AD10 | | 18 | AD13 |
| 3 | SYN (nSYNC) | | 11 | AD11 | | 19 | AD14 |
| 4 | AD4 | | 12 | GND | | 20 | AD15 |
| 5 | AD5 | | 13 | AD3 | | 21 | n/c |
| 6 | AD6 | | 14 | AD2 | | 22 | n/c |
| 7 | AD7 | | 15 | AD1 | | 23 | **CS** |
| 8 | AD8 | | 16 | AD0 | | 24 | Ucc |

Everything is active low, which is why the k1801 RTL names these nAD, nSYNC,
nDIN and nRPLY. Two things are worth pulling out:

**There is a chip select on pin 23**, which is how the UKNC banks a window out —
see below. The design had assumed such a pin had to exist; it does.

**AN on pin 2 is the reply** — nRPLY, the signal the ROM asserts to complete a
transfer. Table 11.26's "вход" against it is a misprint; figure 11.30 draws it
on the output side. It is driven here, open-drain: the output value is always
low and only the direction is toggled, so the line is either pulled down or
released to the bus pull-up, never driven high.

## What the UKNC does with them

The MS 0511 has two K1801VM2 processors — a central one at 8 MHz and a
peripheral one at 6.25 MHz — and **the ROMs are on the peripheral processor's
bus**, not the central one's. That is where any scope probing has to happen.

The PP address space, and where the four chips land in it:

| Range (octal)   | Contents | Chip |
|-----------------|----------|------|
| 000000–077777   | PP RAM, 32 KB | |
| 100000–117777   | banked window | 1801RE2-205, code 011 |
| 120000–137777   | system ROM | 1801RE2-206, code 010 |
| 140000–157777   | system ROM | 1801RE2-207, code 001 |
| 160000–176777   | system ROM | 1801RE2-208, code 000 |
| 177000–177777   | I/O page | |

Two consequences fall straight out of that table, and both are the difference
between working firmware and a board that fights the machine for its own bus.

**Every window can be banked away.** Port 177054 decides, per window, whether
the PP sees ROM or RAM: bit 5 for 120000, bit 6 for 140000, bit 7 for 160000,
and bits 0–4 for the 100000 window, which can also be switched to one of six
banks of external cartridge ROM. A mask ROM has no logic to do that itself, so
something external must gate it. Sheet 1 of the schematic shows how, and it is
not what you would guess:

- **Pin 1 is fed by EDIN, not by the raw K1DIN net.** EDIN comes off the output
  side of the CGM (D10, pin 53) — a read strobe already qualified by the
  banking state. A window switched to RAM simply never strobes its ROM. This is
  the real per-window gate, and it means the gating arrives for free: the
  firmware waits on the read strobe and never hears one it should not answer.
- **CS on pin 23 is strapped to ground on DS1, DS2 and DS3** — permanently
  selected. Only DS4, the 205 covering the switchable 100000 window, has it
  driven, from the CGM's CE0. So CS arbitrates one window, not four. It is also
  what settles the polarity question: grounded means selected, so active low.

The consequence for the firmware is in [Serving](#design), and it is the reason
the response machine is re-armed every cycle.

**The code 0 chip overlaps the I/O page.** Its window is 160000–177777 but only
160000–176777 is ROM; the top 512 bytes belong to the machine's registers.
`gen_rom_images.py` therefore serves only 3840 of the 4096 words for a code 0
image by default, and `--full-window` overrides that if you ever need it.

### Which socket to use

Because the chip decodes its own window from nAD13–nAD15, one board in *any* of
the four sockets can answer for all four. Every socket carries the same 1AD bus,
the same K1SYNC, the same EDIN and the same RPLY. Only CS differs, and that
decides how much care is needed:

- **DS1, DS2 or DS3** — CS strapped to ground, so the board is permanently
  selected and per-window banking comes entirely from EDIN. Nothing to configure
  beyond `SOCKET_CS_CODE 0xFF`.
- **DS4** — CS is CE0, which means "the 100000 window belongs to the on-board
  ROM rather than to RAM or a cartridge". It says nothing about the other three
  windows, which have no CE and are gated by EDIN alone. Set `SOCKET_CS_CODE`
  to `03` so the check is scoped to that window. Applying CS globally would let
  one deasserted CE silence three windows it has no authority over — the whole
  system ROM disappearing whenever software banked something into 100000.

**Whichever socket you use, every original it answers for must come out.** Two
devices driving the same RPLY and the same AD lines is a bus fight, and the
1801RE2 has no idea it has been replaced. If only DS4 is socketed and the other
three are still soldered down, generate an image set containing *only* the 205 —
that is a straight one-for-one replacement and needs no desoldering.

### Adding ROMs beyond the four windows

The PP has no spare address space: 000000-077777 is RAM, 120000-176777 is the
three fixed ROM windows, 177000 up is I/O. The only place more ROM can go is the
100000 window, which port 177054 switches between PP RAM, the on-board 205, and
cartridge banks — two slots of three 8 KB banks each.

The bus really is shared. XS1 carries the same 1AD lines, K1SYNC, K1RPLY and
K1DIN as the ROM sockets, plus all four chip enables: CE0 on B12, CE1 on B14,
CE2 on B13, CE3 on A12. A cartridge is not on a separate bus, it is on this one
with its own enable.

What the DS4 socket does **not** carry is which bank is selected. Pin 23 is CE0
alone, meaning "the on-board ROM owns this window" — so when software switches
to a cartridge, CE0 deasserts and the board correctly goes quiet, but it has no
way to know that bank 2 of slot 1 is now wanted. Serving cartridge banks from
this socket therefore needs CE1, CE2 and CE3 brought in by wire.

There are pins for it. Socket pins 21 and 22 are not connected in the machine
and land on GPIO 12 and 14, and the X1/X2 jumper pads give GPIO 9 and 8 — four
spare inputs for three enables. RAM is not the constraint either: eight windows
already cost 128 KB of the RP2350's 520 KB, and six more banks would add 96 KB.

One thing to establish first: whether EDIN asserts for reads the machine has
directed at a cartridge, or only for the on-board ROM. If it is qualified by CE0
as well, the read strobe never arrives for a cartridge access and K1DIN has to
be wired in too. A scope on pin 1 while software selects a cartridge bank
settles it.

The alternative is to stop fighting the socket and build into a cartridge, which
carries the bus, the strobes and the enables by design. That trades three bodge
wires for a mechanical adapter from a 24-pin DIP footprint.

## Prior art

Before going further, know that this problem has been solved. The **RE-mulator**
is a DIP-24 board built around an STM32F205 at 120 MHz that drops straight into
a 1801RE2 or 1801RR1 socket, takes its power from the socket, can be reflashed
in place, and — with additional external inputs — stands in for up to four
separate chips at once. It has been verified in a BK-0010 running at 3 MHz
replacing a 1801RE2-017. Four chips at once is exactly the UKNC shape, and those
"external inputs" are almost certainly the per-window enables discussed above.

Two honest conclusions:

- If your goal is simply a working UKNC with reprogrammable ROMs, build or buy
  a RE-mulator. It is purpose-made, proven on real hardware, and a fraction of
  the work of this.
- If your goal is to do it on One ROM hardware, its existence is still good
  news: it settles that the part is DIP-24, that socket power is usable, and
  that a 120 MHz CPU-in-the-loop design is fast enough — which is the core
  assumption this design rests on. Its manual also documents the pinout, which
  is the one thing blocking this repo.

## The timing is easier than a 2364, not harder

This is the part worth internalising before worrying about speed. A 2364 gives
you a hard deadline: address changes, and valid data must be on the pins inside
the access time, with nothing to say otherwise. MPI is a fully asynchronous
handshake — the host waits for nRPLY. Responding late costs wait states, not
corruption, and the only real limit is the host's bus timeout, which is orders
of magnitude longer than anything the RP2350 will take.

That is what makes a CPU-in-the-loop design safe here, and it is why the address
decode below is allowed to be a dozen instructions of table lookup rather than
something exotic. The PIO handles the edges; the CPU has from nSYNC to nDIN to
think, and even overrunning that window is survivable.

## The hard constraint: the GPIO 8/9 gap

From One ROM's own board description (`rust/config/json/fire-24-e.json`), the
Fire 24 rev E maps its 22 signal socket pins onto GPIO 0–7 and GPIO 10–23.
**GPIO 8 and 9 go to the X1/X2 jumper pads, not to the socket.**

That gap is why this cannot be a straightforward port of One ROM's approach.
One ROM leans on being able to read a contiguous run of address pins in a single
PIO `in pins, n` and use the raw value — however scrambled the bit order — as a
table index, with the pre-processor pre-scrambling the image to match. Order
does not matter; contiguity does. On Fire 24 rev E the longest run of
socket-connected GPIOs is 14, so sixteen AD lines can never be one window, no
matter how the chip's pins fall.

The way out is to stop trying. Read GPIO 0–23 as a single 24-bit field —
including the two jumper bits, which are static and harmless — and let the CPU
un-scramble it with three 256-entry lookup tables. Sixteen output bits are
handled the same way, in reverse, but entirely at boot: the drive pattern for
every one of the 4096 words is precomputed, so the serving path never scatters a
bit at runtime.

With the real pinout in, the sixteen AD lines land on GPIO

```
0 1 2 3 4 5 6 7  10 11  13  19 20 21 22 23
```

with nSEL on 15, nDIN on 16, nRPLY on 17 and nSYNC on 18. Nothing contiguous
anywhere, which settles the question — but every one of them is inside the
24-bit field, so one read and one write still cover the whole bus. Socket pins
21 and 22 are not connected; they land on GPIO 12 and 14, feed no address bit,
and are pulled down so they do not float.

GPIO 8 and 9 are deliberately never muxed to the PIO and never appear in a
direction mask, so a fitted X jumper cannot be shorted by an output driver.

## Design

Two PIO state machines and one CPU core.

**`mpi_capture`** waits for nSYNC high (so a machine that starts mid-cycle
resynchronises rather than latching garbage), then for the falling edge, then
snapshots all 24 GPIOs into the RX FIFO. The MPI address hold time after nSYNC
is far longer than the ~20 ns of synchroniser plus one instruction, so sampling
straight off the edge lands well inside the valid window.

**Core 1** pops the snapshot, gathers the logical address through
`g_gather[3][256]` (pin inversion baked in), picks the window table by the top
three bits, and pushes the precomputed 24-bit drive pattern. If the address is
not ours it pushes nothing, and the response machine simply stays blocked — a
non-matching cycle produces no bus activity at all, which is the behaviour you
want when you are one of several devices on a shared bus.

**`mpi_respond`** waits for nDIN, presents the data, enables the AD drivers, and
one instruction later enables the nRPLY driver — giving the host data setup time
before the handshake completes. nRPLY is open-drain by construction: its output
value is always 0, so enabling the direction pulls it low and clearing the
direction returns it to the bus pull-up. The line is never driven high.

Roughly six PIO cycles from the nDIN edge to data and nRPLY, about 40 ns at
150 MHz — faster than the chip being emulated.

One correctness wrinkle drives a design choice worth spelling out. We latch on
the address strobe, which is asserted for **every** cycle on the bus, but the
read strobe only arrives for a read the host has decided belongs to us — on the
UKNC, EDIN, which the CGM withholds when the window is banked to RAM. So a
prepared cycle routinely ends without ever being served: writes, cycles for
other devices, banked-out windows.

Once the response machine has executed its `pull`, the pattern is in the OSR and
the TX FIFO reads empty, so neither a FIFO check nor watching the address strobe
can tell it is sitting on a stale value. It would then apply that value to
whatever read came next — wrong data, driven confidently. So `rearm_respond()`
puts the machine back to idle unconditionally at the start of every cycle: a few
register writes inside the strobe-to-strobe gap, in exchange for a machine that
cannot carry state across cycles.

### Files

| File | |
|------|--|
| `firmware/mpi_rom.pio` | the two state machines |
| `firmware/main.c` | hardware setup and the core 1 serving loop |
| `firmware/decode.c` | the per-cycle arithmetic, free of SDK dependencies |
| `test/test_decode.c` | host test of that arithmetic |
| `firmware/board_fire24e.h` | socket-to-GPIO map and chip pinout |
| `firmware/rom_images.h` | image table interface |
| `tools/re2_convert.py` | dump format conversion, tested |
| `tools/gen_rom_images.py` | emits `firmware/rom_images.c` from dumps |
| `tools/diagnose_dump.py` | tells a bad dump from a bad chip |
| `tools/selftest.py` | the self-test pattern, shared by generator and checker |
| `tools/check_selftest.py` | diagnoses a dump of the self-test build |

### Diagnosing a dump

A reader that ignores RPLY cannot tell "the chip did not answer" from "the chip
answered with these bits". When the chip stays silent the AD lines float to
whatever the rig's pull resistors give, so a bit position reads as one constant
value across the whole dump — indistinguishable, by eye, from a stuck bit.

The chip only answers when nAD13–nAD15 match its mask-programmed code, so
addressing the wrong 8 KB window silences it entirely. That makes "wrong window"
and "dead chip" look alike, and the four UKNC chips have four different codes.

`diagnose_dump.py` separates them by shape. Whole bus constant means no reply;
one or two bit positions constant means a stuck bit or an open line; nothing
constant but still disagreeing with a reference means addressing or timing.
Pass `--reference` with a known-good image — the k1801 archive has all four
UKNC chips — and it will say which pattern the differences fit.

The analysis is invariant under the programmer-order transform, so it does not
matter which orientation either file is in.

### Image format

The 1801RE2 dumps in the [1801BM1/k1801](https://github.com/1801BM1/k1801)
archive are in "Sterkh programmer" order: every byte inverted, and the word
addresses inverted too. Reduced to bytes, and matching the original `rev16`
utility exactly:

```
out[addr ^ 0x1FFE] = ~in[addr]
```

`0x1FFE` inverts byte-address bits 1–12 — the twelve word-address bits — leaving
bit 0, which selects the byte within the word. The transform is its own inverse.
Files carry two trailing bytes: the chip code, then a constant `0x03`.

This part is done and checked. Round-tripping is an identity across all 50 dumps
in the archive, the codes recovered from the trailers match the archive's own
table, and converted images are unmistakably PDP-11 code where the raw ones are
not — the BK-0010 monitor goes from 0 to 80 occurrences of `RTS PC` and 0 to 159
of `JSR PC`.

The four UKNC images are `205_mc0511.rom` through `208_mc0511.rom` in that
archive, and they generate as a complete set:

```console
$ ./tools/re2_convert.py 205_mc0511.rom
205_mc0511.rom: code=3
  as read   RTS PC=   0  JSR PC=   0
  converted RTS PC= 110  JSR PC=  76

$ ./tools/gen_rom_images.py -o firmware/rom_images.c \
      205_mc0511.rom 206_mc0511.rom 207_mc0511.rom 208_mc0511.rom
wrote firmware/rom_images.c: 4 image(s), windows ['0', '1', '2', '3']
```

which yields, with 208 correctly stopping short of the I/O page:

```c
const mpi_image_t mpi_images[] = {
    { "205_mc0511", 3, 4096, img_re2_205_mc0511 },
    { "206_mc0511", 2, 4096, img_re2_206_mc0511 },
    { "207_mc0511", 1, 4096, img_re2_207_mc0511 },
    { "208_mc0511", 0, 3840, img_re2_208_mc0511 },
};
```

## Before you plug anything in

Power is settled: the 1801RE2 follows JEDEC, so pin 24 is Ucc and pin 12 GND,
exactly what the Fire 24 hard-wires to its regulator and ground plane. The
board drops in unmodified. CS polarity is settled too: the schematic straps it
to ground on three of the four sockets, so grounded means selected, so active
low. The whole pin configuration is now read off documentation rather than
guessed. What is left is behavioural, not electrical.

**Write behaviour.** Worth answering from the datasheet rather than guessing:
**does a real 1801RE2 assert nRPLY on a write into its window?** If it does not,
writes to ROM produce a bus timeout trap, and software may depend on that. The
BK-ROM-Disk GAL (`RPLY = CHIPSEL & (DIN # !_DOUT)`) does reply to writes, but
that is a RAM-disk controller, not a ROM. If the answer is "no", nothing needs
adding: this firmware only ever responds to nDIN.

## Building

Needs `arm-none-eabi-gcc`, CMake, Ninja and the Pico SDK. The board carries an
RP2354A — an RP2350A with 2 MB of stacked flash — so it builds as a plain
rp2350 target.

```console
$ ./tools/gen_rom_images.py -o firmware/rom_images.c \
      205_mc0511.rom 206_mc0511.rom 207_mc0511.rom 208_mc0511.rom
$ cd firmware
$ PICO_SDK_PATH=/path/to/pico-sdk cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release .
$ cmake --build build
```

Flash `build/mpi_rom.bin` with [One ROM Web](https://onerom.org/web) or the CLI's
`--firmware` option — the same raw-binary route One ROM uses for its own
`onerom-rp235x.bin`, loaded at 0x10000000. A `.uf2` is produced too, but the
board has no BOOTSEL button, so the USB route is the practical one.

### Getting back

One ROM's USB is a TinyUSB device stack presenting its own vendor interface on
VID 0x1209 / PID 0xF542 — "picobootx", an extended picoboot. The web flasher
therefore talks to One ROM's *running firmware*, not to a bootloader. Flashing
this firmware replaces that stack, so the board stops appearing in One ROM Web
entirely.

One ROM Web goes further and validates what you hand it — a binary that is not
recognisable One ROM firmware is refused outright, so it will not flash this
project at all. Two routes do work:

- **[pico⚡flash](https://picoflash.org)**, by the same author. It speaks
  picoboot rather than checking for One ROM metadata, and reaches both One ROM's
  running firmware and the bare bootrom.
- **The bootrom's mass-storage volume**, which takes a `.uf2` by drag and drop.

For the second, header **J2** on the Fire 24 rev E carries everything needed:

| J2 pin | | J2 pin | |
|---|---|---|---|
| 1 | SEL_A | 2 | GND |
| 3 | SEL_B | 4 | GND |
| 5 | **BOOT** | 6 | SWCLK |
| 7 | **RUN** | 8 | SWDIO |

Short **pin 5 to pin 4** and power up, and the volume mounts. BOOT reaches
QSPI_SS through a 1K against its 10K pull-up, so grounding it wins. If the board
is already powered, briefly ground **pin 7** (RUN) instead of power-cycling.

J2 is a footprint, though, not necessarily a fitted header — on a board that
shipped without it there is nothing to short. In that case SWD is the route, and
it is the better one anyway since it works regardless of what is on the flash:

```console
$ openocd -f interface/cmsis-dap.cfg -f target/rp2350.cfg       -c "adapter speed 5000"       -c "program firmware/build/mpi_rom.elf verify reset exit"
```

SWCLK and SWDIO are J2 pins 6 and 8, ground on 2 or 4. Those pins double as
image-select jumpers C and D, so leave those jumpers off while programming.

That route needs no working firmware at all, which makes it the real safety net:
a corrupt image sends the bootrom to USB by itself.

On top of that, **image-select jumper 0 doubles as a recovery jumper**, so you do
not have to go looking for the BOOT pad. Fit it and power on:
before a single socket pin is touched, the board hands straight back to the
bootrom's USB mode. It is checked first thing in `main()` so that it still works
when the rest of this firmware does not — though note it does depend on this
firmware booting at all, which is why BOOT-to-GND remains the fallback beneath
it.

The check does not assume which rail the jumper ties to. A floating pin follows
whichever internal pull is applied and a driven one does not, so comparing a
read under pull-down with a read under pull-up detects a fitted jumper either
way — worth the few microseconds when the cost of getting it backwards is a
board that never runs, or one that cannot be recovered.

`rom_images.c` is generated and gitignored — it holds actual ROM contents, which
have no business in the repository.

## Testing

`decode.c` holds the whole per-cycle arithmetic and deliberately has no SDK
dependency, so the interesting half of the firmware can be tested with nothing
but a host compiler:

```console
$ make -C test check
pin map
window index
round trip over all four windows
  16128 addresses served correctly
silence outside the served windows
  16640 addresses correctly left alone
AD0 does not change the word selected

all checks passed
```

The round-trip test presents every address in every window the way the host
would — inverted and scattered across the real pin map from `board_fire24e.h` —
runs it through the same gather tables the firmware builds, and reads the answer
back off the resulting drive pattern with an independently written model of the
bus. It also checks the board stays off the bus everywhere it should: the four
windows it does not serve, and the I/O page the code 0 chip must stop short of.

The tests have been mutation-checked. Dropping the complement in
`mpi_window_index()`, forgetting that data is inverted on the wire, and serving
the full window over the I/O page are each caught.

What this does **not** test is the PIO. That needs hardware — see below.

### Result on hardware

Read back through an Arduino-based 1801RE2 reader, all eight windows answered
with the right data in the right places: address capture, window decode, data
drive and bus turnaround all work. The code 0 window returned exactly 3840
words, so the I/O page truncation holds too.

About 3% of reads came back displaced by one bit — the correct word, correctly
selected, misaligned. A parallel bus cannot displace bits, so that belongs to
whatever samples the lines, not to the board; `check_selftest.py` now names it
rather than blaming an address line.

### Result in the machine

An MS 0511 with all four mask ROMs removed and one board in the DS4 socket
initialises its video RAM and reaches a cursor. That settles the two things a
bench reader cannot exercise: **nRPLY**, which a reader latching on a fixed
delay never looks at, and **latency against a live bus**, where being slow costs
wait states right up until the read strobe has come and gone before the reply is
ready, and then the cycle gets nothing at all.

What is still unexercised is **CE0**. It only speaks for the 100000 window and
only matters when software banks that window to RAM or to a cartridge, so
ordinary booting never touches it. The board treats an open CS as selected, so a
surprise there fails toward answering rather than falling silent.

The status LED reports the remaining unknown directly: lit means answering, dark
means nothing is asking, and blinking means replies are being prepared and not
taken — some of which is normal, but a reply assembled too late to be sampled
lands in the same count.

## Testing against a ROM reader

If you have a reader that can dump a real 1801RE2, you already have an MPI bus
master, and it is a far better first test than the machine: it exercises address
capture, window decode, the data drive and the bus turnaround, with nothing
expensive attached and no video timing to corrupt.

Flash the self-test build rather than a real ROM image:

```console
$ ./tools/gen_rom_images.py --selftest -o firmware/rom_images.c
$ cmake --build firmware/build
```

That fills all eight windows with a pattern whose every word encodes both its
own index and the window it belongs to. Dump it back through the reader, then:

```console
$ ./tools/check_selftest.py dumped.bin --window 3
```

The point is that the three failures which look identical in a dump of real code
are separable here:

| symptom | reported as |
|---|---|
| board never drove the bus | every bit stuck at one level |
| wrong window answered | a clean pattern, but from another window |
| a data line not driving | that bit position never changes |
| an address line swapped or stuck | valid words at permuted indices, and the XOR names the bits |

A stuck data line below bit 12 also perturbs the index a word claims, so the
checker masks those bits out before blaming the address lines — otherwise one
bad data line reports as an address fault too.

Two things to get right on the bench. Power the board and the reader from the
same supply so they rise together: that is the case One ROM validated for the
brief window where 5 V is present before the regulator has brought VDD up.
And make sure the reader either grounds pin 23 or leaves it open — the firmware
pulls CS down internally so an open pin reads as selected, matching the three
UKNC sockets that strap it to ground.

If the reader does not wait for RPLY but latches after a fixed delay, a passing
dump proves the data path but says nothing about the reply. Worth knowing which
you have before reading too much into a green result.

## Bring-up

Do not go straight into a host.

1. Build, and check the pin map against your own reading of the datasheet.
   `board_fire24e.h` carries the pinout in a comment for exactly that.
2. Drive the board from a second RP2350 running a synthetic MPI master, or from
   a logic analyser plus a hand-clocked cycle. Check the address snapshot
   decodes to what you presented, and that the AD lines and nRPLY stay hi-Z for
   an address outside the configured window — that is the test that protects the
   host's bus drivers.
3. Scope the real machine before committing — on the **peripheral** processor's
   bus. Capture nSYNC, nDIN, nRPLY and a couple of AD lines during a ROM read
   with the original chip in place. That gives you the actual SYNC-to-DIN gap,
   which tells you how much slack core 1 has, and shows whether the AD lines are
   driven push-pull or open-drain. This firmware drives them push-pull during
   its response window; if the host's pull-ups are weak and something else is
   contending, that assumption needs revisiting.
4. On the same capture, watch EDIN across a port 177054 write that banks RAM
   into that window, and confirm the strobe stops arriving. That is the gate
   the whole design leans on, so it is worth seeing rather than assuming.
5. Only then, one image, one window, in the host. Start with 206 or 207 — the
   plain system ROM windows, no banking games, no I/O page adjacency, and CS
   grounded so there is one less variable.

On levels: the RP2350 GPIOs are directly connected to the socket, with no
buffers, and are 5 V tolerant to 5.5 V once VDD is up. One ROM uses 8 mA drive
strength for 5 V hosts and has validated the 20 µs power-on window where VDD is
still rising; the same analysis applies here and is worth reading in the
project's `docs/VOLTAGE-LEVELS.md`.

## Sources

- [One ROM](https://github.com/piersfinlayson/one-rom) — board description
  (`rust/config/json/fire-24-e.json`), serving algorithms
  (`docs/firmware-rewrite.md`), plugin API (`firmware/ora/api.h`), levels
  (`docs/VOLTAGE-LEVELS.md`)
- KR1801RE2 datasheet, table 11.26 and figure 11.30 — the pinout
- Elektronika MS 0511 schematic, revision 5 (sheet 1) — DS1–DS4 wiring, the CGM
  at D10, and the EDIN / CE0–CE3 gating
- [1801BM1/k1801](https://github.com/1801BM1/k1801) — 1801RE2 description, code
  table, dump archive, and the `rev16` conversion utility
- [vldmrrr/BK-ROM-Disk](https://github.com/vldmrrr/BK-ROM-Disk) — GAL equations
  for a device on the same bus; useful confirmation of how nSYNC latching and
  nRPLY generation are done in practice
- [nzeemin/ukncbtl](https://github.com/nzeemin/ukncbtl) — the UKNC PP memory map
  and the port 177054 window gating, in
  `emulator/emubase/Memory.cpp`, `CSecondMemoryController::UpdateMemoryMap()`
- [RE-mulator](https://zx-pk.ru/threads/21519-re-mulyator-vnutriskhemnyj-emulyator-1801re2-1801rr1.html)
  — the existing DIP-24 1801RE2/RR1 in-circuit emulator
- [Elektronika MS 0511](https://ru.wikipedia.org/wiki/Электроника_МС_0511) —
  processors, clocks, and the 1801RE2-205..208 ROM set
- [One ROM Fire 24](https://shop.piers.rocks/product/one-rom-fire-24/),
  [onerom.org](https://onerom.org/)
