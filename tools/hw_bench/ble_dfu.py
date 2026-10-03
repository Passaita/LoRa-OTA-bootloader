#!/usr/bin/env python3
"""Minimal Nordic *legacy* (SDK11 / Adafruit bootloader) BLE DFU client for Linux, using bleak.

Uploads an adafruit-nrfutil / PlatformIO DFU package (.zip with manifest.json, firmware.bin,
firmware.dat) to a board sitting in bootloader BLE OTA mode (advertising "3401_DFU").
Written so BLE OTA behaviour (accept / reject / interruption) can be tested from the laptop
without a phone. Protocol per the bootloader's own source (ble_dfu.h / dfu_transport_ble.c):

  service 00001530-1212-EFDE-1523-785FEABCD123, control point ..1531.., packet ..1532..
  CP opcodes: 01 START(type 04=app) | 02 INIT (00=begin, 01=complete) | 03 RECEIVE | 04 VALIDATE
              05 ACTIVATE+RESET | 08 PRN request (uint16)
  CP notifications: 10 <opcode> <status>  (status 1 = success) | 11 <uint32 bytes received> (PRN)

Usage: ble_dfu.py PACKAGE.zip [--name 3401_DFU] [--prn 8] [--chunk 0] [--stop-after BYTES] [--quiet]
  --stop-after N : drop the connection after N image bytes (simulates an interrupted upload)
Exit code: 0 upload activated, 1 rejected/failed, 2 could not connect.
Large packets that stall, or that the bootloader rejects mid-transfer (OPER_FAILED), are retried once with 20-byte packets automatically.
After an interrupted upload the bootloader keeps its half-finished DFU session (so it can be resumed) and
refuses a new START with INVALID_STATE; this client then sends DFU RESET (opcode 6) and retries once.

NEVER halt the target over SWD while it is in BLE OTA (it can crash the SoftDevice).
Do not run bluetoothctl scans at the same time as this script.
"""
import argparse, asyncio, json, struct, sys, time, zipfile

from bleak import BleakClient, BleakScanner
from bleak.args.bluez import BlueZDiscoveryFilters, BlueZScannerArgs

SVC = "00001530-1212-efde-1523-785feabcd123"
CP = "00001531-1212-efde-1523-785feabcd123"
PKT = "00001532-1212-efde-1523-785feabcd123"
STATUS = {1: "SUCCESS", 2: "INVALID_STATE", 3: "NOT_SUPPORTED", 4: "DATA_SIZE", 5: "CRC_ERROR", 6: "OPER_FAILED"}
OPNAME = {1: "START", 2: "INIT", 3: "RECEIVE", 4: "VALIDATE", 5: "ACTIVATE", 8: "PRN"}


class Dfu:
    def __init__(self, client, quiet):
        self.c = client; self.quiet = quiet
        self.resp = asyncio.Queue(); self.prn = asyncio.Event(); self.prn_bytes = 0

    def log(self, *a):
        if not self.quiet: print(*a, flush=True)

    def on_notify(self, _h, data: bytearray):
        d = bytes(data)
        if d[0] == 0x10 and len(d) >= 3:
            self.resp.put_nowait((d[1], d[2]))
        elif d[0] == 0x11 and len(d) >= 5:
            self.prn_bytes = struct.unpack_from("<I", d, 1)[0]; self.prn.set()
        else:
            self.log("  ?? notification", d.hex())

    async def cp(self, payload: bytes):
        await self.c.write_gatt_char(CP, payload, response=True)

    async def expect(self, op, timeout=30):
        try:
            got_op, st = await asyncio.wait_for(self.resp.get(), timeout)
        except asyncio.TimeoutError:
            raise RuntimeError(f"timeout waiting for the response to {OPNAME.get(op, op)}")
        if got_op != op:
            raise RuntimeError(f"unexpected response for {OPNAME.get(got_op, got_op)} while waiting for {OPNAME.get(op, op)}")
        if st != 1:
            raise RuntimeError(f"{OPNAME.get(op, op)} rejected by the bootloader: {STATUS.get(st, st)}")


