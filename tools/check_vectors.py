#!/usr/bin/env python3
"""Check that a built bootloader image keeps its interrupt/SVC handlers.

With -flto the linker can drop every C handler that only the assembly vector
table references, leaving the startup file's weak `b .` default in every vector
(the bootloader then hangs on the first SVC / SysTick / USB / SoftDevice event).
This was found on hardware on 2026-09-18 (and very likely caused the
2026-09-16 brick). This script fails the build if that happens again.

Usage: check_vectors.py <bootloader.out (ELF)> <bootloader .hex> [bootloader_base=0xF4000]
"""
import os, shutil, subprocess, sys
from intelhex import IntelHex

elf, hexf = sys.argv[1], sys.argv[2]
base = int(sys.argv[3], 0) if len(sys.argv) > 3 else 0xF4000
NM = shutil.which('arm-none-eabi-nm') or shutil.which('arm-none-eabi-nm', path=os.path.expanduser('~/.platformio/packages/toolchain-gccarmnoneeabi/bin'))
if not NM:
    sys.exit('check_vectors: arm-none-eabi-nm not found (put the toolchain on PATH) - treat as FAILED, do not flash')

syms = {}
for line in subprocess.run([NM, elf], capture_output=True, text=True, check=True).stdout.splitlines():
    p = line.split()
    if len(p) == 3: syms[p[2]] = (int(p[0], 16), p[1])

ih = IntelHex(hexf)
def vec(off):
    return int.from_bytes(bytes(ih[base + off + i] for i in range(4)), 'little') & ~1

default = syms['Default_Handler'][0]
# handler name -> vector offset (16 core exception slots + 4 * IRQ number)
EXPECT = {
    'SVC_Handler': 0x2C,
    'SysTick_Handler': 0x3C,
    'POWER_CLOCK_IRQHandler': 0x40 + 4 * 0,
    'RTC1_IRQHandler': 0x40 + 4 * 17,
    'SWI0_EGU0_IRQHandler': 0x40 + 4 * 20,
    'SWI2_EGU2_IRQHandler': 0x40 + 4 * 22,
    'USBD_IRQHandler': 0x40 + 4 * 39,
}
bad = 0
for name, off in EXPECT.items():
    addr, kind = syms.get(name, (None, None))
    v = vec(off)
    ok = kind == 'T' and v == addr and v != default
    bad += not ok
    print(f"  {'ok ' if ok else 'BAD'} {name:26s} vector[+0x{off:02X}]=0x{v:08X}  symbol={'strong' if kind == 'T' else 'WEAK/missing'} @ {hex(addr) if addr else '-'}")
print('vectors: ' + ('handlers are missing - do NOT flash this image' if bad else 'all expected handlers are linked and wired'))
sys.exit(1 if bad else 0)
