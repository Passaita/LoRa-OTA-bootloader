#!/usr/bin/env python3
"""Send CLI lines to a RAK board over USB serial and print the replies.
usage: sercmd.py <USB serial, e.g. <USB serial number> | /dev/serial/by-id/...> "cmd 1" ["cmd 2" ...]
Address boards by USB serial, never by /dev/ttyACMn (the XIAO companion is usually ttyACM0)."""
import glob, serial, sys, time
if len(sys.argv) < 3: sys.exit(__doc__)
dev = sys.argv[1]
if not dev.startswith("/dev/"):
    m = glob.glob(f"/dev/serial/by-id/*{dev}*")
    if len(m) != 1: sys.exit(f"no unique serial device for {dev!r}: {m}")
    dev = m[0]
p = serial.Serial(dev, 115200, timeout=0.3)
time.sleep(0.3); p.read(8192)
for c in sys.argv[2:]:
    p.write((c + "\r").encode()); t0 = time.time(); out = b""
    while time.time() - t0 < 4:
        d = p.read(4096); out += d
        if d == b"" and out: break
    print("=== " + c + "\n" + out.decode(errors="replace").strip())
p.close()
