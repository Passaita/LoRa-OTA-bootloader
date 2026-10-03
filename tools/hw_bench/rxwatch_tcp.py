#!/usr/bin/env python3
"""Listen to everything a MeshCore companion (reached over TCP) hears on the air, decoded, while optionally running remote-admin commands.
usage: rxwatch_tcp.py HOST:PORT [--login NAME PW "cmd 1" "cmd 2" ...] [--listen 60] [--pre 0] [--all] [--log FILE]
One TCP session does both (never open a second client on the companion at the same time).
Default output: adverts (originator name + the relay hashes they collected) and any packet whose path or advert involves --watch prefixes;
--all prints every packet. Relayed adverts leave no record on a stock repeater (its 'neighbors' list is zero-hop only), so this is the
way to see how far a node's advert travels and through which relays."""
import argparse, asyncio, time
from meshcore import MeshCore, EventType

PT = {0: "REQ", 1: "RESP", 2: "TXT", 3: "ACK", 4: "ADVERT", 5: "GRP_TXT", 6: "GRP_DATA", 7: "ANON_REQ", 8: "PATH", 9: "TRACE",
      10: "MULTIPART", 11: "CONTROL", 15: "RAW_CUSTOM"}
RT = {0: "T-FLOOD", 1: "FLOOD", 2: "DIRECT", 3: "T-DIRECT"}

def parse(raw: bytes):
    h = raw[0]; i = 1
    rt, pt = h & 3, (h >> 2) & 15
    if rt in (0, 3): i += 4                       # transport codes
    pl = raw[i]; i += 1
    hs, hc = (pl >> 6) + 1, pl & 63
    path = [raw[i + k * hs:i + (k + 1) * hs].hex() for k in range(hc)]
    i += hs * hc
    return RT[rt], PT.get(pt, str(pt)), hs, path, raw[i:]

def names(contacts):
    return {k.lower(): c.get("adv_name", "") for k, c in contacts.items()}

async def main(a):
    host, port = a.hostport.rsplit(":", 1)
    mc = await MeshCore.create_tcp(host, int(port))
    if mc is None: print("companion not answering on", a.hostport); return 2
    await mc.commands.get_contacts()
    nm = names(mc.contacts)
    watch = [w.lower() for w in a.watch]
    log = open(a.log, "a") if a.log else None
    def out(s):
        s = f"[{time.strftime('%H:%M:%S')}] {s}"; print(s, flush=True)
        if log: log.write(s + "\n"); log.flush()
    def who(hx):                                   # relay hash -> known names with that prefix (repeaters first)
        m = [n for k, n in nm.items() if k.startswith(hx)]
        return f"{hx}({'/'.join(m[:2])})" if m else hx
    def on_rx(e):
        d = e.payload
        try: rt, pt, hs, path, body = parse(bytes.fromhex(d["payload"]))
        except Exception: return
        orig = ""
        if pt == "ADVERT" and len(body) >= 32:
            pk = body[:32].hex(); orig = f" from {pk[:8]} {nm.get(pk, '?')!r}"
        hit = any(orig[6:6 + len(w)] == w or any(p.startswith(w) or w.startswith(p) for p in path) for w in watch)
        if a.all or hit or (pt == "ADVERT" and not watch):
            out(f"rx snr={d.get('snr')} rssi={d.get('rssi')} {rt} {pt}{orig} hash={hs}B path=[{', '.join(who(p) for p in path)}]"
                + ("  <== WATCHED" if hit else ""))
    mc.subscribe(EventType.RX_LOG_DATA, on_rx)
    if a.pre: out(f"listening {a.pre} s before the commands"); await asyncio.sleep(a.pre)
    if a.login:
        name, pw, *cmds = a.login
        ct = mc.get_contact_by_name(name)
        if not ct: out(f"contact not found: {name}"); await mc.disconnect(); return 2
        out(f"login {name} path_len={ct.get('out_path_len')} path={ct.get('out_path', '')}")
        r = await mc.commands.send_login_sync(ct, pw, timeout=30)
        out(f"  login -> {r.type.name if r else 'TIMEOUT'}")
        if r and r.type == EventType.LOGIN_SUCCESS:
            q = asyncio.Queue(); mc.subscribe(EventType.CONTACT_MSG_RECV, lambda e: q.put_nowait(e))
            await mc.start_auto_message_fetching()
            for c in cmds:
                while not q.empty(): q.get_nowait()
                t0 = time.time(); await mc.commands.send_cmd(ct, c)
                try:
                    e = await asyncio.wait_for(q.get(), timeout=a.wait)
                    out(f"  {c!r} -> {e.payload.get('text')!r} ({time.time() - t0:.1f} s)")
                except asyncio.TimeoutError:
                    out(f"  {c!r} -> NO REPLY ({a.wait} s)")
    out(f"listening {a.listen} s"); await asyncio.sleep(a.listen)
    await mc.disconnect()

if __name__ == "__main__":
    p = argparse.ArgumentParser()
    p.add_argument("hostport")
    p.add_argument("--login", nargs="+", metavar="NAME PW CMD", help="contact name, password, then commands")
    p.add_argument("--listen", type=int, default=60, help="seconds to keep listening after the commands")
    p.add_argument("--pre", type=int, default=0, help="seconds to listen before the commands (baseline)")
    p.add_argument("--wait", type=int, default=30, help="seconds to wait for each command reply")
    p.add_argument("--watch", nargs="*", default=[], help="hex prefixes (node id / relay hash) to highlight; default = all adverts")
    p.add_argument("--all", action="store_true", help="print every packet")
    p.add_argument("--log", help="append the output to this file")
    raise SystemExit(asyncio.run(main(p.parse_args())))
