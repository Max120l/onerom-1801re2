# One ROM Fire 24: what the carrier board itself does

Findings about the board rather than the chip it emulates or the machine it
goes into. Both supported revisions are covered; where they differ, both are
given.

Build for one or the other with `-DONEROM_BOARD=FIRE24E` (the default) or
`-DONEROM_BOARD=FIRE24F`.

## The socket-to-GPIO map is identical on rev E and rev F

Every signal pin — all sixteen AD lines, nSYNC, nDIN, nRPLY, CS, the two
unconnected socket pins, and both X pads — lands on the same GPIO on both
revisions. That is why one firmware serves both, why `board.h` holds the whole
pin map rather than each revision carrying a copy, and why choosing a board
changes nothing whatsoever on the serving path.

What moves is the furniture: the status indicator and the jumper GPIOs.

## The jumper block is only half usable

**Measured on rev F hardware in the sibling MHB 2616 project, and explained by
both boards' netlists.** The same structure holds on rev E with different GPIOs
behind the same silkscreen letters.

The 2×4 header carries four jumper columns, labelled A B C D on the underside,
with 5V/GND beside them and X1/X2 below. Fitting a jumper shorts the two pads
of one column. What those pads are is *not* uniform:

| label | rev E GPIO | rev F GPIO | closed jumper ties to | also hard-wired to | usable |
|-------|-----------|-----------|----------------------|--------------------|--------|
| A | 25 | 26 | GND | — | **yes** |
| B | 24 | 27 | GND | — | **yes** |
| C | 26 | 25 | BOOT (→ R2 → QSPI_SS) | SWCLK (MCU pad 23) | **no** |
| D | 27 | 24 | RUN (10K to +3V3) | SWDIO (MCU pad 25) | **no** |

Columns C and D are the board's debug/recovery header doing double duty: their
top pads are BOOT and RUN, and their bottom pads are the select GPIOs *and* the
SWD pads. In the PCB netlist each of those two GPIOs appears on **two** MCU
pins — the GPIO itself and an SWD pin — which is what makes them unreadable as
jumpers: SWD pins carry their own internal pulls (SWCLK down, SWDIO up), and
those fight any pull the firmware applies, so `jumper_fitted()`'s "does this pin
follow our pull?" test cannot see the jumper. On hardware, C read as
permanently fitted whether or not it was.

**Consequence for this firmware.** Only A and B are clean shorts to ground, and
A is the recovery jumper, so exactly one jumper is free. This firmware reads
only A, which is all it needs — the image set is chosen when the firmware is
built. If image selection by jumper is ever wanted, note that one free jumper
carries one bit, not two.

**Do not** try to reclaim C or D by driving them or by pulling harder. D is the
RUN (reset) net and C reaches QSPI_SS through R2; both are load bearing at boot,
and neither is worth a jumper bit.

### If a jumper-selected image is ever wanted

The X1/X2 pads are a 2-pin header of their own (GPIO 9 and 8) and both are
ordinary GPIOs with nothing else on them. A jumper across them can be detected
*actively* rather than by pulls: drive X1 low and read X2, drive X1 high and
read X2; X2 follows only if the jumper is fitted. That is robust in a way the
pull test is not. Untested here; recorded so it is not rediscovered from
scratch. See [ROADMAP.md](ROADMAP.md), where the X pads are also wanted as an
image selector.

## The rev F schematic's net names are rotated

On rev F the schematic's `SEL_A`…`SEL_D` nets do **not** correspond to the
silkscreen letters: net `SEL_A` lands on the pad labelled **D**, `SEL_B` on
**C**, `SEL_C` on **A**, `SEL_D` on **B**. Rev E's net names line up in a
different order again. Identify jumpers from the silkscreen and the PCB
netlist; never from the net names.

## The rev F indicator is a WS2812B

GPIO 29, a single XL-1010RGBC-WS2812B, where rev E has a plain LED on the same
GPIO with its anode through a 1K to +3V3 — so rev E lights when the pin is
driven **low**, and rev F does not care about pin levels at all. It needs the
800 kHz serial protocol, so a `gpio_put` does nothing to it.

This firmware drives it from a small PIO program on **pio1**. Serving owns pio0
— both state machines and its instruction memory — and the two blocks share no
state, so the one part of this firmware with a hard bus deadline cannot be
perturbed by the one part that is purely cosmetic.

Everything that reports through the light goes through `status.h`, so neither
the serving loop nor a line of `diag.c` knows which board it is on. The colours
are listed in [DIAGNOSTICS.md](DIAGNOSTICS.md#the-status-light).

Some WS2812 clones want RGB order rather than GRB. If the colours come out
permuted, that is the first thing to check, and `STATUS_GRB` in `status.h` is
the only thing to change.

## Recovering a board you have flashed

This firmware carries no USB stack. Flashing it replaces One ROM's picoboot, so
the board stops appearing in One ROM Web entirely, and the Fire 24 has no
BOOTSEL button. Three routes back, in the order worth trying:

1. **Jumper A**, on either revision — fit it and power on. Checked first thing
   in `main()`, before a single socket pin is touched, so it still works when
   the rest of the firmware does not. It does depend on this firmware booting
   at all, which is why the other two exist beneath it.
2. **BOOT to GND**, which needs no working firmware. On the jumper block that
   is column C's top pad; on header J2 it is pin 5, with ground on 2 or 4.
   Short it and power up, and the bootrom's mass-storage volume mounts, taking
   a `.uf2` by drag and drop. If the board is already powered, briefly ground
   RUN (J2 pin 7, or column D's top pad) instead of power-cycling.
3. **SWD**, the real safety net, since it works regardless of what is on the
   flash. SWCLK and SWDIO are J2 pins 6 and 8, ground on 2 or 4 — and note
   they are the same nets as jumper columns C and D, so leave those jumpers off
   while programming.

J2 is a footprint rather than a fitted header on some boards, which is exactly
when route 1 earns its place.
