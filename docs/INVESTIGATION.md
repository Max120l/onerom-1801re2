# Repairing an MS 0511 through the ROM socket

The board in this repository was built to replace four mask ROMs. The machine it
went into turned out to be faulty, and because the board sits on the MPI bus it
was already in the right place to say why — so the ROM emulator grew a second
career as an instrument, and the fault was found through the ROM socket and
nothing else.

This file is the record of that: what each measurement said, which conclusions
were wrong, and how they were corrected. It is kept separate from the README
because none of it is needed to use the board, and separate from
[DIAGNOSTICS.md](DIAGNOSTICS.md) because that describes the instruments as they
finished up rather than how they were arrived at.

**The answer, if you only want the answer:** the fault is **D22, a КР1801ВП1-055
bus transceiver**, which fails after about ten minutes from cold and works again
within seconds of being cooled. See
[The thermal fault](#the-thermal-fault-d22-and-why-everything-else-measured-clean)
and [docs/D22-KR1801VP1-055.md](D22-KR1801VP1-055.md).

Two habits are worth taking from this even if the machine is of no interest.
Every watchpoint is **a pair** of addresses rather than one, because a bus does
not distinguish an instruction fetch from a data read. And every conclusion
below that had to be withdrawn was withdrawn for the same reason: a single-shot
capture of a non-repetitive signal was read as though it were representative.

---

## Open: the boot menu

An MS 0511 running this ROM set should show a boot menu — ЗАГРУЗКА, with disk,
ROM cartridge, network, C2, tape, debug and **тестирование** as options. The
emulator draws it from the identical 32 KB image, so the menu is unquestionably
in the ROM the board is serving. A machine that reaches a cursor and answers УСТ
but never shows the menu is therefore taking a different branch, not missing
code.

### What the monitor actually does at power-up

Disassembled with `tools/pdp11dis.py`, the entry at 160300 reads:

```
160300  013704 172660         mov @#172660, r4     ; 172660 is in ROM: 000450
160304  005000                clr r0
160306  010406                mov r4, sp
160310  100465                bmi 160464           ; warm-start check
160312  032737 000020 177716  bit #20, @#177716
160320  001404                beq 160332           ; bit 4 clear -> cold start
160322  013700 000000         mov @#0, r0          ; bit 4 set: restart vector
160326  001401                beq 160332
160330  000110                jmp (r0)
160332  012737 000040 177716  mov #40, @#177716    ; hold the CPU in reset
160340  004767 012706         jsr pc, 173252       ; load the CPU's planes
160344  012737 070045 177010  mov #70045, @#177010
160352  016437 000042 177014  mov 42(r4), @#177014
160360  005037 177716         clr @#177716
160364  012700 000100         mov #100, r0
160370  077001                sob r0, 160370       ; settle
160372  012737 100000 177716  mov #100000, @#177716 ; release the CPU
160400  004767 000004         jsr pc, 160410       ; checksum all four ROMs
```

Two things in the previous version of this section were wrong and are corrected
here. `100465` is **bmi**, not bpl. And bit 4 of 177716 is not a strap: writing
177716 drives the central processor's control lines — bit 4 is **HALT**, bit 5
DCLO, bit 15 ACLO — which is exactly what the sequence above is doing when it
writes 40, then 0, then 100000. The bit-4 branch is a warm-restart hook that
jumps through location 0 if one is set; it has nothing to do with the menu, and
the guess that it explained the missing menu was wrong.

### What the machine said

The first frame read back from hardware was:

| pulse | | reading |
| --- | --- | --- |
| 1 | **long** | the monitor started from our vector — the instrument is valid |
| 2 | **long** | **a ROM block failed its checksum** |
| 3 | short | the PP RAM test found no fault |
| 4 | short | the error-printing routine was not entered |
| 5 | **long** | the startup test ran to completion |
| 6 | short | the boot menu header was never printed |

Pulse 2 is the one that matters, and it reverses the conclusion below. The
images pass that same checksum offline — byte-identical to the reference, all
four blocks verified — so the machine reading a bad block means **it is not
receiving what the board holds**. That is a board-side fault, and it is not
confined to the checksum: a processor fed a wrong word executes it. The missing
menu may be downstream of this rather than a separate question.

Pulses 2 and 4 together are informative. A checksum failure leaves a bit set in
the error mask, and the monitor at 172732 branches to the printing routine when
that mask is non-zero — so pulse 4 should have been long too. The path that
skips it is the `bhi` at 172726, taken when the byte the central processor sent
over channel 0 (port 177060) is greater than 2. So the CPU is reporting
something as well. Pulses can only be lost, never invented, so this is a lead
rather than a proof.

There are exactly two ways our correct data becomes the processor's wrong sum:
reads we decline to answer, and replies we assemble that are never taken. The
watch build now scores both as pulses 7 and 8, and `-DMPI_IGNORE_CS=ON` builds
a twin that answers every window unconditionally — two firmwares differing in
one variable, which is what turns this from an argument into a measurement.

### The reply line

A scope on socket pin 2 in the running machine shows nRPLY doing something a
reply line should not: sharp falls, then an **RC ramp** taking on the order of a
microsecond to climb back, and at the cycle rate the machine actually runs at
(the capture reads ~2.5 µs between cycles) several ramps are cut off by the next
assertion before they arrive anywhere near a logic high. Peak is 4.24 V, so the
level is fine — the *edge* is not.

That is the signature of a line released to hi-Z against a weak pull-up and a
few hundred pF, which is exactly what the firmware does: nRPLY is open drain by
construction, driven low to assert and released to let the bus pull it up. Open
drain is the right model for an MPI reply line, but it assumes the bus can
restore the line quickly, and here it plainly cannot.

The consequence fits the fault. **A reply line that has not finished coming back
up is indistinguishable from one still asserted.** A host reading it that way
takes a reply for a cycle nobody has driven yet and samples the AD lines early —
data that is right in the board and wrong in the processor, intermittently,
which is what the checksum reported.

`-DMPI_RPLY_ASSIST=ON` selects the `mpi_respond_assist` program, which drives
nRPLY high with the pad's own 8 mA for ~320 ns after the host takes the data
before going back to hi-Z. The window is bounded on purpose: nRPLY is shared,
every slave asserts it for its own cycles, and the drive has to be gone before
the next one could. From the observed ramp the line looks like a few hundred pF,
which 8 mA slews in 100–200 ns, so 320 ns does it with margin and still fits
inside the shortest turnaround the PP could produce at 6.25 MHz.

### What the socket looks like with the board pulled

Pin 2 with the board removed sits at a **solid 5 V**. Three consequences, one of
which cuts against the section above:

**There is a real pull-up.** The machine restores its own reply line, so open
drain is the correct model and the assist is a workaround for a slow rise, not a
correction of a wrong model. An earlier note here suggested that a line which
drifted when unloaded would mean the machine expects its slaves to drive high.
It does not drift, so that reading is off the table.

**The board is loading the top of the swing.** Unloaded the line reaches 5.0 V;
with the board fitted the ramps peak at 4.24 V. That 0.76 V appears only when we
are in the socket, and 4.24 V is about what a 3V3 rail plus a diode drop looks
like — the signature of the pad's clamp conducting, which is what a non-5V-
tolerant input on a 5 V bus does. The current involved is well under a milliamp
against any sane pull-up, so it is a loading effect rather than a hazard, but it
is why the ramp flattens as it approaches the top instead of arriving.

**It weakens the timing argument.** An RC curve crosses a TTL VIH of 2.0 V early
in its rise — well before the flattening that the clamp causes — so the line
probably does reach a valid high in time more often than the shape suggests. The
"still looks asserted" mechanism is therefore a plausible lead, not an
established cause.

What settles it is a two-channel capture: **nRPLY on pin 2 against nDIN on pin
1**. If nRPLY is reliably above threshold before nDIN next falls, the reply line
is exonerated and the corruption is elsewhere. If it is not, the mechanism is
confirmed and the assist is the fix. One capture decides it.

### The reply line is exonerated

A two-channel capture with the plain build — nRPLY on pin 2 against nDIN on pin
1 — reads nRPLY 3.92 V peak, 39% high, against a clean 5.6 V nDIN. 3.92 V is
3V3 plus a diode drop, so the pad clamp is conducting exactly as expected, but
it is a comfortable TTL high and the machine reads it fine.

The frame settles it more firmly than the waveform can. **Pulse 8 was short: no
reply we prepared ever went untaken.** The handshake completes on every cycle.
Whatever the reply line looks like, it is not breaking transfers, and the assist
is not the fix. Ugly is not the same as broken.

That frame — long on 1, 2 and 5, short on everything else — also puts the board
in an odd position: we answer every read, on time, every reply is taken, startup
runs to completion, and a ROM block still fails its checksum.

### The first assist program was wrong

Flashed on hardware it made the machine worse — the screen never reached the
cursor, the video memory was not being initialised at all — and the frame read
long on pulse 1, long on pulse 8, short on everything else.

The cause is worth recording because it is a PIO trap rather than a bus
subtlety. **SET maps one pin.** With the SET base at nRPLY, `set pindirs, 0`
releases nRPLY and nothing else, so the sixteen AD lines stayed driven until the
`.wrap` — which the assist had just pushed ~320 ns later. Any cycle the host
opened inside that window met our drivers on every address line. That is
contention on the address, not a timing effect, and it is exactly as destructive
as it sounds.

The IRQ was wrong too. It sat after the delay, so the CPU's served-versus-missed
check at the next address strobe ran before the state machine had raised it,
scored a completed cycle as missed, and re-armed the response machine underneath
itself. Pulse 8 in that frame is mostly this, which makes that run uninformative
about the untaken replies it appears to report.

Both are fixed: the IRQ is raised immediately after the host takes the data, at
the same point in the cycle as the plain program, and a third direction mask —
nRPLY driven, AD released — is preloaded into ISR so the AD lines are let go two
instructions after the strobe while nRPLY alone is held high.

The lesson generalises: an instrumented build has to be timing-identical to the
one it is measuring, and OUT and SET see different pins.

## The reply was arriving before the data

With a per-boot frame that could be trusted, the picture became a contradiction:

| pulse | | |
| --- | --- | --- |
| 1 | long | the monitor started from our vector |
| 2 | **long** | **a ROM block failed its checksum** |
| 4 | short | no reply went untaken |
| 5 | short | the bus carried exactly what we drove, every cycle |
| 6 | long | we were listening from the machine's first cycle |
| 7 | long | all four windows fully covered |

Every word came from us, every reply was taken, the wire carried what we sent —
and the checksum still failed. When every measurement says the data is right and
the processor says it is wrong, the measurements are answering the wrong
question.

They were. Look at what the response program did:

```
    out pindirs, 24     ; drive the AD lines
    mov osr, y
    out pindirs, 24     ; assert nRPLY -- handshake complete
```

**Two instructions. 13 ns at 150 MHz.** nRPLY is the slave saying *the data is
on the lines*, and this program said it 13 ns after starting to drive sixteen
lines into a capacitive 5 V bus — the same bus whose reply line was measured
needing the better part of a microsecond to cross a threshold. The AD lines are
no faster. The host was being invited to latch data that was still on its way.

It fits every symptom exactly. A line already at the right level is correct
immediately, so most words are fine and the machine runs; a word needing a long
transition somewhere is not, so a sum over 16127 of them fails. The boot menu
appears when the dice fall well and does not when they do not.

And it explains why the readback saw nothing. That sample sat at the *end* of
the cycle, after the host had released the strobe — the most forgiving instant
there is, by which time every line has long settled. Pulse 5 was dark because it
was measuring the wrong moment, not because the data was good.

So both halves change. The program now waits ~430 ns after driving the lines
before asserting the reply, and takes its readback **at the instant it asserts**
— the earliest the host could latch, and therefore the only instant worth
checking. Being late costs wait states rather than data, which is the whole
reason a CPU-in-the-loop ROM is viable on this bus; 430 ns against a 2.4 µs
cycle is margin bought at no real price.

**It changed nothing.** The frame came back identical. The setup time is kept
because a slave asserting its reply before its data is valid is wrong however
the machine behaves, and the cost is wait states — but it is not the fault, and
that is the second mechanism proposed here that the hardware has refused.

Which is the point at which guessing at mechanisms should stop. The failure is
also intermittent: boots that print a CPU or CPU-RAM error with no
`- ОШИБКА ПЗУ` beside them are boots where all four blocks verified, and the
monitor prints the ROM line whenever the mask is non-zero. So the next thing to
establish is not *why* but *which* — see pulses 8–9.

## How the two processors are reset, and why it matters here

Worth stating plainly, because it constrains everything above.

**The peripheral processor is the only one with a real reset.** It takes DCLO
and ACLO from the machine's power-on circuitry, and a 1801 starts on the
*falling edge of ACLO with DCLO already low* — an edge, not a level. In the
emulator that is the whole of `CMotherboard::Reset()`: assert both pins on the
PPU, clear the peripherals, release both. Nothing else in the machine is reset
by hardware.

**The central processor has no reset of its own at all.** Its DCLO, ACLO and
HALT pins are driven exclusively by the PP writing port 177716 — bit 5 is DCLO,
bit 15 is ACLO, bit 4 is HALT. `Reset()` never touches them. So the CPU starts
if and only if the PP executes this, out of our ROM:

```
160332  mov #40, @#177716      hold it: DCLO asserted
160340  jsr pc, 173252         load its memory through the plane ports
160360  clr @#177716           release DCLO
160364  mov #100, r0 / sob     settle, a few hundred microseconds
160372  mov #100000, @#177716  release ACLO -- the edge that starts it
```

Two consequences.

**The CPU's power-on is five instructions read from this board.** A single word
misread anywhere in 160332–160376 and the central processor is never started, or
started with the wrong pin sequence, or started before its memory was loaded.
That is a direct route from "an occasional bad ROM read" to `- ОШИБКА ЦП`, and
it does not require the CPU or its RAM to be faulty at all. Pulses 3 and 4 watch
the two ends of it, which separates *the CPU was never started* from *the CPU was
started and failed* — otherwise pure guesswork.

**The PP's own start is an edge from an ageing supervisor.** If that circuit
releases ACLO before the rails have settled, the PP starts erratically, and the
symptom is intermittent trouble that clears on a manual reset — which is the
pattern this machine has shown throughout, including the one boot that reached
the menu. It is also indistinguishable, from the outside, from our own startup
race: the board must be serving before that edge arrives, and nothing in the
firmware can outrun the RP2350 bootrom. Pulse 8 tells the two apart, because it
is short only when the machine asked before we were listening.

So yes — it could be a contributor, and it is worth a scope on the reset line at
power-on to see whether ACLO comes up cleanly or chatters. A supervisor that
retriggers would show as the frame's bits appearing and vanishing between
passes, since each start clears them.

## Testing the central processor's RAM from the other side

`- ОШИБКА ОЗУ ЦП` is the machine saying its central processor's memory is bad,
and that is worth testing properly — which the stock test structurally cannot
do. It runs **on** the central processor, using code that was itself copied
into the memory under test. A fault there corrupts the tester before it can
report on the testee, and every result it produces is suspect.

The peripheral processor can do the job instead, and this matters more than it
sounds because of how the memory is wired:

**The central processor's RAM *is* planes 1 and 2.** Its word at address A is
plane 1 byte A/2 in the low half and plane 2 byte A/2 in the high half — which
is exactly the format of port 177014. So the PP can reach every location of it
through the plane registers, with the central processor **held in reset**, and
the two halves of what comes back name which plane is at fault.

That last part is the useful bit. If a machine has three RAM banks and one of
them is the PP's own (plane 0, which the monitor already tests and passes),
then a `- ОШИБКА ОЗУ ЦП` points at the *other two* — and a bank that tests fine
out of circuit may simply be the one that was never implicated.

```console
$ ./tools/diag/make_ramtest.py -o ramtest.bin
$ ./tools/rom/gen_rom_images.py --logical ramtest.bin -o firmware/rom_images.c
$ cmake -S firmware -B firmware/build-ramtest -G Ninja -DMPI_BEACONS=ON
```

The test holds the CPU in reset for its whole run, checks PP RAM first so a
fault there is not mistaken for a plane fault, then walks both planes twice —
each location holding its address and then the complement, so every bit takes
both values everywhere. It accumulates the XOR of what came back against what
went in, so the result holds exactly the bits that were ever wrong.

Results come back as beacons, and `-DMPI_BEACONS=ON` blinks them. Two things
about that build are worth knowing, because both were got wrong first:

**`MPI_BEACONS` needs `MPI_WATCH`, and used not to say so.** Beacon counting and
the LED frame both live inside `#if MPI_WATCH`, so `-DMPI_BEACONS=ON` alone
produced a firmware that ignored every beacon and ran the ordinary status LED —
which on hardware looks like the board blinking away busily while reporting
nothing whatsoever. It now implies it, resolved before either reaches the
compiler rather than by defining `MPI_WATCH` twice and trusting the later flag
to win.

**The beacons live in ROM, not RAM.** A beacon works by being an address the
board sees go past. At 077700 in PP RAM that depends on the capture machine
latching cycles for addresses we do not serve — probably true, but never
demonstrated: every address this project has confirmed seeing has been one of
ours. At 176700 it needs no assumption at all, because we answer the read
ourselves. Reading ROM is harmless; only the address matters.

| pulse | |
| --- | --- |
| 1 | alive — the test is running |
| 2 | PP RAM (plane 0) passed |
| 3 | PP RAM failed |
| 4 | **planes 1 and 2 passed** |
| 5 | **plane 1 failed** |
| 6 | **plane 2 failed** |
| 7 | finished |
| 8 | **the bit was already wrong on an immediate reread** — dead, not leaky |
| 9–16 | bit 0…7 of the failing plane's byte |
| 17 | **every bit position failed within a single pass** — not a chip, a subsystem |

## Still open: the thermal fault

The dead bit is fixed and the machine boots, runs its own diagnostic and takes
keyboard input — but it still degrades as it warms, which a hard stuck bit
cannot do. So there is a second fault, softer than the first, and it needs a
different kind of measurement: the machine hot, and something still asking.

The plane test now **soaks** rather than parking. It loops for as long as the
machine is left on, and because the firmware's beacon bits are set-only, a bit
that fails on pass four hundred lights its pulse and stays lit. Leave it running
an hour and read the frame afterwards.

Warm, the machine now freezes, blanks, or falls back to the uninitialised
vertical lines after about ten minutes of sitting at the boot screen. All three
are **plane 0** symptoms rather than data corruption: the video tag list lives
at 0000270 in plane 0, which is also the PP's own RAM, so a fault there makes
the display stop making sense and the dispatcher stop dispatching. The soak
tests plane 0 as its first phase, and now names its failing bits too.

That last part exposed a flaw the simulator caught and hardware would not have:
the accumulator recording PP RAM faults was itself in PP RAM, so a bad bit
erased the record of itself and the test reported a clean pass over a faulty
bank. It lives in r6 now — the stack pointer, free because this program never
uses a stack and takes no traps with interrupts masked. **The witness to a
memory fault cannot live in the memory under test.**

One combination is worth reading deliberately: on an intermittent fault **both
"planes passed" and "plane N failed" end up lit**, because over hundreds of
passes both happened. A hard fault cannot produce that pairing, so the frame
distinguishes "always broken" from "sometimes broken" without any timing
information at all. `test/test_ramtest.py` demonstrates it with a fault present
on only one pass in three, rather than leaving it as a claim.

And a clean frame after a long hot soak is just as useful: it would put the
thermal fault outside all three RAM planes, which is most of what is easy to
suspect.

### What the soak actually reported

Two frames, minutes apart, from a cold start:

```
l l s l s s l s s s s s s s s s s     healthy: alive, PP RAM ok, planes ok, done
l l l l l l l l l l l l l l l l l     everything, including pulses 8 and 17
```

There is no third reading between them. That absence is the finding. A weak cell
warming past its retention limit fails **one** bit, in **one** plane, and the
frame grows a pulse at a time; this goes from a completely clean pass to every
bit of every plane, plus pulse 8 (wrong on an immediate reread, so microseconds
after the write, which no retention failure can reach) and pulse 17 (all eight
bit positions lost inside a single pass).

Three banks on two different physical groups of chips do not degrade in unison.
Whatever they share does — the RAS/CAS timing and refresh generator that drives
all 24 chips, or a supply local to the array. Both of those fail as a step, and
that is exactly the shape of the reading.

### Live mode, for freeze spray

The latching frame answers "does this machine ever fail". It cannot answer "did
cooling *this* chip just fix it", because it is built not to: `g_beacons` is
set-only, cleared only when the PP restarts, and the ROM's accumulators sit
outside its loop. A fault lit on pass four hundred stays lit however cold the
guilty part gets. For chasing a thermal fault with a can of freeze spray that
makes it worse than useless — it will report a fault for as long as the machine
is powered, no matter what you do to the board.

So there is a second pairing that reports only the **most recent pass**:

```console
$ ./tools/diag/make_ramtest.py --live -o ramsoak-live.bin
$ ./tools/rom/gen_rom_images.py --logical ramsoak-live.bin -o firmware/rom_images.c
$ cmake -S firmware -B firmware/build-live -G Ninja -DMPI_BEACON_LIVE=ON
$ cmake --build firmware/build-live
```

`--live` moves the clears inside the loop, so each pass starts from nothing;
`-DMPI_BEACON_LIVE=ON` makes core 1 snapshot and clear the beacons on the DONE
beacon, which is the pass boundary. Both halves are needed — either alone still
latches, one because the ROM keeps re-asserting a mask the firmware clears, the
other because the firmware keeps displaying a fault the ROM has forgotten.

The LED stops being a frame and becomes a lamp:

| | |
| --- | --- |
| steady on | the last pass failed |
| dark, with a brief blip each pass | the last pass was clean |
| fast flicker | no pass has completed in fifteen seconds — the PP is stuck |

A pass takes a few seconds, so the lamp follows the machine closely enough to
spray one chip and watch. `test/test_ramtest.py` checks the property this whole
mode exists for: a fault present on pass 2 only must leave pass 3 reporting
clean, because otherwise cooling the guilty chip looks identical to cooling an
innocent one.

### The strobes were a dead end, measured rather than assumed

Six single-shot captures of RAS and CAS at the DRAM pins, in both states, and
they cost three wrong conclusions before producing a right one. The waveform
shapes changed between captures — CAS ramping in one, square in another — and
each time the shape looked like the answer. It was not: those are **Single**
captures of a signal whose content varies cycle to cycle, twelve cycles out of a
continuous stream, and whichever cycle the trigger landed on is what you get.
Two of the captures were relabelled mid-investigation, in opposite directions,
and the "obvious" reading flipped with them both times.

What settled it was picking a statistic instead of a shape. With CAS on CH1 so
its Vrms is reported:

| | healthy | failed |
| --- | --- | --- |
| CAS Vrms | 3.84 V | 3.92 V |
| RAS | 2.38 MHz, 40.0%, 6.00 Vp-p | 2.38 MHz, 40.0%, 5.84 Vp-p |

Two percent, running the wrong way for any story. **The strobes do not change
when the machine fails.** Frequency and duty at that timebase are unusable —
`Duty+: 100.0%` means the scope never found a falling edge on the ramp — which
is its own lesson about which numbers on a cheap scope are load-bearing.

The rule that would have saved all of it: take several captures in the *same*
state before comparing states, so you know what normal variation looks like.

## The address bus, and what it said

Every bit of every plane failing at once is not a statement about chips. Nothing
true of eight independent DRAMs in two separate banks is true of all of them in
the same instant; what they share is the address bus and the strobes, and the
strobes had just been ruled out.

`tools/diag/make_addrtest.py` asks the other question. Three passes over planes 1 and
2, with the CPU held in reset:

```console
$ ./tools/diag/make_addrtest.py -o addrtest.bin
$ ./tools/rom/gen_rom_images.py --logical addrtest.bin -o firmware/rom_images.c
$ cmake -S firmware -B firmware/build-addr -G Ninja -DMPI_BEACONS=ON -DMPI_BEACON_COUNT=19
```

| pass | fill | sees |
| --- | --- | --- |
| 1 | every cell `0` | data lines stuck high |
| 2 | every cell `177777` | data lines stuck low |
| 3 | every cell its own index | addressing **and** data |

The first two are structurally blind to addressing: every cell holds the same
value, so a read that lands on the wrong cell still returns the right answer.
That makes them a pure data-line test. The third is sensitive to both, and it
carries more than pass/fail — with each cell holding its own address, whatever
comes back **is the address of the cell that actually got selected**, so the XOR
against what was asked for names the address bits that went wrong.

Bits the constant passes implicated are masked out of the address report. A data
line stuck at 0 differs from its index in that position too, and calling that an
address fault would point at the wrong half of the board. `test/test_addrtest.py`
holds it to that with a dead data bit, a dead address line, and both at once.

| pulse | |
| --- | --- |
| 1 | alive |
| 2 | pass finished |
| 3 | **data lines bad** |
| 4 | **addressing bad** |
| 5–19 | plane index bit 0…14 |

The machine's answer, warm:

```
l l s l   s l s l s l s l s l s l s l s
    ^ ^   bits: 0 1 2 3 4 5 6 7 8 9 ...
    | addressing bad
    data lines clean
```

**Address bits 1, 3, 5, 7, 9, 11, 13 wrong; 0, 2, 4, 6, 8, 10, 12, 14 clean.**
Every odd bit, no exceptions, with bit 15 outside the tested range.

Two things follow immediately. The 24 DRAMs are exonerated — both constant passes
read back perfectly, so every data line in both planes carries what it is given.
And a perfectly alternating pattern is never four independent faults; it is one
part. Which part depends on how the schematic maps the plane index onto the
DRAMs' eight address pins:

| if row/column is | then the failing bits are |
| --- | --- |
| index 0–7 / index 8–15 | pins **A1, A3, A5, A7** dead in both phases — one package, if the two multiplexers are split odd/even to shorten the routing |
| even index / odd index | **the entire column address**, with the row address perfect |

The second is the simpler failure, and it fits the rest: the column address is
what gets latched on CAS, so a wrong column behind a clean CAS waveform is
exactly the combination that was measured. A wrong column selects the wrong cell
in all 24 chips at once — which is why all eight bit positions in all three
planes go wrong together and immediately, and why the display falls to the
uninitialised vertical lines at the same instant.

One measurement separates them: probe a DRAM address pin with the fault present
and watch whether it changes value between the RAS phase and the CAS phase. Pins
that stop changing at column time, all of them, means the column half of the
multiplexer; specific pins dead in both phases means those lines.

## The thermal fault: D22, and why everything else measured clean

The answer is **D22, a КР1801ВП1-055 gate array, which drives RPLY on the
peripheral processor's bus.** Cooling it keeps the machine running indefinitely;
cooling anything else -- the PP, D8, D11, the DRAM banks -- changes nothing. That
control is the whole result: a single component, repeated, with every other
candidate tried the same way.

### Confirmed in both directions, on the machine's own test

Not on our instruments. With D22 held cold the MS 0511 boots the stock monitor
and runs its own ТЕСТИРОВАНИЕ suite -- ПРОХОД 4, ОШИБОК 0 -- which it had not
managed since the thermal fault appeared. Stop cooling and it fails before that
pass finishes: about a minute, against the ten minutes the whole board takes to
reach the fault from cold.

That gap is the control. A chip that has been chilled returns to ambient in a
minute or two while the board takes ten, so failing inside a minute says the
part's own temperature is the variable and not the air around it. Cooling every
other candidate the same way -- the PP, D8, D11, the DRAM banks -- changed
nothing.

It also retroactively clears the memory. The CPU planes pass the machine's own
test now, so the `- ОШИБКА ОЗУ ЦП` that started this was the dead 4164 on DC7,
and everything after the repair was D22 making good memory look bad.

### What D22 actually is

A **16-bit bidirectional bus transceiver with inversion** — the transceiver
between the machine's inverted AD bus and the peripheral processor's internal
bus, so it sits in the path of every instruction fetched and every word read or
written. Its logic diagram is published; the pin table, the reasoning and a
substitute built from two 74x640s are in
[docs/D22-KR1801VP1-055.md](D22-KR1801VP1-055.md).

That it is a pass-through rather than a storage element is the whole explanation
for the shape of this investigation: nothing is ever *stored* wrong, so every
memory test, register pattern and retention check came back clean while the
machine plainly did not work.

### What has been eliminated since

**The bypass capacitor.** D22's decoupling was pulled and measured: **47 nF**,
which is a correct decoupling value and in tolerance. Replaced with a modern
100 nF anyway -- no change in behaviour. The board uses the same part at every
IC, so there was no odd-one-out to find either. Cleared twice over: the value is
right, and improving it does nothing.

(The marking reads `47н`. Read from a blurry photograph it looked like `47п`,
which would have been 47 pF -- a thousand times too small and a very promising
lead. It was worth chasing and it was worth measuring rather than believing.)

**Latched state of any kind.** The machine recovers *without a power cycle*:
cool D22 and a soft reset on the front panel button brings it straight back. So
nothing is latching -- not latch-up, not a corrupted internal register, nothing
that needs power removed to clear. What crosses a threshold recrosses it as soon
as the temperature drops, which is what a propagation delay against junction
temperature does and what almost nothing else does.

That leaves the die. Worth trying before sourcing a КР1801ВП1-055, in this
order, because both are free and reversible:

- **supply at 5.2 V.** 5 V ±5% puts 5.25 V in spec, and logic of this era gets
  faster with more supply -- more drive, shorter delay. If the part is marginal
  by a little, this may buy back more than cooling does.
- **a fan, not a heatsink.** D22 does not get warm to the touch, so it has almost
  nothing of its own to shed and a heatsink has nothing to remove; its junction
  still sits a few degrees above the surrounding air, and moving that air is the
  only remaining lever.

### Why nothing else ever looked wrong

RPLY is the signal that *completes* a bus cycle. It carries no data, so nothing
it does can be caught by a test that checks values -- and every test in this
repository checks values. That is why the list of things measured clean grew so
long and so useless:

| measured | result |
| --- | --- |
| both CPU planes, zeros and ones fills, with a third of a second between write and check | clean, every run |
| the plane address register, alternating and constant patterns | clean |
| RAS and CAS at the DRAM pins, healthy vs failed | CAS Vrms 3.84 V against 3.92 V |
| the data we drive, sampled at our own pads on every served cycle | clean |
| plane 0's eight chips, all replaced | no change |
| D8, D11, the CPU, the ROM socket, all reflowed | no change |

Nothing is ever *stored* wrong, so every storage test passes. What fails is
*when* the processor is told a cycle is finished.

### The symptoms, and what each one was

Every observation in this file's history is a completion failure seen from a
different angle:

- **hangs.** RPLY never arrives, the cycle never ends, and a 1801 has nothing to
  time it out. The last-address frame froze on 0000246 with no trap, no restart,
  and no further bus activity -- a cycle simply left open.
- **wrong data.** RPLY arriving late means the processor latches the bus at the
  wrong instant, when some lines have settled and others have not. Which bits are
  wrong then depends on the pattern -- which is very likely the odd-bit
  "adjacent-line coupling" fingerprint that was reproducible for days and then
  refused to appear on a test that used the same register with no memory behind
  it.
- **wild jumps.** An instruction fetch completed at the wrong moment returns a
  word that is not the instruction, and the PC goes somewhere arbitrary.
- **plane 0 first.** DRAM cycles have the least timing margin and register
  transfers the most, so as margin erodes the PP's own memory -- reached by
  ordinary `mov r0, (r0)`, the slowest path in the test -- fails while 177010 and
  177014 still work. The soak's digit readout showed exactly that ordering:
  1, then 2, then 4.
- **everything else, afterwards.** Plane 0 is the PP's own RAM, holding its
  vectors, its stack and the video tag list. Once it goes the PP corrupts itself,
  the test running on it reports the CPU planes bad, and the screen falls to the
  uninitialised vertical lines. The delay before that second stage varied from
  one pass to seven, which is what a consequence looks like rather than a second
  independent fault.

### The method note

The instrument was wrong more often than the machine was surprising. Frames that
latched when they should have cleared and cleared when they should have latched;
32 KB of ROM filled with HALT instructions, which turned every excursion the real
machine survives into a death; a trap-vector capture that could not represent
zero; a per-frame sample of an event one pass wide; and three iterations of an
LED format before it could be read reliably at a bench.

The two habits that eventually worked: **run the control** -- cool the other
chips too, or the effect belongs to the board and not the part -- and **prefer a
number to a shape**, because six single-shot scope captures of RAS and CAS
produced three confident and contradictory conclusions, and one Vrms reading
settled it.

## Resolved

With the chip on DC7 replaced, the plane test passes and the MS 0511 reaches the
ЗАГРУЗКА menu, cursor blinking, in 80-column mode.

The whole chain, in the order it was actually established rather than the order
it was guessed: the board serves four windows of a Soviet mask ROM over a
multiplexed bus; the machine's own monitor said its central processor's memory
was bad; a test written for the peripheral processor, run from the ROM socket
with the central processor held in reset, said plane 1; then bit 7; then that
the bit was dead rather than leaky; and the schematic's data-line names put that
bit on one 4164.

Two things worth keeping from it. The board was never at fault — coverage, reply
handshake, chip select, startup race and bus data all measured clean, and every
failure that looked like the board's turned out to be either the machine or a
bug in the instrument. And the instrument was wrong often enough that the habit
of testing it against a model, rather than against the machine it was pointed
at, is what made its answers worth anything.

## Which chips the planes are

From the MS 0511 schematic, the RAM is 24 × K565RU5 (4164, 64K × 1) in two
groups, and the arithmetic pins each plane to a group:

| chips | data lines | width | = |
| --- | --- | --- | --- |
| 8, on D10 | DG0–DG7 | 8 bits × 64 K | **plane 0** — the PP's own RAM |
| 16, on D8 | DC0–DC15 | 16 bits × 64 K | **planes 1 and 2** — the CPU's RAM |

24 chips × 64 Kbit is 192 KB, which is three 64 KB planes, which is what the
emulator allocates. The 8-bit group can only be plane 0, because plane 0 is the
one the PP reaches a byte at a time through 177012. The 16-bit group is the pair
the CPU sees as words, reached together through 177014 — and since that port
puts the **low** byte in plane 1 and the high byte in plane 2:

```
DC0 … DC7   = plane 1 = the low byte of every word the CPU executes
DC8 … DC15  = plane 2 = the high byte
```

So **plane 1 bit 7 is the chip on DC7**. Physically the 16-bit group is likely
two rows of eight, and DC0–DC7 is the row that is plane 1.

One bound worth knowing before trusting a clean result later: the test walks
32768 addresses of each plane, which is half of a 64 KB plane. That covers
everything the central processor can reach — its window below 160000 maps to
about 28 KB of each plane — but not the upper half, which is video-only. A fault
there would be in the same chip regardless, so it does not change which part is
implicated; it does mean a pass is a pass over the CPU-visible half.

## The answer: plane 1, bit 7, hard

On hardware the frame read **long long short short long short long short** —
alive, PP RAM good, planes *not* ok, **plane 1 bad**, plane 2 fine, done.

The final frame reads **long long short short long short long long**, then
seven short and a long: plane 1 bad, **stuck rather than leaky**, **bit 7** and
no other. A single bit of the central processor's RAM that cannot hold a value
at all — wrong on an immediate reread, not merely wrong later.

One bit is one column of the array, so on 1-bit-wide DRAM this is one chip. The
central processor's RAM is plane 1 and plane 2, and plane 1 is the low byte of
every word it executes. That is the machine's `- ОШИБКА ОЗУ ЦП` confirmed from
the outside, by a test running on the other processor with the faulty one held
in reset — and it explains the whole cluster of symptoms at once. The CPU's
program is copied into planes 1 and 2 before it is released, so corrupt low
bytes give a CPU that reports itself broken, halts with `*** СТОП ***`, or runs
just well enough to paint a menu, depending on where the damage lands.

It also explains why a bank tested out of circuit came back clean: plane 0 is
the PP's own RAM, which the monitor tests and passes on every boot, and which
this test passes too.

On the second run, pulses 9–16 read short for bits 0–6 and **long for bit 7**:
a single bit, and therefore a single column of the array — one chip, not the
bank, not the supply, not a shared strobe.

Pulse 8 separates the two ways one bit can be wrong. The test's first pass over
the planes now writes a word and reads it straight back before anything else
touches the array, in both polarities, and records what was already wrong at
that moment. A bit wrong there cannot hold the value at all; a bit right there
and wrong in the later passes held it and lost it. Dead chip against leaky one,
which is what decides whether to suspect the part or its refresh — and, given
the machine has been reported to worsen as it warms, worth reading cold and
warm.

Two details in that pass are load-bearing. Writing 177014 updates the register
as well as the array, so reading it straight back returns what was just written
and proves nothing; the address register has to be rewritten to re-latch the
data registers from memory. And the pass needs the complement half, because
writing the address as the data leaves the high byte counting only 0…127 over a
32768-word plane, so bit 7 of plane 2 would never once be set — the test would
have been structurally blind to exactly the kind of fault it just found, had it
been in the other plane.

### Scoping it, fairly

A probe on the suspect line during the test reported "much less activity than
the other bits", which looked like evidence and was not. **The test writes the
address as the data**, so the low byte counts 0…255 over and over: bit 0 toggles
on every write, bit 7 once per 128. A perfectly healthy bit 7 looks sluggish
next to bit 0 for that reason alone.

So the test no longer parks silently. Once it has reported, it sits in a loop
writing all-zeros then all-ones to one address, which toggles all sixteen plane
data lines at the same rate. Every bit becomes comparable with every other, and
in particular **bit 7 of plane 1 with bit 7 of plane 2** — the same position in
the same kind of chip, and the only genuinely fair comparison available. A weak
or dead line stands out against its own twin instead of against a faster
neighbour. The beacon read stays in the loop so the LED keeps reporting while
the probe is on.

Pulses 9–16 are the follow-up. The test already knew *which bits* — that is the
mask it leaves at 077662 — it simply had no way to say so on hardware. Each
pulse is one bit position of the failing plane's byte, and on a bank built from
1-bit-wide DRAM each bit is one column: a list of chips rather than a diagnosis
to interpret.

### Running it in the emulator, with no emulator changes

ukncbtl needs no patching to run our ROMs. `Emulator_LoadUkncRom()` looks for a
file called **`uknc_rom.bin` in its working directory** and loads exactly
**32256 bytes** from it, falling back to the built-in resource only when the
file is absent. 32256 is our image size exactly, so:

```console
$ cp ramtest.bin /path/to/ukncbtl/uknc_rom.bin
```

and the emulator boots the test ROM in place of the system ROM — the same
substitution the board performs in hardware, for free.

Reading the result there is the only gap, because beacons are *addresses on the
bus*: precisely what the board can see and what an emulator cannot show without
being modified. So the test also leaves its verdict in memory, written after
every test has finished:

| address | |
| --- | --- |
| `077660` | status: 1 PP RAM ok, 2 plane 1 bad, 4 plane 2 bad, 10 done (octal) |
| `077662` | every plane bit that was ever wrong |

A healthy machine parks with `077660` = **11** and `077662` = **0**. Open the
memory view at 077660 and the answer is there, with no virtual LED to build.

The status bits are powers of two, and are written that way after an earlier
version set "done" to a Python `10` — decimal ten, binary 1010 — which
overlapped the plane 1 bit, so a healthy machine and one with a dead plane
reported the identical status word. The test did not catch it because both sides
of the comparison shared the error, which is how a diagnostic ends up lying with
confidence.

### Drawing on the screen, when it is worth it

The video path is now fully specified, and it is simpler than expected. The
display is built from a **tag list in plane 0 starting at 0000270** — and plane
0 is the PP's own RAM, so the PP writes the whole thing with ordinary MOV
instructions, no ports involved. Each 2-word tag is `[addressBits, next]`, where
`addressBits` is where that line's pixels live and the low bits of `next` say
whether the following tag is the 4-word form that sets the palette or the scale.
307 lines are walked; drawing starts at line 19.

So a PP-side diagnostic can put a picture on screen using nothing but normal
memory writes, with the central processor held in reset — which works
identically in ukncbtl and on real hardware, and needs no LED and no beacons.
That is the natural home for test names, `ПРОХОД`/`ОШИБОК` counters and the
monitor test patterns. It is a real piece of work rather than a quick addition,
and the memory verdict above answers the immediate question without it.

`test/test_ramtest.py` runs the assembled image against a model of the PP with
three planes behind the registers and the ability to break one bit of one
plane, and checks that a healthy machine reports pass while a broken plane
reports itself and not its neighbour. It earned its keep twice: the first
version accumulated the OR of the observed and expected values rather than
their difference, so it reported both planes bad whenever either was; and it
caught the assembler bug below.

### The assembler was reading octal as decimal

`_num` used `int(tok, 0)` — Python's rules — so a bare `177716` was **decimal**,
assembled as 133064, and the instruction meant to hold the CPU in reset wrote
into the middle of the ROM instead. Nothing complained; the program was simply
wrong. `make_testrom.py` had never noticed because it interpolates Python ints,
which round-trip through decimal by luck.

Bare numbers are now octal, as on any PDP-11, with `0x`/`0o`/`0b` prefixes and a
trailing dot for decimal (`10.` is ten, `10` is eight). Both generators emit
octal into their templates, and `test/test_testrom.py` still passes, which is
what makes the change safe to have made.

## The machine's own verdict: ЦП, not ПЗУ

With coverage instrumented, the frame came back long on 1, 2, 6 and 7 — and all
four windows complete. Every word of every window we serve was asked for and
answered by us. Nothing was banked away, nothing went unanswered, no reply went
untaken, and we were listening before the machine asked its first question.

The screen is now doing the talking. Repeated resets produce, variously, the
boot menu; a `*** СТОП ***` halt display with a PC and PSW; and the startup test
screen:

```
СТАРТОВЫЙ ТЕСТ
- ошибка ЦП
```

**ЦП is the central processor, and there is no `- ошибка ПЗУ` line beside it.**
On that boot the monitor checksummed all four of our windows and was satisfied,
then found the central processor faulty.

That matters more than it looks, because of how the UKNC is built: **the central
processor has no ROM at all.** Everything below 160000 in its address space is
RAM and everything above is I/O, so it runs entirely on code the PP writes into
it through the plane ports — the copy loop at 173252, sourcing from our image at
160000–173212 while the CPU is held in DCLO reset. The PP then releases it and
reads its verdict back over channel 0 at port 177060.

So the chain is: our ROM → PP → CPU RAM → CPU self-test → a byte at 177060 → the
message on screen. Our end of that chain now measures clean at every point we
can instrument, and the checksum verifies the bytes.

The one link still unmeasured is the wire itself, which is what pulse 5 is for:
the response machine samples the AD lines at the instant the host releases the
read strobe, with our drivers still on, and compares against what it was asked
to drive. If that stays short while the CPU error persists, the board has been
exonerated at every point it is possible to exonerate it, and the fault is in
the machine.

## The boot menu, seen

With the plain watch build flashed, a restart followed by a **reset** produced
the boot menu on the real machine for the first time. That is the single most
important result in this file, and it changes the shape of the problem: nothing
is missing and nothing is fundamentally wrong. The machine is *marginal*.

The way it happened is the lead. A reset is not a power cycle — the board was
already up, clocked and serving when the PP restarted. On a cold start it is
not, and it cannot be:

| | who is doing what |
| --- | --- |
| power applied | the PP leaves reset and fetches its power-up vector at 160000 |
| meanwhile | the RP2350 runs its bootrom, sets up XIP, starts our code, sets the clock, loads two PIO programs, launches core 1 |

Nothing in the firmware can make the bootrom faster, so if the machine asks
before we are listening, its first reads go unanswered. A ROM that is missing
for the first instructions of a startup sequence is exactly the kind of fault
that is intermittent, that clears on a reset, and that leaves the machine in a
state no amount of reading the ROM contents will explain.

Pulse 9 tests it directly. The PP's first read after reset is its power-up
vector, so if the first cycle we ever capture is 160000 we were in time; if it
is anything else we came up mid-stream. **Long on reset and short on cold
power-on confirms the race.**

The prediction is worth making before the measurement, because it is cheap and
sharp: *power on, wait a second, press reset* should boot far more reliably than
a cold start. If it does, the ROM board is not at fault in any interesting
sense — it is simply late, and the fix is to keep it powered or to hold the
machine off until it is ready.

### The board is not the problem — superseded

The reasoning below is left because it is still sound as far as it goes; it is
simply not evidence about what the machine *reads*, which is what pulse 2
settles. Both arguments are about the bytes we hold, and both remain true of a
board whose data never arrives intact.

**The menu text lives 40 bytes from text the machine already displays.** The
ЗАГРУЗКА block is at 103116, and the УСТ settings text the user can reach sits
immediately before it at 103040:

```
103040   ый|3 - выключен  |1 - включен |2 - выключен|..ЗАГРУЗКА.......
103140  ...(0.3): 0.........(1,2): 1|1 - диск        |2 - кассета ПЗУ |
103240  3 - сеть        |4 - стык С2     |5 - магнитофон  |6 - отладка
103340  |7 - тестирование|...
```

Same chip, same window, same 128 bytes. A board that serves one and not the
other is not a failure mode that exists.

**The monitor checksums its own ROMs and is satisfied.** The routine at 160410
sums each window as a ones'-complement sum and compares against four values
mask-programmed into the top of the last chip; a mismatch prints `- ОШИБКА ПЗУ`.
`tools/rom/rom_checksum.py` runs that same algorithm on the images we serve:

```
$ ./tools/rom/rom_checksum.py uknc_rom.bin
160000..176774   3839 words  computed 103607  stored 103607  ok   208
140000..157776   4096 words  computed 162125  stored 162125  ok   207
120000..137776   4096 words  computed 133314  stored 133314  ok   206
100000..117776   4096 words  computed 063160  stored 063160  ok   205 (DS4, ...)

all four blocks pass: the monitor's own ROM test is happy with these images
```

Note the block boundaries: the machine's own test partitions the ROM exactly by
chip window, so a failure names a chip. Flip one bit anywhere in the 205's
window and only that line goes MISMATCH.

### Asking the machine instead of guessing

Which decision is still open, and inference has gone about as far as it usefully
can. The board, though, sees every instruction the PP fetches — so it can be
asked directly. `-DMPI_WATCH=ON` builds a firmware that scores a handful of
monitor addresses and blinks the result; see [DIAGNOSTICS.md](DIAGNOSTICS.md).

The decisive one is **101000**, the `emt 44` whose inline argument points at the
ЗАГРУЗКА string. If it hits, the menu was drawn and something happened to it
afterwards, which makes this a display problem. If it does not, the machine
branched away earlier and the search moves upstream. Either answer removes half
the remaining possibilities, and it costs one reflash and a look at the LED.

### What option 7 does

Selecting 7 (тестирование) from the menu in the emulator gives a screen headed
`Т Е С Т И Р О В А Н И Е` with `ПРОХОД:` and `ОШИБОК:` counters, and the pass
counter increments — it is a continuously looping test with an error tally, not
a one-shot report. That is a good model for our own suite: loop, count passes,
count errors, and stay readable while running.

## Running your own code on the PP

Because the board *is* the ROM, it owns the machine from reset — which makes it
possible to replace the system monitor with a diagnostic that runs on the
peripheral processor itself.

The hook is the power-up vector. The PP fetches its starting PC and PSW from a
HALT-mode vector at **160000/160002**, which is offset 0 of the code 0 image.
The stock monitor points it at 160300; put your own value there and your code
runs before anything else in the machine does, with no monitor to work around.

Reporting results is the harder half — at reset the PP has no screen it can
reach unaided. So the program signals by *reading* from reserved addresses.
Every address is on the AD lines at the strobe and the board latches every
strobe, so the board sees each beacon go past. It is the ROM under the program
and the instrument watching it at once, which is not a thing a mask ROM could
ever be. Beacons live in PP RAM near the top, so a read there disturbs nothing;
only the address matters, never the data.

```console
$ python3 tools/diag/make_testrom.py -o testrom.bin
wrote testrom.bin: 32256 bytes, 48 words of code
  power-up vector at 160000 -> 160300, PSW 000340
  beacons at 077700: 0=alive 1=RAM ok 2=RAM bad 3=done

$ python3 test/test_testrom.py
healthy RAM:                     beacon sequence: [0, 1, 3]
RAM with a bit that will not set: beacon sequence: [0, 2, 3]
all checks passed
```

`tools/pdp11asm.py` is a small assembler covering what test code needs, and
`test/test_testrom.py` runs the assembled image in a model of the PP before it
goes near hardware. That simulator immediately earned its place: the first RAM
test wrote each word's own address into it, which is a good address-decode test
but a weak stuck-bit test — a location only proves bit N works if its address
happens to have bit N set. A bit stuck low at an address that never sets it went
straight through. Hence the second pass with the complement, so every bit takes
both values at every location.

Next, in rough order of usefulness: run the image in ukncbtl, which takes the
same 32 KB file and costs nothing to be wrong in; teach the firmware to watch
the beacon range and report on the status LED; then extend the suite outward
into the I/O page and the channel to the central processor.

### Getting a picture out of it

Beacons are enough for a pass/fail, but not for a diagnostic anyone would want
to read, and the obvious objection to a PP-side test suite is that the video
memory belongs to the central processor. It turns out not to matter: the PP has
its own port into it, and can draw the screen with the CPU held in reset.

| port | what it is |
| --- | --- |
| `177010` | plane address register — a byte address into the 64 KB frame |
| `177012` | plane 0 data — a byte written straight into the CPU's RAM |
| `177014` | plane 1 & 2 data — low byte to plane 1, high byte to plane 2 |
| `177016` | sprite colour |
| `177020`/`177022` | background colour, planes 0–2, bits 0–3 and 4–7 |
| `177024` | pixel byte: writes all three planes through the colour registers |
| `177026` | plane mask |

Write an address to 177010, then a byte to 177012, and a byte of plane 0 has
changed. Three planes give eight colours; 177024 with the background registers
loaded does all three in one write, which is how you fill an area quickly.

The stock monitor uses exactly this, and its startup sequence is the worked
example: the routine at 173252 sets 177010 to 70000 and streams 3839 words into
177014, then walks a table of addresses writing 600 to each — all with the CPU
held in DCLO reset from the `mov #40, @#177716` two instructions earlier. The
screen is up before the central processor has executed anything.

So the diagnostic can look like the stock one — a heading, a list of tests with
results beside them, `ПРОХОД` and `ОШИБОК` counters — and the test patterns for
setting up a monitor (greyscale ramp, colour bars, a border-to-border grid,
convergence crosshatch) are simply fills through 177024. None of it needs a
working CPU, a working keyboard, or a working disk, which is the point: it runs
on a machine that is too broken to run anything else.

The one thing to establish on hardware is the frame layout — where in the 64 KB
the visible lines actually are, which on this machine is set by a line table
rather than being a flat bitmap. The monitor's own drawing code is the reference
for that, and `tools/pdp11dis.py` reads it.

## A conditional that should not have been one

One more, this one entirely ours rather than the machine's. It is the reason the
plain and diagnostic builds now share a single serving path.

After the RAM repair the plain build would not initialise the machine and the
status LED blinked fast — which is precisely what the LED is defined to mean:
cycles being served, replies prepared, none of them taken.

The response program samples the bus on every served cycle and autopushes the
result. Something has to empty that FIFO whether or not anyone reads it, and the
drain was written inside `#if MPI_WATCH`. The watch builds emptied it; the plain
build never did. Four served cycles filled it, autopush stalled the state
machine at the `in`, and the board stopped answering — while core 1 carried on
capturing addresses and queueing replies nobody would ever take, which is the
5 Hz blink exactly.

The comment above the drain said "it also has to happen every iteration
regardless: autopush stalls the state machine on a full FIFO", and it was behind
a conditional. Knowing the rule and encoding it are different things.

It drains unconditionally now, with only the comparison under `MPI_WATCH`, so
the plain and diagnostic builds share the one path and cannot diverge again.

