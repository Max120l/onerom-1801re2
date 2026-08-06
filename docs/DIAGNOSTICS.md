# Diagnostics through the ROM socket

A ROM emulator sits on the bus, sees every cycle, and is the only device in the
machine that is guaranteed to be working — you flashed it yourself. That makes
the ROM socket a diagnostic port, and this file describes the instruments built
on it.

None of this is needed to use the board as a ROM. Every option here is off by
default and compiles to nothing: with `MPI_WATCH` off the diagnostics contribute
zero bytes, and the resulting binary is byte-identical to one built from a tree
with `diag.c` deleted. A board left in a machine should be a ROM and nothing
else.

For how these instruments were used and what they found on one particular
machine, see [INVESTIGATION.md](INVESTIGATION.md). For the idea taken further —
a general-purpose diagnostic suite for other machines — see
[DIAGNOSTICS-AS-A-ROM.md](DIAGNOSTICS-AS-A-ROM.md).

## Two kinds of instrument

**Watch the stock ROM run.** Keep the machine's real ROM image and score
addresses as they go past on the bus. This observes without changing anything,
so it can answer "how far did the monitor get" on a machine whose screen shows
nothing. Its limit is that it can only ask questions the stock ROM's own control
flow happens to answer.

**Replace the stock ROM.** Serve a purpose-built image instead, so the machine
runs code you wrote. This asks anything you like, and it runs on a machine far
too broken to boot — but the machine no longer does its normal job while it
runs.

Both report through the same one-bit channel: the board's status LED.

## Reading the LED

Everything reports as a **frame**, repeating for as long as the board is
powered:

| | |
|---|---|
| **2.5 s lit** | the frame marker — the start |
| 800 ms dark | |
| then, one pulse per item, in table order: | |
| **700 ms lit** | that item is set |
| **100 ms lit** | that item is clear |
| 300 ms dark between pulses, **900 ms after every fifth** | |

Three details are each there because their absence caused a misreading on
hardware, and all three are worth understanding before trusting a frame:

- **The marker is lit, not dark.** It was originally a long dark gap, and once
  the pulses were grouped — also with dark gaps — the frame contained three
  different lengths of darkness and no way to tell which one began it. Nothing
  else in a frame is lit for longer than 700 ms, so a solid two and a half
  seconds is unmistakable.
- **Pulses are grouped in fives.** A thirty-pulse frame at a uniform cadence is
  not readable by a human: you must hold a running count for half a minute, and
  one miscount silently renames every pulse after it. This is not hypothetical —
  a frame was read as 22/24/26 when it was actually 22/23/24/26, and the two
  decode to different faults. "Group four, pulse two" needs no running count and
  survives looking away.
- **Every item gets a pulse**, hit or miss. Any scheme that blinks only the hits
  cannot be counted, because a position with nothing in it is indistinguishable
  from the end of the frame.

A phone camera in slow motion is a legitimate way to read a long frame, and was
used for the real ones.

Frames are slow — twenty pulses is roughly twenty seconds — which matters when
choosing between the latching and clearing modes below.

## The build matrix

All options are `cmake -S firmware -B <dir> -G Ninja -D<OPTION>=ON`. Implications
are applied by CMake before anything is defined, so turning on a dependent option
turns on what it needs; the build prints which.

| Option | |
|---|---|
| *(none)* | plain ROM emulation. No diagnostics compiled at all. |
| `MPI_WATCH` | score watchpoints and bus facts from the **stock** ROM, blink the frame |
| `MPI_BEACONS` | blink a **test ROM's** beacons instead of watchpoints (implies `MPI_WATCH`) |
| `MPI_BEACON_PASS` | clear the frame each pass, so it shows the *current* pass (implies `MPI_BEACONS`) |
| `MPI_BEACON_LIVE` | reduce the whole pass to a single lamp: pass or fail, now (implies `MPI_BEACONS`) |
| `MPI_SOAK_DIGIT` | the soak's verdict as one to four flashes rather than a long frame |
| `MPI_SOAK_P0BITS` | eight pulses naming plane 0's failing bits, latched from the first pass that failed alone |
| `MPI_BEACON_KILLADDR` | sixteen pulses: the address of the cycle immediately before the PP last restarted |
| `MPI_BEACON_LASTADDR` | sixteen pulses: the last address seen on the bus, whether or not anything restarted |
| `MPI_IGNORE_CS` | serve every window regardless of socket CS — isolates whether honouring CS costs reads |
| `MPI_RPLY_ASSIST` | briefly drive nRPLY high on release instead of leaving the rise to the bus |

