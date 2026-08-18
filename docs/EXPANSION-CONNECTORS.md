# The MS 0511's expansion connectors

Read from two sources that do not fully agree: the redrawn four-sheet
schematic (micklab's reconstruction) and the factory documentation insert
(«Вкладыш с опечатками и схемы»), cross-checked against ukncbtl's device
models and the physical board. This is the groundwork for roadmap step 4.

What the factory placement drawing and the physical board establish:

| connector | what it is | whose bus |
|---|---|---|
| **X1, X2** | the two cartridge slots — 48-pin (2×24, counted on the machine), full multiplexed МПИ; the redrawn schematic designates them XS1/XS2 | **peripheral processor** (К1 nets) |
| **XP1** | the CPU-bus edge connector — silkscreened XP1 on the board, where the network adapter card sits | **central processor** (К2 nets) |

(The redrawn schematic's other connectors decode as: XP3/XP4 two video
outputs — RGB, sync, sound, composite; XP5 the printer port off the
КР580ВВ55А; XS5 the network/С2 line jack; XS3/XS4 the keyboard matrix
ribbons; XP6 power.)

## Two machine revisions, two cartridge interfaces

The factory documentation insert («Вкладыш с опечатками и схемы») describes
an **earlier revision** of the MS 0511. Its cartridge schematic («Устройство
запоминающее») shows a **demultiplexed** interface — data Б5–Б12 through a
К555АП6, separate address lines, a MEMR strobe, pins numbered to А32/Б32 (a
2×32 connector) — a simple ROM-style bus feeding four К573РФ6А EPROMs.

The machine this project works on is the later revision, matching the
redrawn four-sheet schematic (mc0511_05): its cartridge slots are **2×24 —
counted on the physical machine** — and carry the **raw multiplexed К1 МПИ**
per the tables below. The contradiction between the two documents is a
revision difference, not an error in either.

The revision split also offers a reconciliation for the reported claim that
the floppy controller was once fitted at the XP1 position: on the earlier
machine, a demultiplexed ROM-only slot could not host a disk controller, so
the CPU-side connector was the only home for one; the later revision's
full-МПИ slots make the PP-side controller possible, which is the
configuration ukncbtl models. Recorded as a hypothesis.

The insert's cartridge mechanicals (120 × 100 mm board, 83 mm tongue) belong
to the earlier revision as well — **measured against the real machine, the
2×24 slot accommodates a PCB only about 65 mm wide**, which is consistent
with 24 contact positions per row. So the insert is historical context only;
every dimension of a cartridge for this revision — tongue width and
thickness, contact pitch and offsets, bay depth, height clearance, keying —
must be measured from the machine and its slot. Nothing in either document
substitutes for calipers here.

## The cartridge slots: full МПИ, 48 pins

Confirmed by the finger count on the machine: the redrawn schematic's
XS1/XS2 tables are the cartridge slots (board silkscreen X1/X2). They carry
**complete PP МПИ** access, including everything a ROM never needs:

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

Unresolved: pins A11, A13, B5, and the ВВ/АА nets — none load-bearing.

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
- **The slot bus is the same multiplexed МПИ the ROM-socket firmware already
  speaks** — capture on СИА, respond on ВВОД, open-drain СИП — plus the
  write strobe, interrupts and DMA the socket never had. The existing PIO
  discipline transfers directly; what's new is the write path and the
  device-side address decode against the raw strobes instead of EDIN.
- **XP1 is a second, different opportunity**: CPU-side devices (the network
  space), the empty 160000–176777 window — and it carries the CPU's HALT
  line, which makes a hardware debugger card possible.
- The floppy-at-XP1 claim is now plausibly a property of the earlier
  revision (see above); on this machine the PP-side slot is the natural and
  modelled home.
- The physical cartridge envelope IS now specified, via prior art:
  y-salnikov's uknc_sd_fdd cartridge (EAGLE sources in its Hardware/
  directory) is **62.5 × 49.0 mm** with an edge package named
  `SNP15-48-BOARD` — 48 pads A1–A24/B1–B24 at **2.5 mm pitch**, pad
  1.5 × 4.04 mm, the field spanning 57.5 mm — matching the ~65 mm slot
  measurement and the СНП15-48 in the machine's BOM. Its board netlist also
  independently confirms this file's slot pinout on every pin the design
  uses, including **WR on B8** — writes through the slot, proven in a
  working product. (The project carries no licence file, so its sources and
  PCB are all-rights-reserved by default: dimensions and pinout are facts
  and recorded here, the design files are not to be copied without asking.)
