#!/usr/bin/env python3
"""Remote-admin CLI over LoRa through a MeshCore companion reached over TCP (e.g. a WiFi companion on port 5000/5001).
usage: rcmd_tcp.py HOST:PORT CONTACT_NAME PASSWORD "cmd 1" ["cmd 2" ...] [--wait 30] [--pubkey HEX64] [--retries 1] [--no-flood-login]
--pubkey adds the contact (as a repeater, route unknown = flood login) if the companion does not know it yet.
Same session as the phone app's repeater command line; replies are printed with their round-trip time (or NO REPLY).

2026-09-28 (A15, N2):
* Login: if the login over the companion's stored route times out, the route is reset and the login is repeated as a FLOOD
  (board 2's stored 1-byte route 'b5' kept timing out; a flood login worked every time). --no-flood-login disables it.
* Replies are matched to commands by content where the reply format is known (ver -> "v1.", get -> "> ", fw pull-status, fw duty,
  fw route, fw slot, fw bl, stats-*): a late answer to an earlier command is printed as "LATE reply to '<cmd>'" instead of being
  taken as the answer to the current one. Read-only commands without a reply are retried --retries times (default 1); commands
  that change something are never re-sent.
"""
import argparse, asyncio, re, time
from meshcore import MeshCore, EventType

# command prefix -> regex a reply to it must match (unknown commands accept any reply)
EXPECT = [
    ("ver",            r"^v\d"),
    ("get ",           r"^> "),
    ("fw pull-status", r"^(idle|offering|downloading|verifying|COMPLETE|FAILED|WAITING|looking for)"),
    ("fw duty",        r"^(duty |OK - duty|ERR)"),
    ("fw pull-abort",  r"^OK - (aborted|search)"),
    ("fw pull ",       r"^(OK - |ERR|Pull from|remote session|looking for)"),   # a status line here = a LATE reply from an earlier session
    ("fw blupdate",    r"^(OK - |ERR|Update the BOOTLOADER|remote session|looking for)"),
    ("fw route",       r"^(route|ERR)"),
    ("fw slot",        r"^(VALID|INVALID|empty)"),
    ("fw bl",          r"^bootloader rev"),
    ("fw peers",       r"(#\d|no peers|no peer)"),
    ("fw radio",       r"(nonRX|silent|last rx|OK)"),
    ("stats-",         r"^\{"),
    ("clock",          r"\d\d:\d\d"),
]
READ_ONLY = re.compile(r"^(ver|get |fw pull-status$|fw duty$|fw route$|fw slot$|fw bl$|fw peers|fw radio$|stats-|clock$)")

def pattern(cmd):
    for pre, rx in EXPECT:
        if cmd.startswith(pre): return re.compile(rx)
    return None

def fits(cmd, text):
    p = pattern(cmd)
    return p is None or bool(p.search(text or ""))

def ts(): return time.strftime('%H:%M:%S')

async def login(mc, ct, a):
    print(f"[{ts()}] login {a.name} path_len={ct.get('out_path_len')} path={ct.get('out_path','')} hash_mode={ct.get('out_path_hash_mode')}")
    r = await mc.commands.send_login_sync(ct, a.password, timeout=a.wait)
    print("  login ->", r.type.name if r else "TIMEOUT")
    if (not r or r.type != EventType.LOGIN_SUCCESS) and a.flood_login and ct.get("out_path_len", -1) >= 0:
        rr = await mc.commands.reset_path(ct)                   # N2: the stored route may be stale - log in by flood
        print(f"[{ts()}] stored route failed: reset_path -> {rr.type.name if rr else None}, flood login")
        await mc.commands.get_contacts(); ct = mc.get_contact_by_name(a.name)
        r = await mc.commands.send_login_sync(ct, a.password, timeout=a.wait)
        print("  flood login ->", r.type.name if r else "TIMEOUT")
    return ct, (r is not None and r.type == EventType.LOGIN_SUCCESS)

async def main(a):
    host, port = a.hostport.rsplit(":", 1)
    mc = await MeshCore.create_tcp(host, int(port))
    if mc is None: print("companion not answering on", a.hostport); return 2
    await mc.commands.get_contacts()
    ct = mc.get_contact_by_name(a.name)
    if not ct and a.pubkey:
        c = {"public_key": a.pubkey.lower(), "type": 2, "flags": 0, "out_path_len": -1, "out_path": "", "out_path_hash_mode": 0,
             "adv_name": a.name, "last_advert": 0, "adv_lat": 0.0, "adv_lon": 0.0}
        r = await mc.commands.add_contact(c); print("contact added:", r.type.name)
        await mc.commands.get_contacts(); ct = mc.get_contact_by_name(a.name)
    if not ct: print("contact not found:", a.name, "(give --pubkey)"); await mc.disconnect(); return 2
    ct, ok = await login(mc, ct, a)
    if not ok: await mc.disconnect(); return 1
    q = asyncio.Queue(); mc.subscribe(EventType.CONTACT_MSG_RECV, lambda e: q.put_nowait(e))
    await mc.start_auto_message_fetching()
    unanswered = []                                              # earlier commands still without a reply (a late one may come)
    rc = 0
    for c in a.cmds:
        tries = 1 + (a.retries if READ_ONLY.match(c) else 0)
        got = None
        for attempt in range(tries):
            t0 = time.time(); await mc.commands.send_cmd(ct, c)
            deadline = t0 + a.wait
            while got is None and time.time() < deadline:
                try: e = await asyncio.wait_for(q.get(), max(0.1, deadline - time.time()))
                except asyncio.TimeoutError: break
                text = e.payload.get("text")
                if fits(c, text) and not any(fits(u, text) and pattern(u) is not None and pattern(c) is None for u in unanswered):
                    got = (time.time() - t0, text)
                else:
                    owner = next((u for u in unanswered if fits(u, text)), None)
                    print(f"[{ts()}]   LATE reply to {owner!r}: {text}" if owner else f"[{ts()}]   unmatched reply (not for {c!r}): {text}")
                    if owner: unanswered.remove(owner)
            if got: break
            if attempt + 1 < tries: print(f"[{ts()}] {c!r} -> NO REPLY in {a.wait}s, retrying (read-only)")
        if got:
            print(f"[{ts()}] {c!r} -> ({got[0]:.1f}s) {got[1]}")
        else:
            print(f"[{ts()}] {c!r} -> NO REPLY in {a.wait}s" + ("" if READ_ONLY.match(c) else " (not re-sent: it may still have been executed)"))
            unanswered.append(c); rc = 3
    await mc.disconnect(); return rc

if __name__ == "__main__":
    ap = argparse.ArgumentParser(); ap.add_argument("hostport"); ap.add_argument("name"); ap.add_argument("password"); ap.add_argument("cmds", nargs="+")
    ap.add_argument("--wait", type=float, default=30); ap.add_argument("--pubkey")
    ap.add_argument("--retries", type=int, default=1, help="re-send a read-only command that got no reply (default 1)")
    ap.add_argument("--no-flood-login", dest="flood_login", action="store_false", help="do not fall back to a flood login")
    raise SystemExit(asyncio.run(main(ap.parse_args())))
