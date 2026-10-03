#!/usr/bin/env python3
"""Time-limited USB serial reader for an ESP32 (Espressif) port, survives the port disappearing/reappearing (boot loops)."""
# time-limited serial reader (120 s); opens without touching DTR/RTS (pyserial asserts them on open unless preset)
import glob,time,serial
end=time.time()+int(__import__("sys").argv[1]) if len(__import__("sys").argv)>1 else time.time()+60
while time.time()<end:
    ps=glob.glob('/dev/serial/by-id/*Espressif*')
    if not ps: time.sleep(0.1); continue
    try:
        s=serial.Serial(); s.port=ps[0]; s.baudrate=115200; s.timeout=0.3; s.dtr=False; s.rts=False; s.open()
        print(f"\n[{time.strftime('%T')}] open {ps[0]}",flush=True)
        while time.time()<end:
            d=s.read(4096)
            if d: print(d.decode('utf-8','replace'),end='',flush=True)
        s.close()
    except Exception as e: print(f"\n[{time.strftime('%T')}] port gone: {e}",flush=True); time.sleep(0.2)
