#!/usr/bin/env python3
"""Remote-admin CLI over LoRa through a USB MeshCore companion (e.g. the XIAO ESP32S3 + Wio SX1262).

Usage: rcmd.py [--port /dev/ttyACM0] [--wait 20] CONTACT_NAME PASSWORD "cmd 1" ["cmd 2" ...]
Logs in to CONTACT_NAME (a repeater) with PASSWORD, then sends each command as a CLI line and prints
the reply text (or NO REPLY after --wait seconds). Same path as the phone app's repeater command line.
"""
import argparse, asyncio, time
from meshcore import MeshCore, EventType


def reset_companion(port):
    """ESP32-S3 USB-JTAG: RTS pulse = chip reset (same as esptool --after hard_reset). The companion's USB serial
    sometimes stops answering after the host closes the port; a reset brings it back (settings are kept)."""
    import serial
    s = serial.Serial(port); s.dtr = False; s.rts = True; time.sleep(0.2); s.rts = False; s.close(); time.sleep(3)
    drain(port)


def drain(port, quiet_s=2.0, max_s=15.0):
    """Read and discard until the companion has been quiet for quiet_s: it pushes a burst of queued frames after a
    reset / reconnect, and the library's APP_START handshake fails if it lands inside that burst."""
    import serial
    for _ in range(20):
        try: p = serial.Serial(port, 115200, timeout=0.2); break
        except Exception: time.sleep(0.5)
    t0 = last = time.time()
    while time.time() - last < quiet_s and time.time() - t0 < max_s:
        if p.read(4096): last = time.time()
    p.close()


async def main(a):
    drain(a.port)
    mc = await MeshCore.create_serial(port=a.port, baudrate=115200)
    if mc is None:
        print("companion not answering - resetting it"); reset_companion(a.port)
        mc = await MeshCore.create_serial(port=a.port, baudrate=115200)
        if mc is None: print("companion still not answering"); return 2
    await mc.commands.get_contacts()
    ct = mc.get_contact_by_name(a.name)
    if not ct:
        print("contact not found:", a.name); return 2
    print(f"[{time.strftime('%H:%M:%S')}] login {a.name} path_len={ct.get('out_path_len')}")
    r = await mc.commands.send_login_sync(ct, a.password, timeout=a.wait)
    print("  login ->", r.type.name if r else "TIMEOUT", (r.payload if r else ""))
    if not r or r.type != EventType.LOGIN_SUCCESS:
        await mc.disconnect(); return 1
    q = asyncio.Queue()
    mc.subscribe(EventType.CONTACT_MSG_RECV, lambda e: q.put_nowait(e))
    if a.rxlog:   # over-the-air receive times at the companion (separates radio delay from host fetch delay)
        mc.subscribe(EventType.RX_LOG_DATA, lambda e: print(f"    [{time.strftime('%H:%M:%S')}] rx {e.payload.get('payload_typename', e.payload.get('payload_type', '?'))} "
                                                             f"snr={e.payload.get('snr')} len={len(str(e.payload.get('payload', '')))//2}"))
        mc.subscribe(EventType.MESSAGES_WAITING, lambda e: print(f"    [{time.strftime('%H:%M:%S')}] MESSAGES_WAITING"))
    await mc.start_auto_message_fetching()
    for c in a.cmds:
        while not q.empty(): q.get_nowait()
        t0 = time.time()
        await mc.commands.send_cmd(ct, c)
        try:
            e = await asyncio.wait_for(q.get(), a.wait)
            print(f"[{time.strftime('%H:%M:%S')}] {c!r} -> ({time.time()-t0:.1f}s) {e.payload.get('text')}")
        except asyncio.TimeoutError:
            print(f"[{time.strftime('%H:%M:%S')}] {c!r} -> NO REPLY in {a.wait}s")
    await mc.disconnect()
    return 0


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", default="/dev/ttyACM0"); ap.add_argument("--wait", type=float, default=20)
    ap.add_argument("--rxlog", action="store_true", help="print every packet the companion hears, with time")
    ap.add_argument("name"); ap.add_argument("password"); ap.add_argument("cmds", nargs="+")
    raise SystemExit(asyncio.run(main(ap.parse_args())))
