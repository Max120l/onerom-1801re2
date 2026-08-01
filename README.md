# Emulating a 1801RE2 on a One ROM Fire 24 rev E

Firmware that makes an RP2350-based One ROM board behave like a Soviet 1801RE2
mask ROM — a chip that sits on the MPI bus, the domestic equivalent of DEC's
Q-bus, where address and data share one set of sixteen lines.

Target machine: **Elektronika MS 0511 (UKNC)**, which uses four of them.

**Status: design plus a skeleton. The bus protocol code has never run on
hardware, and the chip pinout is not yet filled in.** The image conversion
tooling is finished and tested. Read [Before you plug anything in](#before-you-plug-anything-in)
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
the socket must be carrying an enable from external decode. **Ignore it and you
will drive the bus while the PP's RAM is also driving it.** Which pin it arrives
on is one of the things to confirm from the schematic; `GPIO_nSEL` in
`board_fire24e.h` is where it goes.

**The code 0 chip overlaps the I/O page.** Its window is 160000–177777 but only
160000–176777 is ROM; the top 512 bytes belong to the machine's registers.
`gen_rom_images.py` therefore serves only 3840 of the 4096 words for a code 0
image by default, and `--full-window` overrides that if you ever need it.

One upside: because there is no per-chip select in the ROM itself, a single
board can answer for several windows at once, so all four UKNC chips are within
reach of one Fire 24 — provided the real chips are out and the enable lines for
the other three sockets are wired across.

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

One correctness wrinkle is handled explicitly. A cycle we answered that turns
out not to be a read — a write into our window, or an abandoned transfer —
leaves the response machine holding a pattern it would wrongly apply to the next
read. `discard_stale_response()` drops it once nSYNC releases without nDIN
having fired.

### Files

| File | |
|------|--|
| `firmware/mpi_rom.pio` | the two state machines |
| `firmware/main.c` | table construction and the core 1 serving loop |
| `firmware/board_fire24e.h` | socket-to-GPIO map (verified) and chip pinout (**not** verified) |
| `firmware/rom_images.h` | image table interface |
| `tools/re2_convert.py` | dump format conversion, tested |
| `tools/gen_rom_images.py` | emits `firmware/rom_images.c` from dumps |

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

Three things are unresolved. The first two can destroy hardware.

**Power pin position.** The Fire 24 hard-wires socket pin 24 to its regulator
input and pin 12 to ground, because that is where 23xx/27xx ROMs put them. If
the 1801RE2 puts +5 V or ground anywhere else, the board is wrong for the job
and must not be inserted. Check this first, with a datasheet or a meter on a
board you can sacrifice. Everything else here is worthless if this is wrong.

**Signal assignment.** Which socket pin carries each of nAD0–nAD15, nSYNC, nDIN
and nRPLY. The part is DIP-24 and the signal count fits comfortably — 16 + 3 + 2
power = 21, leaving room for the enable and a couple of spares — but I could not
retrieve the pin numbering: every source that has it is outside this
environment's network egress allowlist, so the requests never left the sandbox.
That is a limitation here, not a dead end for you. In rough order of
convenience:

- **The RE-mulator manual**, `pdp-11.ru/mybk/emulator_1801RE2-1801RR1/1801pp1_manual.pdf`
  — a DIP-24 drop-in replacement has to document exactly this.
- The [RE-mulator thread](https://zx-pk.ru/threads/21519-re-mulyator-vnutriskhemnyj-emulyator-1801re2-1801rr1.html)
  on zx-pk.ru, and the [pk-fpga.ru thread](https://forum.pk-fpga.ru/viewtopic.php?f=43&t=5450).
- The UKNC schematic (RetroPC.org hosts a corrected set), which additionally
  shows the port 177054 enable wiring you need anyway.
- `oldpc.su/articles/re2/1801re2.html` and the LSI documentation at
  `archive.pdp-11.org.ru/BIBLIOTEKA/DVKTXT/LSI/`.

**Enable line and write behaviour.** Which pin carries the window enable, and
one question worth answering from the datasheet rather than guessing: **does a
real 1801RE2 assert nRPLY on a write into its window?** If it does not, writes
to ROM produce a bus timeout trap, and software may depend on that. The
BK-ROM-Disk GAL (`RPLY = CHIPSEL & (DIN # !_DOUT)`) does reply to writes, but
that is a RAM-disk controller, not a ROM. If the answer is "no", nothing needs
adding: this firmware only ever responds to nDIN.

`board_fire24e.h` refuses to compile until you fill the tables in and define
`RE2_PINOUT_CONFIRMED`. That is deliberate.

## Bring-up

Do not go straight into a host.

1. Confirm the power pins. Nothing else matters until this is settled.
2. Fill in `board_fire24e.h` and build.
3. Drive the board from a second RP2350 running a synthetic MPI master, or from
   a logic analyser plus a hand-clocked cycle. Check the address snapshot
   decodes to what you presented, and that the AD lines and nRPLY stay hi-Z for
   an address outside the configured window — that is the test that protects the
   host's bus drivers.
4. Scope the real machine before committing — on the **peripheral** processor's
   bus. Capture nSYNC, nDIN, nRPLY and a couple of AD lines during a ROM read
   with the original chip in place. That gives you the actual SYNC-to-DIN gap,
   which tells you how much slack core 1 has, and shows whether the AD lines are
   driven push-pull or open-drain. This firmware drives them push-pull during
   its response window; if the host's pull-ups are weak and something else is
   contending, that assumption needs revisiting.
5. Watch the enable pin across a port 177054 write that banks RAM into your
   window, and confirm the firmware goes quiet when it does.
6. Only then, one image, one window, in the host. Start with 206 or 207 — the
   plain system ROM windows, no banking games and no I/O page adjacency.

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
