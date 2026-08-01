"""The self-test pattern, shared by the generator and the checker.

Flashing a real ROM image and dumping it back tells you pass or fail. Flashing
a pattern whose contents encode their own address tells you *what* went wrong,
which on a multiplexed bus is most of the work: a scrambled address line, a data
line that never drives, and the wrong window answering all look alike in a dump
of real code.

    word(k, i) = (i & 0xFFF) | ((((i & 0xF) ^ k) & 0xF) << 12)

The low twelve bits are the word index outright. The top four are the index's
low nibble XOR the window number, which does three jobs at once: every data line
toggles inside a single window (a plain ramp would leave the top four stuck),
the top nibble checksums the bottom so a wrong word is detectable, and the
window number is recoverable from the data alone -- so a dump proves *which*
window answered, not merely that something did.

The window term has to differ per nibble to be detectable. A nibble-uniform
constant such as k * 0x1111 cancels out of the checksum exactly, and every
window then validates as every other.
"""

WORD_MASK = 0xFFFF
WORDS = 4096
WINDOWS = 8


def word(window: int, index: int) -> int:
    top = ((index & 0xF) ^ (window & 0xF)) & 0xF
    return ((index & 0xFFF) | (top << 12)) & WORD_MASK


def image(window: int) -> list:
    return [word(window, i) for i in range(WORDS)]


def recover(window: int, w: int):
    """Return the index this word claims to be, or None if it is not ours."""
    index = w & 0xFFF
    if ((w >> 12) & 0xF) != (((index & 0xF) ^ (window & 0xF)) & 0xF):
        return None
    return index


def identify(w: int, index: int):
    """Which window would make this word valid at this index?"""
    return (((w >> 12) & 0xF) ^ (index & 0xF)) & 0xF
