# The MS 0511's expansion connectors, from the schematic

Read from the machine's schematic (the four-sheet PDF, sheets 1 and 3; this
project had previously only used sheet 1's ROM-socket region), cross-checked
against ukncbtl's device models for what lives at which addresses. This is the
groundwork for roadmap step 4, the MPI PicoMem.

The machine has two different expansion worlds, on two different processors'
buses, and the naming matters:

| connector | what it is | whose bus |
|---|---|---|
| **XS1, XS2** | the two cartridge slots, 48-pin (СНП15-48), rows A1–A24 / B1–B24 | **peripheral processor** (К1 nets) |
| **XP1** | the internal expansion connector — where the network adapter lives (board silkscreen reads ХР1, easily misread as KP1) | **central processor** (К2 nets) |

(For completeness, the other connectors decode as: XP3/XP4 two video outputs —
RGB, sync, sound, composite; XP5 the printer port off the КР580ВВ55А; XS5 the
network/С2 line jack; XS3/XS4 the keyboard matrix ribbons; XP6 power. The
schematic's X1/X2 are the CPU crystal's terminals, not connectors.)

## The cartridge slots are full bus slots, not ROM sockets

This is the finding that changes the project's assumptions. XS1/XS2 carry the
**complete PP МПИ**, including everything a ROM never needs:

**Row A** (Soviet name → Q-bus equivalent where they differ):

| pin | net | |
|---|---|---|
| A1 | +5V | |
| A2 | К1АД15 | AD15 |
| A3 | К1А16 | 17th address bit |
| A4 | 4000 кГц | 4 MHz clock |
| A5 | К1ППРИ | **IAKO — interrupt grant** (daisy chain) |
| A6 | К1ВУ | BS — I/O page select |
| A7 | К1ПВ | SACK |
| A8 | К1БАЙТ | WTBT — write/byte |
| A9 | К1ПОСТ | DCLO — DC power ok |
| A10 | К1ТПР | **VIRQ — interrupt request** |
| A11 | *(not identified)* | |
| A12 | СЕ3 | cartridge select 3 |
| A13 | *(not identified)* | |
| A14 | К1ВВОД | DIN — read strobe (raw, not the CGM's EDIN) |
| A15 | К1СИА | SYNC |
| A16–A22 | АД5, АД7, АД9, АД11, АД3, АД1, АД13 | odd AD lines |
| A23 | ВВ | *(net "BB" — purpose not yet identified)* |
| A24 | +5V | |

**Row B:**

| pin | net | |
|---|---|---|
| B1 | GND | |
| B2 | АА | *(net "AA" — purpose not yet identified)* |
| B3 | 40/80 | display-width strap |
| B4 | РЕЖ2 | MODE2 |
| B5 | *(not identified)* | |
| B6 | К1ТПД | **DMR — DMA request** |
| B7 | К1ППДИ | **DMGO — DMA grant** |
| B8 | К1ВЫВОД | **DOUT — write strobe** |
| B9 | К1ПИТ | ACLO — AC power fail |
| B10 | СБРОС | INIT — bus reset |
| B11 | ИНДЕКС | INDEX (floppy index line, pre-routed to the slot) |
| B12 | СЕ0 | cartridge select 0 (the DS4/100000-window select) |
| B13 | СЕ2 | cartridge select 2 |
| B14 | СЕ1 | cartridge select 1 |
| B15 | К1СИП | RPLY |
| B16–B23 | АД4, АД6, АД8, АД10, АД2, АД0, АД12, АД14 | even AD lines |
| B24 | GND | |

So a slot card gets: all sixteen AD lines plus А16, SYNC, **DIN and DOUT**,
RPLY, WTBT, **interrupts (VIRQ/IAKO)**, **DMA (DMR/DMGO)**, SACK, INIT, the
power-monitor flags, a 4 MHz clock, and **all four CE selects**. Reads,
writes, interrupts, DMA, reset — a complete bus citizen's interface. The
floppy's INDEX line is even pre-routed. This is why the floppy controller and
the community hard-drive interface could be slot devices with registers in the
I/O page: the slot sees every PP bus cycle.

Note the strobe difference from the ROM sockets: the sockets get **EDIN**, the
CGM's banking-qualified read strobe; the slots get the **raw К1ВВОД** plus the
CE lines to do their own qualification. A slot card decodes what it pleases.

Unresolved pins (A11, A13, B5, and the ВВ/АА nets) need either a zoomed read
of the drawing around the slot symbols or a continuity check against a real
machine; none of them is load-bearing for the design.

## XP1: the CPU-side expansion connector

The К2 nets — the central processor's МПИ. Control pins identified so far
(the AD0–15 block continues in a table section not yet transcribed):

| pin | net | |
|---|---|---|
| 1 | +5V | |
| 2 | GND | |
| 3 | К2ВЫВОД (DOUT2) | write strobe |
| 5 | К2СИП (K2RPLY) | reply |
| 7 | К2ВВОД (DIN2) | read strobe |
| 9 | К2СИА (SYNC2) | sync |
| 10 | К2А16 | 17th address bit |
| 11 | К2БАЙТ (WTBT2) | write/byte |
| 13 | К2ТПР (K2VIRQ) | interrupt request |
| 17 | К2ППРИ (K2IAKO) | interrupt grant |
| 18 | РЕЖ1 (MODE1) | |
| 19 | К2ВУ (K2BS) | I/O page select |
| 23 | К2ППДИ (K2DMGO) | DMA grant |
| 24 | К2ТПД (K2DMR) | DMA request |

The network adapter lives here, its registers at CPU 176560+ — which is why
the CPU's user-space probe in [BUILTIN-DEBUGGER.md](BUILTIN-DEBUGGER.md)
found 160000–176777 hanging: that measurement was, literally, this connector
with nothing answering.

## What this settles for the MPI PicoMem (roadmap step 4)

- **Disk emulation belongs in a cartridge slot (XS1/XS2).** The contracts to
  satisfy are PP-side: the floppy controller at 177130 (ukncbtl
  `emubase/Floppy.cpp`) and the hard-drive interface at 110000–117777 banked
  by port 177054 (`emubase/Hard.cpp`). The slot carries everything needed,
  interrupts and DMA included, and history agrees — that is where the real
  controllers plugged in.
- **The CPU-side connector (XP1) is a different, second opportunity**: network
  emulation, a CPU-side device, or an answer for the empty 160000–176777
  window. Not the disk project.
- The connector to source or adapt: **СНП15-48**, 48 pins, two rows.
- The RP2350 needs more pins than the ROM-socket board: 17 address/data + 2
  strobes + RPLY + WTBT + SYNC + 4 CE + VIRQ/IAKO + DMR/DMGO + INIT ≈ 30
  signals minimum — RP2350B territory, or an RP2350A plus registered
  transceivers.