Three cache variables tune the beacon frame to the test ROM being flashed. They
are **not** optional bookkeeping: get them wrong and the frame is misread rather
than obviously broken.

| Variable | | default |
|---|---|---|
| `MPI_BEACON_COUNT` | how many pulses the frame has | 18 |
| `MPI_BEACON_DONE` | which beacon ends a pass — only used by the clearing modes | 6 |
| `MPI_BEACON_OK_MASK` | bitmask of beacons that are *not* failures, for the lamp | soak's |
| `MPI_SYS_CLK_KHZ` | system clock | 150000 |

Each test ROM prints its own values when it builds the image; the per-ROM
sections below repeat them.

### Latching, clearing, and which to use

The same beacons can be presented three ways, and the choice is about what
question you are asking rather than taste.

- **Latching** (`MPI_BEACONS` alone) — nothing is ever cleared, so the frame is
  the union of everything since power-on. Right for "did this *ever* happen".
  Wrong for freeze spray, where it can only ever say "it failed at some point"
  and never recovers to say the cooling worked.
- **Per pass** (`MPI_BEACON_PASS`) — cleared at the start of each pass, so the
  frame describes the pass now running. Right for a test that can wedge partway
  through: the frame freezes showing exactly the phases it reached, which a
  latching frame cannot say and a lamp has no room to.
- **Lamp** (`MPI_BEACON_LIVE`) — the pass reduced to pass/fail, updated every
  pass. Right for freeze spray, where you need the answer to track the can in
  your hand rather than a frame from twenty seconds ago.

One sampling bug here is worth knowing, because it produced a convincing wrong
answer: the frame was sampling the live pass counter once per frame while passes
complete in seconds, so a perfectly healthy machine looked wedged. The clearing
modes now show the last *completed* pass while the next one runs.

## Watching the stock ROM

```console
$ cmake -S firmware -B firmware/build-watch -G Ninja -DMPI_WATCH=ON
$ ninja -C firmware/build-watch
```

This serves the bus exactly as the normal build does — the scoring happens after
the reply has been queued, so a watch build cannot introduce the timing fault it
might be used to rule out — and repurposes the status LED to report which of a
handful of monitor addresses the machine has executed.

| pulse | | a long pulse means |
| --- | --- | --- |
| 1 | 160302 after 160300 | the monitor started from our vector — if this is short, stop, nothing else means anything |
| 2 | 160450 after 160446 | a ROM block failed its checksum |
| 3 | 160342 after 160340 | the PP called the routine that loads the central processor's memory |
| 4 | 160374 after 160372 | **the PP released the central processor** — the ACLO edge that starts it |
| 5 | 101006 after 101004 | the boot menu header was printed |
| 6 | 174170 after 174172 | the PP is scanning its idle task queue — alive and dispatching |
| 7 | *(bus)* | a reply was prepared and the host never took it |
| 8 | *(bus)* | the bus carried something other than what we drove |
| 9 | *(bus)* | the first cycle of this boot was the power-up vector fetch — we won the startup race |
| 10 | *(bus)* | **the bus went quiet** — a whole second with no cycle served |
| 11 | *(bus)* | all four windows fully covered: every word we serve was asked for |
| 12–13 | *(bus)* | a 2-bit number, most significant first, naming the block whose checksum failed — **meaningless unless pulse 2 is lit** |

Pulses 6 and 10 are a pair, and they exist for the frozen screen: a boot menu
drawn with no blinking cursor and no response to the keyboard. The cursor blink
and the keyboard scan are both tasks on the PP's dispatcher, so a static menu
means the PP is not dispatching — and these say which kind:

| 6 | 10 | |
| --- | --- | --- |
| long | short | the PP is alive and scanning, but no task ever becomes ready — interrupts or the timer |
| short | long | the PP has stopped fetching altogether — a halt, a trap, or a reply that never came |
| short | short | it left the dispatcher and is running somewhere else — a wild jump |

```
00 = 205, 100000-117777      10 = 207, 140000-157777
01 = 206, 120000-137777      11 = 208, 160000-176777
```

