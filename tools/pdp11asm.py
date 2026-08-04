"""A very small PDP-11 assembler -- enough to write test ROMs for the PP.

Not a general assembler. It covers the instructions a hardware test needs:
move things, compare them, count, branch, and stop. Anything cleverer is
better written elsewhere and pasted in as .word.

Numbers are OCTAL unless prefixed (0x/0o/0b) or suffixed with a dot for
decimal, which is the PDP-11 convention -- see _num.

Operand syntax:
    r0..r5, sp, pc      register
    (r0)                register deferred
    (r0)+               autoincrement
    #40 / #10.          immediate, octal / decimal
    @#177716            absolute
"""

import re

REGS = {f"r{i}": i for i in range(8)} | {"sp": 6, "pc": 7}

DOUBLE = {"mov": 0o010000, "cmp": 0o020000, "bit": 0o030000,
          "bic": 0o040000, "bis": 0o050000, "add": 0o060000,
          "movb": 0o110000, "cmpb": 0o120000, "sub": 0o160000}
SINGLE = {"clr": 0o005000, "com": 0o005100, "inc": 0o005200,
          "dec": 0o005300, "neg": 0o005400, "tst": 0o005700,
          "asr": 0o006200, "asl": 0o006300, "sxt": 0o006700,
          "swab": 0o000300,
          "clrb": 0o105000, "comb": 0o105100, "tstb": 0o105700}
BRANCH = {"br": 0o000400, "bne": 0o001000, "beq": 0o001400,
          "bge": 0o002000, "blt": 0o002400, "bgt": 0o003000,
          "ble": 0o003400, "bpl": 0o100000, "bmi": 0o100400,
          "bhi": 0o101000, "blos": 0o101400, "bvc": 0o102000,
          "bvs": 0o102400, "bcc": 0o103000, "bhis": 0o103000,
          "bcs": 0o103400, "blo": 0o103400}
# Source is a register, destination is a general operand: dst ^= reg.
REGSRC = {"xor": 0o074000}
SIMPLE = {"halt": 0o000000, "wait": 0o000001, "reset": 0o000005,
          "nop": 0o000240, "rti": 0o000002, "return": 0o000207}


class AsmError(Exception):
    pass


def _num(tok, labels, strict=True):
    """Evaluate a number or label.  Bare digits are OCTAL, as on a PDP-11.

    This used to be int(tok, 0), i.e. Python's rules, which made a bare 177716
    decimal and assembled it as 133064 -- a wrong program, silently, with the
    write landing in the middle of the ROM instead of on a device register. On a
    machine whose every address, opcode and register is conventionally written
    in octal, octal is the only sane default.

    Explicit prefixes still work for the rare non-octal constant, and a trailing
    dot is decimal in the PDP-11 tradition: 10. is ten, 10 is eight.
    """
    tok = tok.strip()
    if tok in labels:
        return labels[tok]
    try:
        if tok.endswith("."):
            return int(tok[:-1], 10)
        if tok[:2].lower() in ("0x", "0o", "0b"):
            return int(tok, 0)
        return int(tok, 8)
    except ValueError:
        pass
    if strict:
        raise AsmError(f"cannot evaluate {tok!r}")
    return 0        # first pass: a forward label, resolved on the second


def _operand(tok, labels, strict=True):
    """Return (mode_reg_field, [extra_words])."""
    tok = tok.strip()
    if tok in REGS:
        return REGS[tok], []
    m = re.fullmatch(r"\((\w+)\)\+", tok)
    if m and m.group(1) in REGS:
        return 0o20 | REGS[m.group(1)], []
    m = re.fullmatch(r"\((\w+)\)", tok)
    if m and m.group(1) in REGS:
        return 0o10 | REGS[m.group(1)], []
    if tok.startswith("@#"):
        return 0o37, [_num(tok[2:], labels, strict) & 0xFFFF]
    if tok.startswith("#"):
        return 0o27, [_num(tok[1:], labels, strict) & 0xFFFF]
    raise AsmError(f"unsupported operand {tok!r}")


def assemble(source, origin):
    """Assemble to (origin, [words]).  Two passes, so labels can be forward."""
    lines = []
    for raw in source.splitlines():
        line = raw.split(";")[0].strip()
        if line:
            lines.append(line)

    labels, pc = {}, origin
    for _pass in (1, 2):
        strict = _pass == 2
        out, pc = [], origin
        for line in lines:
            while ":" in line:
                name, _, line = line.partition(":")
                labels[name.strip()] = pc
                line = line.strip()
            if not line:
                continue
            op, _, rest = line.partition(" ")
            op = op.lower()
            args = [a for a in rest.split(",") if a.strip()]

            if op == ".word":
                words = [_num(a, labels, strict) & 0xFFFF for a in args]
            elif op in SIMPLE:
                words = [SIMPLE[op]]
            elif op in BRANCH:
                target = _num(args[0], labels, strict)
                off = (target - (pc + 2)) // 2
                if strict and not -128 <= off <= 127:
                    raise AsmError(f"branch out of range at {pc:06o}")
                words = [BRANCH[op] | (off & 0xFF)]
            elif op in DOUBLE:
                src, sx = _operand(args[0], labels, strict)
                dst, dx = _operand(args[1], labels, strict)
                words = [DOUBLE[op] | (src << 6) | dst] + sx + dx
            elif op in REGSRC:
                if args[0].strip() not in REGS:
                    raise AsmError(f"{op} needs a register source")
                r = REGS[args[0].strip()]
                dst, dx = _operand(args[1], labels, strict)
                words = [REGSRC[op] | (r << 6) | dst] + dx
            elif op in SINGLE:
                dst, dx = _operand(args[0], labels, strict)
                words = [SINGLE[op] | dst] + dx
            else:
                raise AsmError(f"unknown instruction {op!r}")
            out += words
            pc += 2 * len(words)
    return origin, out
