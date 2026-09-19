# CANARY — a RAM retention canary for the MS 0511

A small RT-11 program that turns the machine into its own long-running memory
instrument. It fills every word RT-11 will grant it with an address-derived
signature, then rescans forever; every word that changes *by itself* is logged
— address, expected and actual value, pass number, clock ticks — to a file on
the floppy, which on a kakave-equipped machine means the SD card. The log is
built to survive the failure it is watching for: the file is created,
pre-written and closed before testing starts, then rewritten block-by-block in
place, and a header-block heartbeat (pass count, uptime) is refreshed every
pass. When the machine finally freezes, the card holds the whole decay
history up to the last completed pass, including — when nothing decayed at all
— the time of death.

Built for the first machine's warm-up failure (static screen regions rotting
after minutes of play, then a freeze — see the roadmap), where the built-in
memory test passes because it runs cold and immediate.

**Status: verified end-to-end in ukncbtl** (booted FODOS 3.0, ran 36 passes,
four decays injected from the debugger were caught, logged and decoded) **and
run on the first machine**: 722 passes, 7.4 minutes, zero decays in CPU RAM —
then the screen went black and the heartbeat stopped. The fault this machine
has is not in the tested planes; see the roadmap.

## Using it

1. Put `CANARY.SAV` on a bootable RT-11/FODOS disk image
   (`rt11dsk a disk.dsk CANARY.SAV`, from
   [ukncbtl-utils](https://github.com/nzeemin/ukncbtl-utils)), or use an
   image that already carries it. Copy the image to the kakave card as
   `DISKA.DSK`.
2. Boot: menu item 1, answer FODOS's date and time prompts, press Enter at the
   startup-file prompt, then at the `.` prompt: `RUN CANARY`.
3. It prints the tested region, e.g. `TESTING 005100 - 145424`, and a status
   line `PASS n  ERRORS n  UP n S` that updates every pass (1.5 passes/s in
   emulation, 1.64 on hardware), and sends a BEL to the console each pass — if
   the firmware's terminal rings on BEL, that is an audible heartbeat that
   outlives a dead screen. Leave it running until the machine misbehaves or
   freezes. Any key stops it cleanly, and the log records that it was a key
   (v02 header word 14) rather than a death.
4. Power off, take the card to a PC:
   `python3 canary_log.py DISKA.DSK` — the decoder walks the RT-11 directory
   itself, no other tool needed. It prints every decay event, a histogram of
   which bits flipped (one dominant bit position means one DRAM chip), the
   flip direction (1→0 dominant is charge leaking away — the retention
   signature), the time of the first decay after the fill, and the address
   spread.

The heartbeat is the second instrument: a run with zero events still records
how many passes it completed and the uptime at the last one. A freeze with an
empty event log therefore means "the crash is not in the tested region" (plane
0 or the RT-11 area — see limits).

## What it tests, and what it cannot

- **Coverage**: everything `.SETTOP` grants — from just above the program to
  the bottom of the resident monitor. Under FODOS 3.0 that is 005100–145424,
  24 682 words, roughly three quarters of CPU user RAM. The RT-11 resident
  monitor (145424 up) and the low 2.5 KB are not tested.
- **Plane 0 is out of reach.** It belongs to the peripheral processor; CPU
  code cannot address it (proven in [BUILTIN-DEBUGGER.md](../../docs/BUILTIN-DEBUGGER.md)).
  The tag list, the PP's structures, the console screen all live there. A
  machine that freezes with a clean canary log is pointing at plane 0 or the
  PP side — that is the bus-master cartridge's territory.
- The test is *retention*, not a march test: it detects words that change
  while nobody writes them. Address-line and coupling faults show up too, but
  are not separated.
- It needs the machine to boot and RT-11 to run; it is for machines that die
  warm, not machines that die cold.

## Log format

RT-11 file `CANARY.LOG`, 64 blocks. Block 0 is the header, blocks 1–63 hold
events, 32 per block, 8 words each. All words little-endian.

| header word | meaning |
|---|---|
| 0–1 | magic `CA` `NA` |
| 2 | format version (1) |
| 3–4 | pass count (low, high) |
| 5–6 | decay event count (low, high) |
| 7–8 | clock ticks at the last heartbeat (**high word first**) |
| 9–10 | clock ticks at fill time |
| 11 | next free event block |
| 12–13 | tested region: low limit, exclusive high limit |
| 14 | 1 = stopped cleanly by key (v02); 0 = the run ended some other way |

| event word | meaning |
|---|---|
| 0 | pass number (low) |
| 1 | address |
| 2 | expected (address XOR 125252) |
| 3 | actual |
| 4–5 | clock ticks, high word first |
| 6 | pass number (high) |
| 7 | marker 125125 when the slot is valid |

Clock is 50 Hz. `.GTIM` gives time of day, so the decoder reports uptime as
ticks minus the fill-time ticks.

## Building

Needs a MACRO-11 assembler; [shattered/macro11](https://github.com/shattered/macro11)
(a portable C implementation) is what this was built with:

```
macro11 CANARY.MAC -o canary.obj -l canary.lst
python3 obj2sav.py canary.obj CANARY.SAV 1000
```

`obj2sav.py` turns the object file into an RT-11 `.SAV` image: it lays the
absolute text records into a memory image, writes the header words (start
address, stack, high limit) and the **memory-usage bitmap** at 360–377. Three
things about that toolchain cost real time and are worth knowing:

- **The SAV bitmap is byte-oriented, most significant bit first**: bit 7 of
  byte 360 is memory block 0 (locations 0–777), bit 6 block 1, and so on.
  Checked against DIR.SAV and DATIME.SAV from the FODOS distribution. With the
  bits in the wrong order RT-11 loads the wrong blocks — the symptoms were
  "Input error" (bits pointing past the end of the file) and a program that
  ran whatever happened to be in memory before it.
- **`.ENABL AMA` is mandatory** with this assembler for absolute-section code:
  without it every PC-relative operand is emitted with its displacement
  deferred to an RLD record the converter does not process, so the listing
  looks right and every `INC WRTBLK`-style instruction hits the wrong word.
- **`<15><12>` inside `.ASCII` is not safe here**: `<12>/text/` is parsed as
  "12 divided by text". Control characters go in separate `.BYTE` lines.

Also learned the hard way, and now handled in the source: `.TTYIN` only
returns characters immediately when JSW bit 12 (single-character mode) is set
as well as bit 6 (no wait) — with bit 6 alone, FODOS delivers input line by
line, so "any key" needed Enter; `.CLOSE` sets a new
file's length to the highest block *written*, so the log file is written end
to end before it is closed, or it would be zero blocks long; and all
directory work (`.ENTER`, `.CLOSE`, `.LOOKUP`) is done *before* `.SETTOP`,
because the USR swaps over user memory.

## The bench

Everything was tested in nzeemin's console
[ukncbtl-debugger](https://github.com/nzeemin/ukncbtl-debugger): attach the
disk image, drive the keyboard by script, run frames, OCR the screen, set CPU
breakpoints, and — after adding the `ms ADDR=VALUE` memory-write command the
help already advertised — corrupt words in the test region while the canary
runs, then read the log back with the decoder. The keyboard needs a few frames
between keystrokes (they are dropped otherwise), FODOS's startup asks for
date, time and a startup file before giving the `.` prompt, and the 64-block
pre-write takes about ten emulated seconds — inject decays after the first
`PASS` line appears, not before.