The loop body is the same code for all four blocks, so no fetch address
distinguishes them. The data does: `cmp 176766(r5), r3` at 160442 reads its
stored sum from 176776, 176774, 176772 and 176770 as r5 walks 8, 6, 4, 2, and
that read lands directly behind the fetch of the instruction's second word at
160444 — a pair, so the summing loop cannot forge it while reading those same
words as data on its way down.

**A frame describes one boot.** Hits were originally never cleared, which made a
frame the union of every boot since the board was powered — and that ambiguity
bit immediately, when a boot printed a CPU error with no ROM error while the
checksum pulse was still lit from an earlier attempt. The frame now clears when
the PP takes PC and PSW from its power-up vector, detected as the pair 160002
directly behind 160000 so the checksum cannot forge it by reading those same two
words on its way down.

That clearing was got wrong once, in a way worth recording. It was first done on
core 0, at the top of the next frame — which is up to ten seconds after the
restart, by which time the machine has finished booting. The wipe therefore
landed squarely on the startup evidence it existed to isolate, and the frame
came back with the sanity pulse *short* while the "we saw this boot begin" pulse
was long: a combination that cannot happen on a machine that is booting at all,
since it says the monitor never ran while we watched it start. The reset now
happens on core 1, two cycles into the new boot, and coverage is a generation
tag rather than a bitmap precisely so that clearing it costs one increment
instead of a 4 KB memset between two bus cycles.

The frame has been trimmed as questions closed. The PP RAM test, the error
printer and the end of the startup test answered consistently — RAM clean, no
error line, startup completes — and every pulse spent re-confirming a settled
fact is one the reader has to count past. The CS pulse went with them: it never
lit, so no read was ever declined, and `-DMPI_IGNORE_CS=ON` is moot.

Pulses 6–9 exist because the instrument had a blind spot the size of the
remaining question. With "we declined a read" and "a reply went untaken" both
negative, we answer every read we are asked and the host takes every answer, and
the machine *still* computes a bad checksum. Either the data is corrupted
electrically between our pins and the processor, or some reads never reached us
— and the second is invisible to any counter of cycles we saw, because the read
strobe is EDIN and the CGM withholds it for a window banked elsewhere.

The startup checksum reads every word of every window exactly once, so one bit
per word settles it: all four windows covered means every word came from us and
the corruption is electrical; a window short of its count means its reads went
somewhere else, and that window is the failing block.

### Why each watchpoint is a pair

The first version of this scored a single address, and on hardware it reported
every watchpoint as hit. That was not a finding, it was a bug, and the reason is
worth keeping: **the bus does not distinguish an instruction fetch from a data
read**, and the startup test checksums all four ROMs — it reads 16127 of the
16128 words, every watchpoint among them, within moments of power-on. "This
address was read" is true of the entire ROM.

What separates a fetch from a data read is the company it keeps. Straight-line
execution reads consecutive words back to back; the checksum walks *downwards*
and puts three fetches of its own loop body between every pair of data reads, so
a data read of A is followed by 160434, never by A+2. The plane-copy loop at
173270 ascends but does the same. So a watchpoint is two addresses — the one to
score and the one that must have arrived immediately before it — and picking the
second word of a multi-word instruction makes the predecessor its own first
word, which holds whether the instruction was reached by fall-through or branch.

`test/test_watchpoints.py` checks both halves against the ROM image: that each
pair is really adjacent in the code, that the disassembly at the predecessor is
what we think it is, and that neither ROM-scanning loop can forge the adjacency.
It also reports what the old rule would have done, which is how the bug is kept
fixed:

```console
$ python3 test/test_watchpoints.py uknc_rom.bin
  160300 -> 160302  mov @#172660, r4         2 words  ok
  160446 -> 160450  beq 160452               1 word   ok
      (single word, falls through with no bus cycle)
  ...
no ROM-reading loop forges these adjacencies:
  checksum (descending)    clear
  plane copy (ascending)   clear

under the old address-only rule the checksum alone would light 6 of 6 watchpoints
```

The pairing can only produce false negatives: another master interleaving a
cycle, or a write landing between the two fetches, breaks the pair and loses a
hit. A short pulse means "not seen", not "did not happen".

Edit `firmware/watch.h` to watch something else; the table is the interface.

## Replacing the stock ROM: the test ROMs

