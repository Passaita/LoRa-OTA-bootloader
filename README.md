# LoRa-OTA bootloader for the RAK3401

**The small piece that lets your MeshCore repeaters update themselves over LoRa: no climbing, no cable, no site visit.**

This is the bootloader half of **[MeshCore LoRa-OTA](https://github.com/Passaita/MeshCore-LoRa-OTA)**, a community repeater firmware
that fetches new versions from the repeater next door over the mesh. It is a fork of oltaco's
[OTAFIX 2.3 bootloader](https://github.com/oltaco/Adafruit_nRF52_Bootloader_OTAFIX) (`0.9.2-OTAFIX2.3-BP1.4`) with **one feature
added**: it installs a firmware image that the running repeater has downloaded over LoRa. Everything OTAFIX does (Bluetooth OTA,
serial DFU, UF2, double-reset, "no app -> Bluetooth OTA") works as before.

> **Current release: [rev 8](https://github.com/Passaita/LoRa-OTA-bootloader/releases/tag/bootloader-rev8)**, public community test.
> It is the standard bootloader of the test fleet. Use it together with the
> [LoRa-OTA app](https://github.com/Passaita/MeshCore-LoRa-OTA/releases/tag/v1.17.28-rc21) (v1.17.28-rc21).

## Why a repeater needs it

A program can't safely overwrite itself while it runs. So the LoRa-OTA app only *downloads* the new, compressed image into a spare
area of flash (the "slot"), checks it, and restarts. The bootloader, which is small, simple and never overwritten by an app update,
then does the risky part: unpack the slot over the old app, mark it valid, and start it. If the power fails half way, the next start
simply finishes the job.

## Why you'll like it

* **Install once, then forget the ladder.** One Bluetooth visit per node puts this bootloader and the LoRa-OTA app on it. From then on,
  app updates arrive over LoRa from neighbouring repeaters.
* **The bootloader itself updates over LoRa too.** `fw blupdate bl-rev-8` fetches a new bootloader from a neighbour; the app and all
  settings are kept.
* **Power-cut safe.** The install step was tested with a simulated power cut at every single flash write (5153 checks), plus real
  power pulls on the bench. A node that loses power during an install finishes it at the next start.
* **Seed a version without installing it.** `fw pull-ble` restarts into a Bluetooth upload that goes into the slot, not over the
  running app. The node keeps running and offers the new version to its neighbours. Five minutes without data and it goes back to
  the app, unchanged.
* **Your node stays your node.** Name, identity, contacts, radio settings and passwords live in a flash area the bootloader never
  touches, so onboarding and every update keep them.
* **No lock-in.** Stock repeater firmware runs on it fine (stock apps up to 792 KiB), and images are deliberately not signed, so you can
  always go back to stock firmware.
* **Booster off in the bootloader (new in rev 8).** The RAK13302 1 W booster stays off while the bootloader runs, so it draws no
  current during installs and Bluetooth DFU. This does not cure every weak battery: one test board on a LiFePO4 cell near 3.3 V still
  resets when Bluetooth DFU starts. Do Bluetooth updates on a charged battery or on USB.

## Install it (once per node)

On a RAK3401 running stock MeshCore with the stock OTAFIX bootloader:

1. Send `start ota` from the phone app. The node shows up as `3401_DFU` in the **nRF Device Firmware Update** app. Upload
   **`LoRa-OTA-bootloader-rev8_s140_6.1.1-ONBOARD.zip`**.
2. **Right after**, upload the LoRa-OTA app's **`-firmware.zip`** from the
   [app release](https://github.com/Passaita/MeshCore-LoRa-OTA/releases/tag/v1.17.28-rc21). A Bluetooth bootloader update erases the
   app, so the node is off the air until this step is done.
3. Join your circle with `fw passphrase <your phrase>` and you're set. The rest is in the
   [LoRa-OTA README](https://github.com/Passaita/MeshCore-LoRa-OTA#getting-started-once-per-node).

`fw bl` on the node then shows `rev 8`. No erase is needed: the node keeps its identity and settings.

## Release files

| File | Use |
|---|---|
| `LoRa-OTA-bootloader-rev8_s140_6.1.1-ONBOARD.zip` | by Bluetooth from the **stock** OTAFIX bootloader (`start ota`); upload the app zip right after |
| `LoRa-OTA-bootloader-rev8-bootloader_only.zip` | by Bluetooth from an older LoRa-OTA bootloader; upload the app zip right after |
| `LoRa-OTA-RAK3401-bootloader-rev8.lota` | over LoRa: nodes take it from a neighbour (`fw blupdate bl-rev-8`), app kept |
| `LoRa-OTA-bootloader-rev8.hex` | SWD (`program ... verify`, never a mass erase) |
| `LoRa-OTA-bootloader-rev8-update.uf2` | UF2 drag and drop |

Each release has a `SHA256SUMS.txt`. The served image of rev 8 has crc32 `E0BC335E` (`fw bl` shows it).

## Is it safe? The honest part

* The bootloader only installs images that carry the LoRa-OTA firmware tag and fit the app region. Bluetooth OTA stays available as the
  recovery path at all times.
* There is **no automatic rollback**: one slot, no A/B copy. Roll a new version out to a node you can reach first.
* Images are **not signed**, on purpose (see above). On the LoRa side, only nodes in your circle can serve or fetch firmware, and every
  transfer is encrypted; see the app's [SECURITY.md](https://github.com/Passaita/MeshCore-LoRa-OTA/blob/main/docs/lora-ota/SECURITY.md).
* This is a **community project**, not an official Adafruit, OTAFIX or RAKwireless release.

## Documentation

| You want to... | Read |
|---|---|
| use LoRa-OTA day to day (commands, fleet updates, recovery) | the [MeshCore-LoRa-OTA](https://github.com/Passaita/MeshCore-LoRa-OTA) repository |
| understand the flash map, apply state machine and safeguards | [docs/FWPULL_BOOTLOADER.md](docs/FWPULL_BOOTLOADER.md) |
| see what changed in each revision | [docs/REVISIONS.md](docs/REVISIONS.md) |
| look up boards, BLE names, DFU app settings and troubleshooting from upstream OTAFIX | [docs/OTAFIX-README.md](docs/OTAFIX-README.md) |

## Building and tests

* Build: `make BOARD=wiscore_rak3401 FW_OTA_BL_REV=8` with the ARM GCC toolchain on `PATH`.
* Reproducible: this rebuilds the published rev 8 hex byte for byte:
  ```
  SOURCE_DATE_EPOCH=1791302731 make BOARD=wiscore_rak3401 FW_OTA_BL_REV=8 GIT_VERSION=0.9.2-OTAFIX2.3-BP1.4-41-blrev8 \
    GIT_SUBMODULE_VERSIONS="lib/nrfx (7a4c9d9) lib/tinyusb (9775e76) lib/uf2 (adbb8c7)"
  ```
* `tools/fw_ota_hosttest`: `make test` runs the **unmodified** apply code on a simulated nRF52840 flash with a power cut at every flash
  operation (5153 checks); `make mutants` puts known bugs back, and each must be caught (23/23).
* `tools/check_vectors.py`: run before flashing any build. `tools/hw_bench`: Bluetooth DFU client, LoRa monitors, power-cut rigs.

## Credits and licence

* **[OTAFIX](https://github.com/oltaco/Adafruit_nRF52_Bootloader_OTAFIX)** by oltaco, based on the
  [Adafruit nRF52 bootloader](https://github.com/adafruit/Adafruit_nRF52_Bootloader) (MIT). Nordic SDK files keep their own licence.
* **LoRa-OTA additions**: LoRa-OTA contributors, MIT like the code they extend ([LICENSE](LICENSE)).

Not an official Adafruit, OTAFIX or RAKwireless release.
