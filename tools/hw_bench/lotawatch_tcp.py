#!/usr/bin/env python3
"""Decode LoRa-OTA security-v2 discovery traffic (v1.17.28+) heard by a MeshCore companion over TCP - passive, transmits nothing.
Knowing the circle passphrase, it derives the circle key C like the node (PBKDF2-HMAC-SHA256, 50,000 rounds, salt "LoRa-OTA circle v1"),
the discovery channel (HMAC(C, "lota2-disc")) and the list key, then decrypts every GRP_TXT on that channel:
  DISC  "LOTA1 <sess><seq> <target> [F]"    a discovery (fw list '*', or a pull looking for '@ID8')
  RSP   LOTA1R + LIST_RSP                    a flooded answer: server id, fw version, kind, the route it carries, MAC ok?
  HEAL  LOTA1H + LIST_RSP                    a server's heal notice
RAW_CUSTOM (the transfer protocol, zero-hop or direct) is shown by type byte only (its payload is MACed/encrypted per session).
usage: lotawatch_tcp.py HOST:PORT --phrase "..." [--listen 120] [--log FILE] [--login NAME PW "cmd" sleep:25 "cmd" ...]"""
import argparse, asyncio, hashlib, hmac, struct, time
from Crypto.Cipher import AES
from meshcore import MeshCore, EventType

LOTA_TYPES = {1: "OFFER_REQ", 2: "OFFER_ACK", 3: "OFFER_NAK", 4: "CHUNK_REQ", 5: "CHUNK", 7: "ABORT", 8: "LIST_REQ", 9: "LIST_RSP", 10: "BAD_AUTH", 11: "OFFER_REQ2"}
RT = {0: "T-FLOOD", 1: "FLOOD", 2: "DIRECT", 3: "T-DIRECT"}

def keys(phrase):
    c = hashlib.pbkdf2_hmac("sha256", phrase.encode(), b"LoRa-OTA circle v1", 50000, 32)
    disc = hmac.new(c, b"lota2-disc", hashlib.sha256).digest()          # the channel secret (32 B; AES uses the first 16)
    lst = hmac.new(c, b"lota2-list", hashlib.sha256).digest()[:16]
    return c, disc, hashlib.sha256(disc).digest()[0], lst

def parse(raw):
    h = raw[0]; i = 1; rt, pt = h & 3, (h >> 2) & 15
    if rt in (0, 3): i += 4
    pl = raw[i]; i += 1; hs, hc = (pl >> 6) + 1, pl & 63
    path = [raw[i + k * hs:i + (k + 1) * hs].hex() for k in range(hc)]; i += hs * hc
    return RT[rt], pt, hs, path, raw[i:]

def decrypt(secret, body):
    mac, ct = body[1:3], body[3:]
    if not ct or len(ct) % 16: return None
    if hmac.new(secret, ct, hashlib.sha256).digest()[:2] != mac: return None
    return AES.new(secret[:16], AES.MODE_ECB).decrypt(ct)

def list_rsp(b, lkey):
    if len(b) < 46 + 8 or b[0] != 0xF7 or b[1] != 9: return "not a LIST_RSP"
    enc = b[45]; hs, hc = (enc >> 6) + 1, enc & 63; n = 46 + hs * hc + 8
    if n > len(b): return f"bad length (route enc {enc:#x})"
    ok = hmac.new(lkey, b[:n - 8], hashlib.sha256).digest()[:8] == b[n - 8:n]
    sess, seq = struct.unpack_from("<IH", b, 2); fwv, plen = struct.unpack_from("<II", b, 12)
    name = b[20:36].rstrip(b"\0").decode(errors="replace"); kind = "bl" if b[36] == 1 else "app"
    route = [b[46 + k * hs:46 + (k + 1) * hs].hex() for k in range(hc)]
    return f"server {b[8:12].hex().upper()} {name!r} {kind}-{fwv} for sess {sess:08X}/{seq} route {route} MAC {'OK' if ok else 'BAD'}"