`tools/diag/make_*.py` each build a complete 32256-byte ROM image that replaces
the system monitor. Every one of them starts by writing `mov #40, @#177716` —
holding the **central processor** in DCLO reset — so that nothing but the PP is
touching what is being measured.

### The beacon protocol

A test ROM has no console, no serial port and no working machine to print
through. What it does have is a bus that the board is already watching, so it
reports by **reading addresses that mean nothing**: `tst (r1)` on a reserved
address is one bus cycle, harmless, and the board sees it go past.

Beacons live at **176700** and up, one word apart — inside the last window, past
the end of the program and past anything it touches. In ROM rather than in RAM,
and that is not cosmetic: a beacon in RAM relies on the capture machine latching
cycles for addresses we do not serve, which is true only if the socket's SYN is
the raw bus strobe and has never actually been demonstrated. In ROM we answer
the read ourselves, so we cannot fail to see it.

Beacon *n* is pulse *n+1* in the frame.

### Unused ROM is filled with NOPs

Every image fills its unused space with `000240` (NOP), and that is a
correction rather than a detail. It was originally filled with zeros — and a
zero word is `HALT`. Any stray jump into 32 KB of ROM landed in a field of halts
and the machine stopped dead, while the stock ROM it replaces has real code at
every address and would have carried on. The instrument was making excursions
fatal that the real machine survives, and then reporting the death as the
machine's.

A NOP fill turns the image into a slide: land anywhere below the program and
execution walks up into it and re-enters the test. Each ROM also plants a
**landing beacon** just before its entry point, which the power-up vector jumps
over — so that beacon means "arrived by falling through ROM", which no healthy
start can produce. A derail becomes a countable event instead of the end of the
run.

### `make_testrom.py` — is the PP's own RAM sound?

The first and smallest of them, and the one that established the technique.
Writes and reads back the PP's own RAM with the CPU held off.

```console
$ ./tools/diag/make_testrom.py -o testrom.bin
$ ./tools/rom/gen_rom_images.py --raw testrom.bin -o firmware/rom_images.c
```

Beacons: `0` alive, `1` RAM ok, `2` RAM bad, `3` done.

**Its beacons are at 077700, in PP RAM, not at 176700 in ROM** — it predates the
move into ROM described above. `PP_BEACON_BASE` is compiled in at 176700 with no
CMake override, so `-DMPI_BEACONS=ON` will *not* blink this ROM's beacons; edit
`firmware/watch.h` if you want them on the LED. As it stands it is run in
ukncbtl, which costs nothing to be wrong in, and by `test/test_testrom.py`
against a model of the PP. The three later ROMs all put their beacons in ROM and
work with the LED directly.

### `make_ramtest.py` — the central processor's RAM, from the other side

The machine's own startup test reports "ОШИБКА ОЗУ ЦП", central processor RAM
error — but it runs that check *on* the central processor, using code copied
into the RAM under test. A fault there corrupts the tester before it can report
on the testee.

This runs the same test from the PP, through the plane ports, with the CPU held
in reset:

```
177010  plane address register -- a byte index, latches all three planes
177012  plane 0 data (byte)     -- the PP's own RAM / video plane 0
177014  plane 1 & 2 data (word) -- low byte plane 1, high byte plane 2
```

The CPU's RAM *is* planes 1 and 2, so one word written to 177014 tests both at
one address and the two halves of what comes back name which plane failed.

```console
$ ./tools/diag/make_ramtest.py -o ramtest.bin
$ ./tools/rom/gen_rom_images.py --logical ramtest.bin -o firmware/rom_images.c
$ cmake -S firmware -B firmware/build-soak -G Ninja -DMPI_BEACONS=ON
```

Beacons: `0` alive, `1` PP RAM ok, `2` PP RAM bad, `3` planes ok, `4` plane 1
bad, `5` plane 2 bad, `6` done, `7` stuck, `8`–`15` failing bit 0–7, `16` every
bit at once, `17` the landing beacon. The `MPI_BEACON_COUNT=18` and
`MPI_BEACON_DONE=6` defaults are this ROM's, so it needs no overriding. It also
leaves a status word at 077660 and the bits ever wrong at 077662, readable from
a monitor.

`--live` moves the accumulator clears inside the loop, so each pass reports only
itself. Pair it with `-DMPI_BEACON_LIVE=ON` for the freeze-spray lamp:

```console
$ ./tools/diag/make_ramtest.py --live -o ramsoak-live.bin
```

