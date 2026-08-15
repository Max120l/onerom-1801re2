# The UKNC's built-in debugger, decoded

Boot menu item **6 — отладка** starts a machine-language monitor that ships in
the system ROM and appears to be documented nowhere reachable. What follows was
recovered by disassembling the ROM this project serves and then verifying every
claim live on the machine, command by command. It is written down because the
next person to select item 6 will otherwise face what we faced: a blank screen,
a cursor, and a parser that ignores wrong guesses in complete silence.

## What it is

A **console-mode (пультовый) debugger for the central processor**. The
peripheral processor runs the screen-and-keyboard side as an ordinary program;
the CPU sits **halted** between your commands, and the monitor reaches into it
with the К1801ВМ2's console-mode operations — `MFUS`/`MTUS` to read and write
its memory, `WCPC`/`WCPS` to set its PC and PSW, and the hardware GO and STEP
ops to resume it. The register display on entry shows the CPU parked with
PC = 000002: it has executed a HALT at address 0 and stopped.

The same architecture as this project's ROM-socket diagnostics, one layer down:
something alive watching something stopped.

## The grammar

Every command is **`[octal number] [command key]`**. Digits `0–7` echo as you
type; **ЗБ** deletes the last digit; the first non-digit key is taken as the
command. The prompt is `@` (which the UKNC font renders close enough to `a` to
mislead).

Two things about the parser that cost us real time:

- **Unrecognised keys are ignored with no feedback whatsoever.** No echo, no
  bell. A working key with no meaning and a broken key are indistinguishable —
  we nearly dismantled a healthy keyboard over five keys (С, И, Ч among them)
  that were doing exactly what the code says: nothing, silently.
- **Commands are alphabet-blind.** Input is masked with `bic #240`, which folds
  case and folds KOI-8 Cyrillic onto the Latin codes — Р and R, М and M, С and
  S are the same command. Every command is therefore reachable from two
  different physical keys (its РУС key and its ЛАТ key), which is also a handy
  way to test individual switches without opening the keyboard.

## Commands

| command | what it does |
|---|---|
| **Р** (R) | print all of the CPU's registers R0–R7 plus РS (the PSW). **Рn** opens register *n* for editing; **РS** opens the PSW. |
| **М** (M) | memory mode. Prints nothing on entry — an invisible escape sequence — and waits. See below. |
| **А** (A) | breakpoint slots. **А0**–**А3** open four address registers; on resume the monitor saves the word at each address and plants `000000` (HALT) there, so the CPU traps back to the console when it arrives. Breakpoints by implanted halts. |
| **Т** (T) | loads a test program from ROM into the CPU's RAM at 001000 and launches it. Treat like GO: it ends the quiet halted-CPU session. |
| **Д** (D) | clears the screen and swallows keystrokes until ^C. |
| *number* + **space** | **GO** — writes the number into the CPU's saved PC and resumes it there. With no number: continue from where it stopped. The one command here that deserves respect. |
| **ВК** | close the current line/location without writing; fresh prompt. |
| ^C | abort. ^L (code 14) clears the screen. |

## Editing — registers and memory share one grammar

With a register or memory location open:

| key | does |
|---|---|
| octal digits + **↓** | **write** the value, open the **next** (register mod 8, or address+2) |
| octal digits + **↑** | **write** the value, open the **previous** |
| bare **↓ / ↑** | step without writing |
| **ВК** | close without writing |

In М mode, *number* + **→** opens that address. The display format is
`address/ contents`.

A worked example, as run on the real machine:

```
@ Р3              R3=157776
  123456 ↓        (writes, moves to R4)
  ↑                R3=123456        the deposit, read back

@ М 001000 →     001000/ 000000
  125252 ↓  052525 ↓  177777 ↓  000000 ↓
  ↑ ↑ ↑ ↑         all four words read back in reverse
```

That is a hand-run memory test: the same question `make_ramtest` asks, one word
at a time, with a person as the loop.

## `*** ЗАВИСАНИЕ ***` and the map it draws

ЗАВИСАНИЕ ("hang") is the monitor's **bus-timeout report**: it issued the read,
nothing on the CPU's bus ever replied, and rather than freeze it tells you so
and re-prompts. This makes the debugger a hand-held answers-versus-hangs probe
of the CPU's **user-space** memory map — measured live on this machine:

| CPU user space | result |
|---|---|
| 000000–157777 | RAM (planes 1 and 2). Reads and writes work. |
| 160000–176777 | **hangs.** Nothing answers on a cartridge-less machine. The CPU's resident firmware is *not* here — it lives in console-mode space; `MFUS` reads user space. |
| 177xxx | I/O registers answer. The console-channel cluster sits at 177560–177566; a ready bit (000200) was observed at **177562**. Status registers ignore writes — depositing into one is not a broken monitor, it is hardware telling you its actual state. |

