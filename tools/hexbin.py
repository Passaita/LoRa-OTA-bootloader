#!/usr/bin/env python3
"""Intel-HEX -> bytes helper (type 0/2/4 records). Used by the bootloader-image tests."""
import sys

def load_hex(path):
    mem = {}; ext = 0
    for l in open(path):
        l = l.strip()
        if not l.startswith(':'): continue
        n = int(l[1:3], 16); a = int(l[3:7], 16); t = int(l[7:9], 16); d = bytes.fromhex(l[9:9 + 2 * n])
        if t == 4: ext = int.from_bytes(d, 'big') << 16
        elif t == 2: ext = int.from_bytes(d, 'big') << 4
        elif t == 0:
            for i, b in enumerate(d): mem[ext + a + i] = b
    return mem

def bootloader_image(path, lo=0xF4000, hi=0xFE000):
    """The bytes a node would serve: lo .. last programmed byte (0xFF gaps filled, length rounded up to 4)."""
    mem = load_hex(path)
    ks = [k for k in mem if lo <= k < hi]
    end = (max(ks) + 4) & ~3
    return bytes(mem.get(a, 0xFF) for a in range(lo, end))

if __name__ == '__main__':
    open(sys.argv[2], 'wb').write(bootloader_image(sys.argv[1]))
