// Address watchpoints: ask the machine what it actually executed.
//
// The board is the ROM, so every instruction the peripheral processor fetches
// is a read it hands us. That makes a question like "did this machine ever
// execute the code that prints the boot menu?" answerable without a logic
// analyser, without a serial port, and without modifying the ROM contents at
// all -- we are already being told, we just have to keep score.
//
// Nothing here is on the reply path. Watchpoints are checked after the response
// has been queued, so a diagnostic build serves the bus identically to a normal
// one and cannot introduce the timing fault it is being used to rule out.
//
// Build with -DMPI_WATCH=ON; the status LED then blinks a frame per pass, one
// pulse per watchpoint, long for hit and short for miss. See README.

#ifndef WATCH_H
#define WATCH_H

#include <stdbool.h>
#include <stdint.h>

// Addresses in the *logical* PP address space, i.e. what the disassembly shows.
// These come from disassembling the stock monitor (tools/pdp11dis.py); each one
// is on a path that is taken only under the condition named, so a hit is a fact
// about the machine rather than an inference.
//
//   160300  the monitor's entry point. Must hit: if this one is dark the PP is
//           not starting from our vector and nothing below means anything.
//   160450  "inc r0" inside the ROM checksum loop, reached only when a block's
//           sum disagrees with the value mask-programmed into the last chip.
//           A hit is the machine telling us one of our four images is wrong.
//   160530  "bis #2, r0" -- the PP RAM test found a fault.
//   172764  the routine that prints "- ОШИБКА ..." lines. Hits when the monitor
//           believes something failed, which is worth knowing separately from
//           whether the message is legible on screen.
//   174152  the PP task dispatcher: the startup test ran to completion.
//   101000  "emt 44" with the string pointer to ЗАГРУЗКА following it -- the
//           instruction that prints the boot menu header. This is the question:
//           a hit means the menu was drawn and something happened to it after;
//           a miss means the machine never got there.
//
// Order is the LED pulse order. Keep it stable, and keep the count at or below
// MPI_WATCH_MAX so a frame stays countable by a human.
#define MPI_WATCH_ADDRS { 0160300, 0160450, 0160530, 0172764, 0174152, 0101000 }
#define MPI_WATCH_MAX   8

// A hit means "this address was read", which on a machine whose only memory in
// that range is us means "fetched" in all but pathological cases -- a data read
// of a code address would also count. The 1801VM2 does not prefetch past the
// instruction it is executing, so a not-taken branch does not produce a hit.

#endif // WATCH_H
