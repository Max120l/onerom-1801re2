# The ROM socket as a diagnostic port

*An idea worth building properly, written down while the reasons are fresh.*

For what actually exists today rather than what should, see
[DIAGNOSTICS.md](DIAGNOSTICS.md).

Everything in this repository was built to find one fault in one machine, and
almost none of it is really about that machine. The general thing underneath is:

> **A ROM emulator is the only device in a computer that is simultaneously the
> source of the code and an observer of the bus.** That makes the ROM socket the
> best diagnostic port on any machine that has one — better than a serial port,
> because it works before anything is initialised, and better than a logic
> analyser, because it can *change what the machine executes* and then watch what
> happens.

A mask ROM could never do this. It answers reads and has no idea what it is being
asked. A One ROM board answering the same reads knows every address, can serve
different code on demand, and can report what it saw — all through pins the
machine already has.

## What this turned into, in practice

For the Elektronika MS 0511 the loop became: write a small PDP-11 program,
assemble it, serve it in place of the system ROM, and have it report by *reading
reserved addresses* the board watches for. No console, no serial port, no working
RAM required — the machine reports before it can print, and the report survives
the machine crashing, because the recording is on the other side of the socket.

That gave, in order of how much they mattered:

| test | what it answers |
| --- | --- |
| **watchpoints** | did the machine execute this instruction? Scored as address *pairs* so a data read cannot be mistaken for a fetch |
| **coverage** | did every word of every window get asked for? Proves reads are reaching us rather than being banked elsewhere |
| **RAM walk** | which plane, which bit, dead or leaky, from outside the processor under test |
| **address-bus test** | constant fills are blind to addressing, an index fill is not — so the two together separate a bad data line from a bad address line, and the XOR names the address bits |
| **register patterns** | one register, alternating vs constant patterns, no memory involved — separates lines interfering from a bit stuck |
| **last address / trap vector** | where the processor was when it stopped, and which trap took it |

## What generalises

Most of it. The machine-specific parts are small:

- **an assembler for the target CPU** (`tools/pdp11asm.py` is 140 lines)
- **the ROM layout**: reset vector, entry point, window mapping
- **the beacon addresses**, which need only be addresses the board serves and the
  program never otherwise touches
- **a handful of device registers** if the test wants to reach memory the CPU
  cannot address directly

Everything else — the pair-scored watchpoints, the coverage bitmap, the
"which bits differ" arithmetic, the pattern selection, the last-address capture,
the whole idea of a program that reports by reading — is architecture-neutral.
A 6502, a Z80 or a 6809 version is the same design with a different assembler.

## The reporting problem, and the fix

The LED is the weakest part of this by a wide margin. It went through, in order:
a latching frame, a live lamp, a per-pass frame, pulses grouped in fives, a lit
start marker, and finally a single digit — because a twenty-seven pulse frame is
not a readout, it is an endurance test, and a frame that cannot be read reliably
is not a measurement however much information it theoretically contains. One
reading came back as 22/24/26 when it was probably 22/23/24/26, and those decode
to different faults.

**USB is the answer and the board already has it.** The RP2350 has native USB and
the same cable used for flashing can carry a CDC serial port. Then a frame is a
line of text, results have names instead of pulse positions, and a soak can log
with timestamps instead of being watched. One caution to design around: the board
is powered from the socket, so plugging USB into a live machine needs thinking
about before it is done casually.

Worth keeping the LED as a fallback for the case where a laptop cannot be near
the machine — but as the fallback, not the primary.

## Design notes that cost real time to learn

- **Latch or live, and know which you want.** A latching frame answers "did this
  ever happen"; it is useless for freeze spray, where the question is whether the
  fault just went away. Both modes are needed and they are not interchangeable.
- **Sample faster than the event.** A window one pass wide is invisible to a
  reader that samples once per frame. Latch on the core that sees every event.
- **A test that dies before it can speak looks exactly like a test with nothing
  to say.** Phase markers — one beacon per loop, lit on entry — turn a silent
  death into "it stopped in the third loop".
- **Do not fill unused ROM with zeros.** On a PDP-11 a zero word is HALT, so 32 KB
  of zeros turns every stray jump into a fatal one, while the ROM being replaced
  has real code everywhere. Fill with NOPs and the image becomes a slide that
  lands back in the test. The severity being measured was partly the instrument's.
