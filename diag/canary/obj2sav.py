#!/usr/bin/env python3
"""macro11 .OBJ (absolute .ASECT code) -> RT-11 .SAV with header + memory bitmap."""
import sys
obj, sav, start = sys.argv[1], sys.argv[2], int(sys.argv[3], 8)
data = open(obj, "rb").read()
img = bytearray(65536); lo, hi = 1 << 16, 0
i = 0
while i < len(data):
    if data[i] == 0: i += 1; continue
    assert data[i] == 1 and data[i+1] == 0
    ln = data[i+2] | (data[i+3] << 8)
    body = data[i+4:i+ln]; i += ln + 1
    if body[0] == 3:                     # TXT record
        addr = body[2] | (body[3] << 8); payload = body[4:]
        img[addr:addr+len(payload)] = payload
        lo = min(lo, addr); hi = max(hi, addr + len(payload))
hi += hi & 1
def putw(a, v): img[a] = v & 0xFF; img[a+1] = (v >> 8) & 0xFF
putw(0o40, start); putw(0o42, 0o1000); putw(0o44, 0); putw(0o50, hi)
for blk in range(0, (hi + 0o777) // 0o1000):   # byte 360 bit 7 = block 0, bit 6 = block 1, ...
    img[0o360 + blk // 8] |= 0x80 >> (blk % 8)
end = ((hi + 511) // 512) * 512
open(sav, "wb").write(img[:end])
print(f"{sav}: code {lo:o}-{hi:o}, {end//512} blocks, bitmap bytes 360..={img[0o360]:03o} {img[0o361]:03o}")