### `make_addrtest.py` — which *address* bits does it get wrong?

"Which data bit failed" is the right question when one 1-bit-wide DRAM has died:
each bit is one chip, so the answer is a part number. It is the wrong question
when every bit of every plane fails at once — nothing true of eight independent
chips in two banks is true of all of them simultaneously. What they share is the
address bus and the strobes.

So three passes: fill with 0, fill with 177777, fill each cell with its own
index. The first two are **blind to addressing** — every cell holds the same
value, so a read landing on the wrong cell still returns the right answer, and
they can only report a data-line fault. The third is sensitive to both, and
carries more than pass/fail: since each cell holds its own address, whatever
comes back *is* the address of the cell that actually got selected. XOR it with
the address asked for and the difference names the bits that went wrong.

Bits the constant passes implicated are removed from the address report, because
a stuck data bit shows up in the address pass too and would otherwise be read as
an address line.

```console
$ ./tools/diag/make_addrtest.py -o addrtest.bin
$ ./tools/rom/gen_rom_images.py --logical addrtest.bin -o firmware/rom_images.c
$ cmake -S firmware -B firmware/build-addr -G Ninja \
      -DMPI_BEACONS=ON -DMPI_BEACON_COUNT=27 -DMPI_BEACON_DONE=1
```

Beacons: `0` alive, `1` done, `2` data lines bad, `3` addressing bad, `4` plane
address register bad, `5`–`19` address bit 0–14, `20`–`26` phase reached
(register, fill 0, check 0, fill 1s, check 1s, fill addr, check addr). The phase
markers are what make a wedged pass readable: the frame freezes showing exactly
how far it got. `--stop-on-fail` holds the first failure rather than continuing.

### `make_regtest.py` — registers only, no memory at all

Every other test here reaches its question through memory, which entangles the
answer with the DRAM, its address multiplexer and its strobes. This one writes
patterns to the plane address register and reads them straight back. No cell is
ever addressed, nothing is stored, and a whole pass is a few dozen instructions
— so passes complete thousands of times a second and the LED reflects the
machine's state *now* rather than ten seconds ago.

The patterns separate two faults:

| | |
|---|---|
| `000000`, `177777` | every bit the same — blind to interaction *between* lines, can only find a bit stuck high or low |
| `125252`, `052525` | every bit the opposite of both neighbours — maximal stress on adjacent-line coupling, and the only patterns here that can see it |

Both halves of each pair, because a coupling fault is not symmetric: a line
dragged toward its neighbour shows only when the two disagree in one particular
direction, and testing `125252` alone would find half of them.

```console
$ ./tools/diag/make_regtest.py -o regtest.bin
$ ./tools/rom/gen_rom_images.py --logical regtest.bin -o firmware/rom_images.c
$ cmake -S firmware -B firmware/build-reg -G Ninja \
      -DMPI_BEACON_PASS=ON -DMPI_BEACON_COUNT=21 -DMPI_BEACON_DONE=1
```

Beacons: `0` alive, `1` pass finished, `2` alternating patterns failed (lines
interfering), `3` constant patterns failed (a bit stuck), `4`–`19` bit 0–15,
`20` the landing beacon.

## Testing the board itself

Distinct from all of the above, which test the *machine*. The self-test build
fills all eight windows with a pattern whose every word encodes both its own
index and the window it belongs to, so a dump taken through a ROM reader can be
diagnosed rather than merely failed. See
[Testing against a ROM reader](../README.md#testing-against-a-rom-reader).

## Where the code lives

| | |
|---|---|
| `firmware/diag.c` | the whole instrument: scoring, beacons, and every LED frame |
| `firmware/diag.h` | what the emulator calls — empty inlines when `MPI_WATCH` is off |
| `firmware/watch.h` | the watchpoint and beacon tables; the table is the interface |
| `tools/diag/make_*.py` | the test ROMs |
| `tools/diag/selftest.py`, `check_selftest.py` | the board's own wiring self-test |
| `test/test_*test.py` | each test ROM run against a model of the PP |
| `test/test_watchpoints.py` | the watchpoint pairs checked against the real ROM image |

`firmware/main.c` calls into `diag.h` at four points in the serving loop and
once at startup, and knows nothing else about any of this. With `MPI_WATCH` off
all five are empty inlines, which is what keeps the plain build identical.