- **Report each verdict when it is reached**, not at the end of the pass, or a
  crash discards everything learned before it.
- **Run the control.** Cooling a chip and seeing improvement means nothing until
  cooling three other chips shows no improvement. This is what finally separated
  D22 from everything else, and its absence is what wasted the most time.
- **Prefer a number to a shape.** Six single-shot scope captures of RAS and CAS
  produced three confident and contradictory conclusions; one Vrms reading
  settled it. Instruments that require judgement produce judgement.

## What a general version might look like

```
onerom-diag/
  targets/<arch>/    assembler, ROM layout, beacon protocol
  tests/             the architecture-neutral test logic
  host/              USB client: run a test, stream results, log a soak
  sim/               a model of the target, so the diagnostic is itself tested
```

The `sim/` directory is not optional. Every test in this repository was wrong at
least once in a way that only a simulator caught — an accumulator for a memory
fault that lived in the memory under test, a status constant whose bits
overlapped another, an OR where a XOR was meant. A diagnostic nobody has tested
is a guess with a confident voice.

## Open questions

- How much of the beacon protocol can be shared across architectures? Reading a
  reserved address works anywhere; the addresses do not.
- Can the board self-identify the host? Reset vector and first fetch addresses
  are a strong fingerprint for common machines.
- Is there a useful "generic" first test — one that assumes nothing about the
  host except that it fetches from the socket, and reports whether it is running
  at all, how fast, and what it reads?

---

# Emulating the other chips, not just the ROM

*Max's second idea, and it generalises the first one further than it looks.*

The 1801ВП1 family is a set of semi-custom gate arrays used across Soviet PDP-11
clones, in a distinctive 42-pin package, and they are exactly the parts that
cannot be bought when one dies. Emulating them from a microcontroller in that
footprint would be worth a great deal to anyone restoring these machines.

It works for some of them and not others, and the line between is sharp.

## The test: does anything wait for it?

This is why OneROM works at all, and it is stated in `firmware/main.c`:

> Being late costs wait states, not corruption.

The MPI bus has a handshake. A ROM may take a microsecond to answer and the
machine simply waits, so an emulator a hundred times slower than the original is
functionally perfect. **That property is the whole licence for emulating a chip
with software.**

A bus transceiver has no handshake — it is in the path *of* the handshake.
Nothing waits for it, because everything is waiting through it.

| emulate in software | why |
| --- | --- |
| ROM, RAM | the bus waits; proven by this project |
| UART, timer, keyboard, disk controllers | register-based, microsecond response, handshake gives slack |
| interrupt controllers | responds to a request rather than sitting in a combinational path |

| do not emulate in software | why |
| --- | --- |
| bus transceivers, e.g. the -055 | nothing waits; the emulator *is* the path |
| address multiplexers, DRAM strobe generators | nanosecond windows, no handshake at all |
| decoders and glue | must settle before the cycle can proceed |

The numbers, for the -055 specifically: an RP2350 sampling sixteen pins,
inverting and driving sixteen others costs the input synchroniser, a few PIO
cycles and the pad delays — call it 30–40 ns at 150 MHz. A 74x640 is around 10,
and the original is one to three gate delays. Inserting that into a bus whose
failure mode is *already* timing margin would make things worse, not better.

## One board, two populations

The two columns want different silicon but the same mechanics, so it is one
project rather than two: **a 42-pin footprint adapter, populated either way.**

- **RP2350** for the register-like variants — and the bus interface is already
  written. The PIO programs, the address decode, the reply handshake and the
  whole diagnostic apparatus in this repository transfer directly.
- **CPLD** for the glue variants — an ATF1504AS or similar, where the answer is
  combinational logic at wire speed and there is nothing to run.

Same board, same pins, same adapter, one decision at assembly time.

## Why this follows from the ROM work rather than being a new idea

A ROM emulator turned out to be a diagnostic port because it is both the source
of the code and an observer of the bus. A peripheral emulator in the same family
is the same thing again: it can serve a device, lie about a device, or simply
report every access to a device — which is a second kind of instrument for a
machine that cannot yet print anything.

And it inherits the lesson that cost the most time here: **build the simulator
first.** Every test in this repository was wrong at least once in a way only a
model caught. An emulated peripheral that has never been run against a model of
its host is a guess with a confident voice, and this time the guess would be
wired into the machine rather than watching it.
