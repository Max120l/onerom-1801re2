# The MS 0511's expansion connectors

Read from two sources that do not fully agree: the redrawn four-sheet
schematic (micklab's reconstruction) and the factory documentation insert
(«Вкладыш с опечатками и схемы»), cross-checked against ukncbtl's device
models and the physical board. This is the groundwork for roadmap step 4.

What the factory placement drawing and the physical board establish:

| connector | what it is | whose bus |
|---|---|---|
| **X1, X2** | the two cartridge slots (factory designators, on the placement drawing and the cartridge's own schematic) | **peripheral processor** |
| **XP1** | the CPU-bus edge connector — silkscreened XP1 on the board, where the network adapter card sits | **central processor** (К2 nets) |

(The redrawn schematic's other connectors decode as: XP3/XP4 two video
outputs — RGB, sync, sound, composite; XP5 the printer port off the
КР580ВВ55А; XS5 the network/С2 line jack; XS3/XS4 the keyboard matrix
ribbons; XP6 power.)

## The factory ROM cartridge, and an unresolved contradiction

The factory insert's cartridge schematic («Устройство запоминающее») shows
the cartridge interface as a **demultiplexed** bus: data Б5–Б12 (D0–D7,
buffered by a К555АП6), separate address lines А5–А19+, a MEMR strobe on
Б23, and power pins numbered up to **А32/Б32** — so a connector of at least
2×32 pins, carrying a simple ROM-style bus, feeding four К573РФ6А EPROMs.
Something on the motherboard (the КА1515ХМ1 gate arrays) demultiplexes the
PP bus for these slots.

The insert also carries the cartridge's mechanical drawing: **120 × 100 mm
board, 1.5 mm edge tongue with a keying notch at 51.25 mm, component height
10 mm max, two Ø3.5 mounting holes** — the physical envelope any
cartridge-format PicoMem must fit.

The redrawn schematic, however, contains twin 48-pin tables (designated
XS1.1/XS1.2 and XS2.1/XS2.2, rows A1–A24/B1–B24) carrying the **raw
multiplexed К1 МПИ** — АД0–15 plus А16, СИА, ВВОД **and ВЫВОД**, СИП, БАЙТ,
ТПР/ППРИ, ТПД/ППДИ, ПВ, СБРОС, ПОСТ/ПИТ, 4 МГц, all four СЕ selects, and
ИНДЕКС. An earlier revision of this file assigned those tables to the
cartridge slots; the factory cartridge schematic contradicts that — a 48-pin
connector cannot have pins numbered А32, and a demultiplexed interface is
not raw МПИ. **Which physical connector the 48-pin МПИ tables describe is
currently unresolved.** The discriminating observation is trivial at the
machine: count the fingers in a cartridge slot (2×24 vs 2×32).

The 48-pin table contents are kept below unchanged, since whatever connector
they belong to, the pinout was read carefully and К1 full-bus access exists
somewhere reachable — the floppy controller's ИНДЕКС line among the pins says
that connector is disk-related.

## The redrawn schematic's 48-pin К1-МПИ tables (connector assignment open)

Whatever they mate with physically, these tables describe **complete PP МПИ**
access, including everything a ROM never needs:

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

A card on this connector gets: all sixteen AD lines plus А16, SYNC, **DIN and
DOUT**, RPLY, WTBT, **interrupts (VIRQ/IAKO)**, **DMA (DMR/DMGO)**, SACK,
INIT, the power-monitor flags, a 4 MHz clock, all four CE selects, and the
floppy INDEX line. Reads, writes, interrupts, DMA, reset — a complete bus
citizen's interface, in contrast to the ROM sockets' banking-qualified EDIN.

Unresolved: pins A11, A13, B5, the ВВ/АА nets, and — above all — which
physical connector this is.

## XP1: the CPU-bus edge connector, complete

Confirmed against the physical board (silkscreen XP1, network card fitted).
The full table from the redrawn schematic — **60 pins**, unlisted numbers
being the interleaved grounds:

| pin | net | | pin | net | |
|---|---|---|---|---|---|
| 1 | +5V | | 27 | К2АД0 | |
| 2 | GND | | 29 | К2АД1 | |
| 3 | К2ВЫВОД | DOUT | 31 | К2АД2 | |
| 5 | К2СИП | RPLY | 32 | К2ПОСТ | DCLO |
| 7 | К2ВВОД | DIN | 33 | К2АД3 | |
| 9 | К2СИА | SYNC | 34 | К2ПИТ | ACLO |
| 10 | К2А16 | address 16 | 35–43 odd | К2АД4–АД8 | |
| 11 | К2БАЙТ | WTBT | 44 | К2ПОРТ | *(unidentified)* |
| 13 | К2ТПР | VIRQ | 45–49 odd | К2АД9–АД11 | |
| 17 | К2ППРИ | IAKO | 50 | К2ПВ | SACK |
| 18 | РЕЖ1 | MODE1 | 51–55 odd | К2АД12–АД14 | |
| 19 | К2ВУ | BS | 56 | 4608 кГц | clock |
| 23 | К2ППДИ | DMGO | 57 | К2АД15 | |
| 24 | К2ТПД | DMR | 59, 60 | +5V, GND | |
| 25 | К2СБРОС | INIT | | | |
| 26 | К2ОСТ | **HALT — the CPU's halt line, on the connector** | | | |

The network adapter lives here, its registers at CPU 176560+ — which is why
the CPU's user-space probe in [BUILTIN-DEBUGGER.md](BUILTIN-DEBUGGER.md)
found 160000–176777 hanging: that measurement was, literally, this connector
with nothing answering.

## What this settles for the MPI PicoMem (roadmap step 4)

- **The register contracts are PP-side regardless of connector**: the floppy
  at 177130 sits in ukncbtl's `CSecondMemoryController` (the PP), as does the
  hard-drive window at 110000–117777 banked by 177054 — whose slot-select bit
  chooses "hard drive in slot 1 or 2", i.e. the cartridge slots. Verified
  against class boundaries in `Memory.cpp`, not just address lists.
- **If the cartridge interface is demultiplexed** (as the factory cartridge
  schematic shows), a cartridge-format PicoMem gets a *simpler* job than the
  ROM-socket firmware — no address/data multiplexing phase at all. Whether
  the slot also carries a write strobe and I/O-page access (which the
  community hard-drive's writable registers imply) is the next thing to
  establish from the insert's remaining pages or the slot pinout itself.
- **XP1 is a second, different opportunity**: CPU-side devices (the network
  space), the empty 160000–176777 window — and it carries the CPU's HALT
  line, which makes a hardware debugger card possible.
- Reported claim, not yet reconciled: that the floppy controller was fitted
  at the XP1 position in some machines. The registers say PP; the claim says
  the CPU connector. One of the two pictures is incomplete — the insert's
  remaining pages likely settle it.
- The physical cartridge envelope is now fully specified (120×100 mm, tongue
  and notch dimensions above) — enough to draw a mechanically correct
  PicoMem card today.