async def upload(args):
    z = zipfile.ZipFile(args.package)
    mf = json.loads(z.read("manifest.json"))["manifest"]
    # legacy DFU packages carry ONE kind of update: application | bootloader | softdevice | softdevice_bootloader
    # (bootloader + application in one package is not supported by the bootloader)
    kind = next(k for k in ("application", "bootloader", "softdevice", "softdevice_bootloader") if k in mf)
    man = mf[kind]
    fw, dat = z.read(man["bin_file"]), z.read(man["dat_file"])
    sd_size = man.get("sd_size", 0); bl_size = man.get("bl_size", 0)
    if kind == "bootloader": bl_size = bl_size or len(fw)
    if kind == "softdevice": sd_size = sd_size or len(fw)
    app_size = len(fw) if kind == "application" else 0
    img_type = {"softdevice": 0x01, "bootloader": 0x02, "softdevice_bootloader": 0x03, "application": 0x04}[kind]
    print(f"package: {args.package}  kind {kind}  image {len(fw)} B, init packet {len(dat)} B")

    # BlueZ caches devices it has already seen and does not re-emit advertisements for them unless
    # something changes, so a plain find_device_by_name() can miss a board that IS advertising.
    # DuplicateData=True makes it report every advertisement. A hard timeout stops a wedged
    # BlueZ discovery from hanging this script silently.
    found = {}
    async def scan():
        ev = asyncio.Event()
        def cb(d, adv):
            n = adv.local_name or d.name
            if n == args.name: found["dev"] = d; ev.set()
        sc = BleakScanner(cb, bluez=BlueZScannerArgs(filters=BlueZDiscoveryFilters(DuplicateData=True)))
        async with sc:
            await asyncio.wait_for(ev.wait(), 25)
    try:
        await asyncio.wait_for(scan(), 35)
    except (asyncio.TimeoutError, Exception) as e:
        print(f"could not find a device advertising {args.name!r} within the scan window ({type(e).__name__}). "
              "Blue LED blinking? BLE OTA mode? No other bluetoothctl/bleak scan running? "
              "Try `bluetoothctl remove <address>`."); return 2
    dev = found["dev"]
    print(f"found {dev.address}")
    async with BleakClient(dev, timeout=20) as client:
        d = Dfu(client, args.quiet)
        await client.start_notify(CP, d.on_notify)
        acq = getattr(getattr(client, "_backend", None), "_acquire_mtu", None)   # BlueZ reports 23 until asked
        if acq:
            try: await acq()
            except Exception: pass
        t0 = time.time(); sent = 0
        try:
            d.log("START"); await d.cp(bytes([0x01, img_type]))
            await client.write_gatt_char(PKT, struct.pack("<III", sd_size, bl_size, app_size), response=False)
            try:
                await d.expect(1)
            except RuntimeError as e:
                if "INVALID_STATE" in str(e) and args.retry_reset:
                    # A dropped connection leaves the bootloader's DFU state machine "receiving" so a
                    # dropped upload can be RESUMED. To start over, send DFU RESET (opcode 6): it restarts
                    # the bootloader, which re-enters BLE OTA when there is no valid app.
                    print("bootloader still holds a half-finished session (INVALID_STATE): sending DFU RESET and retrying")
                    try: await d.cp(bytes([0x06]))
                    except Exception: pass
                    return 3
                raise

            d.log("INIT"); await d.cp(bytes([0x02, 0x00]))
            for i in range(0, len(dat), 20): await client.write_gatt_char(PKT, dat[i:i + 20], response=False)
            await d.cp(bytes([0x02, 0x01])); await d.expect(2)

            # Firmware data: the OTAFIX bootloader negotiates ATT MTU up to 247 and accumulates packets into flash
            # pages, so each write can carry up to MTU-3 bytes (a multiple of 4) instead of the legacy 20.
            pk = args.chunk or max(20, min(244, (getattr(client, "mtu_size", 23) - 3)) // 4 * 4)
            await d.cp(struct.pack("<BH", 0x08, args.prn))
            d.log(f"RECEIVE ({pk} B packets, PRN every {args.prn} packets, MTU {getattr(client, 'mtu_size', '?')})"); await d.cp(bytes([0x03]))
            npk = 0
            for i in range(0, len(fw), pk):
                if args.stop_after is not None and sent >= args.stop_after:
                    print(f"--stop-after {args.stop_after}: dropping the connection at {sent} B"); return 1
                chunk = fw[i:i + pk]
                await client.write_gatt_char(PKT, chunk, response=False); sent += len(chunk); npk += 1
                if npk % args.prn == 0:
                    d.prn.clear()
                    try: await asyncio.wait_for(d.prn.wait(), 10)
                    except asyncio.TimeoutError:
                        # a rejection while streaming shows up as a failed response instead of a PRN
                        if not d.resp.empty(): op, st = d.resp.get_nowait(); raise RuntimeError(f"{OPNAME.get(op, op)} failed mid-transfer: {STATUS.get(st, st)}")
                        raise RuntimeError("no packet receipt notification (transfer stalled)")
                if sent % 32768 < pk and not args.quiet:
                    print(f"  {sent}/{len(fw)} B  ({sent / (time.time() - t0):.0f} B/s)", flush=True)
                if not d.resp.empty():
                    op, st = d.resp.get_nowait()
                    if st != 1: raise RuntimeError(f"{OPNAME.get(op, op)} failed mid-transfer at {sent} B: {STATUS.get(st, st)}")
                    d.resp.put_nowait((op, st))
            await d.expect(3, 60)
            d.log(f"transfer complete: {sent} B in {time.time() - t0:.0f}s"); d.log("VALIDATE")
            await d.cp(bytes([0x04])); await d.expect(4, 60)
            d.log("ACTIVATE + RESET")
            try: await d.cp(bytes([0x05]))
            except Exception: pass                              # the board resets and drops the link
            print("UPLOAD ACCEPTED - board is activating the new image"); return 0
        except RuntimeError as e:
            print(f"UPLOAD FAILED after {sent} B: {e}")
            # stalled, or rejected mid-transfer (OPER_FAILED; "RECEIVE failed mid-transfer: SUCCESS" on bootloader zips) with large
            # packets: main() retries with 20-byte ones (boards 1 and 3 need them for app AND bootloader zips, 2026-09-27/28)
            if ("no packet receipt notification" in str(e) or "mid-transfer" in str(e)) and args.chunk != 20 and args.stop_after is None:
                return 4
            return 1


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("package"); ap.add_argument("--name", default="3401_DFU"); ap.add_argument("--prn", type=int, default=8)
    ap.add_argument("--chunk", type=int, default=0, help="data bytes per write (multiple of 4); 0 = MTU-3 (max 244), 20 = legacy")
    ap.add_argument("--stop-after", type=int, default=None); ap.add_argument("--quiet", action="store_true")
    ap.add_argument("--no-reset-retry", dest="retry_reset", action="store_false",
                    help="do not send DFU RESET and retry when the bootloader reports INVALID_STATE at START")
    args = ap.parse_args()
    def run_once():
        rc = asyncio.run(upload(args))
        if rc == 3:                               # RESET sent: give the bootloader time to restart, then retry once
            time.sleep(8); args.retry_reset = False
            rc = asyncio.run(upload(args))
        return rc
    rc = run_once()
    if rc == 4:
        # 244-byte packets are only proven for APP uploads on the LoRa-OTA bootloader; bootloader images and the stock OTAFIX
        # bootloader stalled at the first receipt notification (2026-09-26). Fall back to 20-byte packets (~800 B/s).
        print("large packets stalled or were rejected - retrying with 20-byte packets (slower, always accepted; next time: --chunk 20)")
        args.chunk = 20; args.retry_reset = True
        rc = run_once()
        if rc == 4: rc = 1
    sys.exit(rc)


if __name__ == "__main__":
    main()