If roadmap step 2 (cartridge serving from the One ROM) ever lands, `М 160000 →`
answering with data instead of ЗАВИСАНИЕ is its natural acceptance test.

Note the converse limitation: this debugger reaches the **CPU's** world only.
The PP's own RAM — plane 0, the video tag list, the beacon area our test ROMs
use — is not addressable from here.

## The screen, and why the debugger cannot draw on most of it

The display is a tag list in plane 0 starting at 000270: one entry per scan
line, each saying where that line's pixels live. Reading the list out of a
running machine (via ukncbtl's Display List viewer — the tags are in plane 0,
which this debugger cannot see) gives the monitor's screen layout:

| lines | pixel addresses (plane bytes) | what |
|---|---|---|
| 0–16 and all blank fillers | **000000** | one shared blank buffer |
| 19–29 | 175700–176450 | the status row (РУС/ЛАТ indicator etc.) |
| 31–294 | **100000–151060**, stride 0120 | the main screen, 80 bytes per line |
| 296–306 | 176570–177410 | bottom service rows |

CPU user space reaches plane bytes 0–67777 only (CPU address = 2 × plane byte).
Every visible line therefore lies **outside user-mode reach** — and the one
tag target that IS reachable, the shared buffer at plane byte 0, belongs to
lines 0–18, which the video path never renders at all: they are the vertical
blanking interval (`if (yy < 19) continue;` in ukncbtl's render loop, matching
hardware).

So the verdict, proven from both ends: **drawing on the console's screen from
this debugger is impossible by construction.** Not hidden by overscan, not
mapped to a black palette — unreachable. The console armors its entire visible
display against user-mode writes, and the only writable buffer is in the
blanking interval. Software that wants CPU-drawn graphics builds its own tag
list pointing into CPU-reachable plane bytes — the tags decide whose memory
the screen is — and the tag list itself lives in plane 0, which belongs to
the PP. Drawing is a cooperation between both processors, by design.

(An earlier revision of this section claimed a deposit at addresses 0–6 would
show as a bar on the blank top lines. It does not — those lines are never
drawn — and the claim is kept here, corrected, as a record of how it failed:
the deposit was verified in memory, on hardware and in the emulator, and the
pixels were looked for and absent in both before the render source explained
why.)

Two traps for anyone re-verifying this in ukncbtl, both walked into here:

- The memory window opens on **ROM** view, not CPU — cycle spaces with the
  Space key, or use the console's `p` (switch processor) and `m <addr>`. A
  deposit that "did not land" was in CPU memory all along, in a pane that was
  not being displayed.
- The console keeps its **session transcript as ASCII text in plane-0 RAM
  around 002000–002270**, directly below the tag list. Watching that region
  while typing shows your own keystrokes and the monitor's replies streaming
  into memory — which looks exactly like deposits going astray and is nothing
  of the kind. It also provides a live proof of the space split: the debugger
  session shows `002274/ 000000` while plane 0 at the same numeric address
  holds a tag pointer — same address, two memories.

## ROM geography, for whoever digs next

For the stock ROM set (reference image, 32256 bytes): main command loop and
dispatch at **161044–161150**; open-location editor at **161424**; function-key
dispatch tables at **162222** (editor) and **162350** (М mode); octal reader at
**162602** (digits, echo, ЗБ handling); the Т loader at **162522**, fed from
**164600** with destination 001000; prompt and message strings at **163505**
(ESC sequence), **163510** (`CR LF SI "@ "`), **163530** (`CR LF SO "*** "`).
Function keys arrive as codes 176–207 and dispatch after `sub #176`. The
CPU-side text (ТЕСТИРОВАНИЕ, ПРОХОД, ОШИБОК, the network messages, ЗАВИСАНИЕ
itself) is stored in **KOI-7**, which reads as ASCII transliteration in a hex
dump — the KOI-8 menu strings and the KOI-7 program strings coexist in one ROM,
and a string search that assumes one encoding finds half the truth.

## Method note

Every entry in the command table was found by reading the dispatch comparisons
in the disassembly, and then **confirmed by a human at the machine** reporting
what the screen did — including the two places the static reading was wrong
until the hardware corrected it: the register display (Р alone dumps
everything; the code path read as an error handler) and the user-space hang at
160000 (the static reading placed the firmware copy there; `MFUS` sees user
space, where nothing answers). The machine remains the authority.
