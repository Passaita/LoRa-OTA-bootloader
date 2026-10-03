#!/usr/bin/env python3
"""Serial CLI for an ESP32-S3 board on its USB-JTAG port (Heltec V4): usage esp_cli.py "cmd" ["cmd" ...]
Opens with DTR/RTS held low; opening can still reboot the board, so it waits 4 s before the first command (settings survive)."""
# serial CLI for an ESP32-S3 USB-JTAG port WITHOUT toggling DTR/RTS (toggling resets the chip / enters download mode)
import glob,sys,time,serial
s=serial.Serial(); s.port=glob.glob('/dev/serial/by-id/*Espressif*')[0]; s.baudrate=115200; s.timeout=0.2; s.dtr=False; s.rts=False; s.open()
time.sleep(4); s.read(8192)
for c in sys.argv[1:]:
    s.write((c+"\r").encode()); t=time.time(); out=b""
    while time.time()-t<2.5:
        out+=s.read(4096)
        if out.rstrip().endswith(b">") is False and b"\n" in out and time.time()-t>0.8: break
    print(f"{c!r:32} -> {out.decode('utf-8','replace').strip()!r}")
