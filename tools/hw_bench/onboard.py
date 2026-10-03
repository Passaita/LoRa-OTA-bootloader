#!/usr/bin/env python3
"""Onboard a node that runs a stock bootloader + stock app: bootloader first, then the LoRa-OTA app, all over BLE (no cable, no ST-Link).

  onboard.py --bootloader bootloader-only.zip --app app-tagged.zip [--dry-run] [--skip-bootloader]

Flow (proven on a factory RAK bootloader 0.4.4 and on OTAFIX 2.3):
  1. finds the node: advertising '<name>_OTA' (running stock app; `start ota` on the node first) or a bootloader (`*_DFU`, `AdaDFU`);
     a running app is sent the buttonless jump (control point 1531 START with notifications on) and the bootloader is awaited;
  2. pre-flight: reads the bootloader's Device Information (model / firmware revision) and REFUSES to go on unless it reports S140 6.1.1
     (the packages are built for that SoftDevice; the bootloader package also carries --sd-req 0xB6 and the node refuses a mismatch);
  3. uploads the bootloader-only package; the node applies it and falls back into BLE OTA on its own (its application was wiped);
  4. uploads the tagged app package. The node's settings (name, radio, admin password, identity) are untouched. The slot stays EMPTY until
     the node pulls an image from a server you trust (`fw pull`) - it never pulls by itself.
The node is OFF THE AIR between step 3 and the end of step 4; do not interrupt the transfers. Only one BLE scan at a time on this machine.
"""
import argparse, asyncio, os, sys, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ble_dfu
from bleak import BleakClient, BleakScanner
from bleak.args.bluez import BlueZDiscoveryFilters, BlueZScannerArgs

CP = "00001531-1212-efde-1523-785feabcd123"
DIS_MODEL, DIS_FW = "00002a24-0000-1000-8000-00805f9b34fb", "00002a26-0000-1000-8000-00805f9b34fb"
IS_DFU = lambda n: n is not None and (n.endswith("_DFU") or n == "AdaDFU")
IS_APP = lambda n: n is not None and n.endswith("_OTA")

async def find(pred, timeout):
    found = {}
    ev = asyncio.Event()
    def cb(d, adv):
        n = adv.local_name or d.name
        if pred(n): found["d"], found["n"] = d, n; ev.set()
    async with BleakScanner(cb, bluez=BlueZScannerArgs(filters=BlueZDiscoveryFilters(DuplicateData=True))):
        try: await asyncio.wait_for(ev.wait(), timeout)
        except asyncio.TimeoutError: return None, None
    return found["d"], found["n"]

async def jump(dev):
    async with BleakClient(dev, timeout=20) as c:
        await c.start_notify(CP, lambda h, d: None)
        try: await c.write_gatt_char(CP, bytes([0x01, 0x04]), response=True)
        except Exception: pass                       # the node resets into its bootloader and the link drops
        await asyncio.sleep(2)

async def read_dis(dev):
    async with BleakClient(dev, timeout=20) as c:
        out = {}
        for k, u in (("model", DIS_MODEL), ("firmware", DIS_FW)):
            try: out[k] = bytes(await c.read_gatt_char(u)).decode(errors="replace")
            except Exception: out[k] = "?"
        return out

def upload(pkg, name):
    ns = argparse.Namespace(package=pkg, name=name, prn=8, stop_after=None, quiet=False, retry_reset=True)
    rc = asyncio.run(ble_dfu.upload(ns))
    if rc == 3:                                      # a half-finished session: DFU RESET was sent, try once more
        time.sleep(8); ns.retry_reset = False; rc = asyncio.run(ble_dfu.upload(ns))
    return rc

async def to_bootloader(timeout=60):
    dev, name = await find(lambda n: IS_DFU(n) or IS_APP(n), 30)
    if not dev: sys.exit("no node found: put it in BLE OTA (`start ota`) and bring the phone/laptop within range")
    if IS_APP(name):
        print(f"found the running app '{name}': sending the buttonless jump"); await jump(dev)
        dev, name = await find(IS_DFU, timeout)
        if not dev: sys.exit("the node did not come up in its bootloader")
    print(f"bootloader '{name}' at {dev.address}"); return dev, name

def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--bootloader"); ap.add_argument("--app", required=True)
    ap.add_argument("--skip-bootloader", action="store_true", help="the node already runs the LoRa-OTA bootloader")
    ap.add_argument("--dry-run", action="store_true", help="find the node and run the pre-flight only")
    a = ap.parse_args()
    if not a.skip_bootloader and not a.bootloader: ap.error("--bootloader is required (or --skip-bootloader)")
    dev, name = asyncio.run(to_bootloader())
    info = asyncio.run(read_dis(dev))
    print(f"pre-flight: model '{info['model']}', firmware '{info['firmware']}'")
    if "s140 6.1.1" not in info["firmware"]:
        sys.exit("REFUSED: the bootloader does not report SoftDevice s140 6.1.1 - these packages are not for this node")
    if a.dry_run: print("dry run: nothing uploaded"); return
    if not a.skip_bootloader:
        print("=== step 1: bootloader"); rc = upload(a.bootloader, name)
        if rc: sys.exit(f"bootloader upload failed (rc {rc}); the node stays in BLE OTA on its old bootloader - retry")
        print("waiting for the node to come back in BLE OTA ..."); time.sleep(12)
        dev, name = asyncio.run(find(IS_DFU, 60))
        if not dev: sys.exit("the node did not come back in BLE OTA after the bootloader update")
    print("=== step 2: LoRa-OTA app"); rc = upload(a.app, name)
    if rc: sys.exit(f"app upload failed (rc {rc}); the node waits in BLE OTA - retry with --skip-bootloader")
    print("done. The node starts the app with its own settings; give it an image with `fw pull <server>` (it never pulls by itself).")

if __name__ == "__main__":
    main()