async def main(a):
    c, secret, chash, lkey = keys(a.phrase)
    fp = hashlib.sha256(c).digest()[:4].hex().upper()
    host, port = a.hostport.rsplit(":", 1)
    mc = await MeshCore.create_tcp(host, int(port))
    if mc is None: print("companion not answering on", a.hostport); return 2
    await mc.commands.get_contacts()
    nm = {k.lower(): v.get("adv_name", "") for k, v in mc.contacts.items()}
    log = open(a.log, "a") if a.log else None
    def out(s):
        s = f"[{time.strftime('%H:%M:%S')}] {s}"; print(s, flush=True)
        if log: log.write(s + "\n"); log.flush()
    def who(hx):
        m = [n for k, n in nm.items() if k.startswith(hx)]
        return f"{hx}({'/'.join(m[:2])})" if m else hx
    out(f"circle fingerprint {fp}, discovery channel hash {chash:02X}, listening {a.listen} s")
    def on_rx(e):
        d = e.payload
        try: rt, pt, hs, path, body = parse(bytes.fromhex(d["payload"]))
        except Exception: return
        pth = "[" + ", ".join(who(p) for p in path) + "]"; q = f"snr={d.get('snr')} rssi={d.get('rssi')}"
        if pt == 5 and body and body[0] == chash:                       # GRP_TXT on the discovery channel
            p = decrypt(secret, body)
            if p is None: out(f"{q} {rt} GRP_TXT disc-channel hash, MAC/length BAD {pth}"); return
            txt = p[5:]
            if txt[:6] in (b"LOTA1R", b"LOTA1H"): out(f"{q} {rt} {'RSP ' if txt[5:6] == b'R' else 'HEAL'} {list_rsp(txt[6:], lkey)} {pth}")
            else: out(f"{q} {rt} DISC {txt.rstrip(bytes(1)).decode(errors='replace')!r} {pth}")
        elif pt == 15 and body and body[0] == 0xF7:
            out(f"{q} {rt} RAW {LOTA_TYPES.get(body[1], body[1])} ({len(body)} B) {pth}")
    mc.subscribe(EventType.RX_LOG_DATA, on_rx)
    if a.login:                                              # optional: run admin commands on one node in the same session
        name, pw, *cmds = a.login
        ct = mc.get_contact_by_name(name)
        r = await mc.commands.send_login_sync(ct, pw, timeout=30) if ct else None
        if ct and (not r or r.type != EventType.LOGIN_SUCCESS):      # stored path failed: forget it, log in by flood (learns a new path)
            out(f"login {name} -> {r.type.name if r else 'TIMEOUT'} on the stored path, flood login")
            await mc.commands.reset_path(ct); r = await mc.commands.send_login_sync(ct, pw, timeout=40)
        out(f"login {name} -> {r.type.name if r else 'TIMEOUT'}")
        if r and r.type == EventType.LOGIN_SUCCESS:
            q = asyncio.Queue(); mc.subscribe(EventType.CONTACT_MSG_RECV, lambda e: q.put_nowait(e))
            await mc.start_auto_message_fetching()
            for c in cmds:
                if c.startswith("sleep:"): await asyncio.sleep(float(c[6:])); continue
                while not q.empty(): q.get_nowait()
                t0 = time.time(); await mc.commands.send_cmd(ct, c)
                try: e = await asyncio.wait_for(q.get(), timeout=a.wait); out(f"  {c!r} -> {e.payload.get('text')!r} ({time.time() - t0:.1f} s)")
                except asyncio.TimeoutError: out(f"  {c!r} -> NO REPLY ({a.wait} s)")
    await asyncio.sleep(a.listen); await mc.disconnect()

if __name__ == "__main__":
    p = argparse.ArgumentParser(); p.add_argument("hostport"); p.add_argument("--phrase", required=True)
    p.add_argument("--listen", type=int, default=120); p.add_argument("--log"); p.add_argument("--wait", type=int, default=30)
    p.add_argument("--login", nargs="+", metavar="NAME PW CMD", help="node, password, commands ('sleep:N' pauses N s)")
    raise SystemExit(asyncio.run(main(p.parse_args())))
