# fw-pull bootloader (RAK3401) – design and operation

This bootloader is [`oltaco/Adafruit_nRF52_Bootloader_OTAFIX`](https://github.com/oltaco/Adafruit_nRF52_Bootloader_OTAFIX)
tag `0.9.2-OTAFIX2.3-BP1.4` (which lists the RAK3401 and advertises `3401_DFU` over BLE) **plus one feature**:
it can unpack a compressed firmware image that the running application has downloaded over LoRa into a staging slot.
Everything the OTAFIX bootloader does (BLE OTA, serial DFU, double-reset DFU, "no valid app → BLE OTA") is unchanged.

The application half is described in `docs/lora-ota/` of the MeshCore-LoRa-OTA repository. Revisions 1-7: `REVISIONS.md`.

## Why a custom bootloader

The application cannot overwrite itself while it runs, and a 448 KiB application does not fit into a second slot next
to a 344 KiB staging area. So the app only *downloads* the compressed image into the slot and asks for a reboot; the
bootloader – which is small, simple and never overwritten – does the risky part: decompress the slot over the app
region, record the app as valid, boot it.

Design decisions (all made with the project owner):

* **One slot, read-only for the bootloader.** The compressed image stays in the slot after the apply, so the node can
  serve it to the next repeater and can redo the apply after a power cut. No A/B, no automatic rollback: images are
  bench-tested on an accessible node first.
* **BLE OTA stays the recovery path** (`3401_DFU`) and the way to load the very first image.
* **Only tagged images are accepted** (see below).

## Flash map (nRF52840, SoftDevice S140 6.1.1)

| Range | Size | Content |
|---|---|---|
| 0x00000–0x26000 | 152 KiB | MBR + SoftDevice |
| **0x26000–0x96000** | 448 KiB | **application** (`FW_OTA_APP_MAX` = 458,752 B) |
| **0x96000–0xEC000** | 344 KiB | **staging slot**: page 0 = 36-byte header, payload from +0x1000 (348,160 B) |
| **0xEC000–0xED000** | 4 KiB | **control page** (64-byte record, CRC-32, version 5) |
| 0xED000–0xF4000 | 28 KiB | InternalFS (MeshCore settings + node identity) |
| 0xF4000–0xFE000 | 40 KiB | bootloader (36,680 of 38,912 B used, 94.3 %) |
| 0xFE000 / 0xFF000 | | MBR params / bootloader settings |

The slot header holds payload length and CRC-32, raw length and CRC-32, firmware version, compression id and the five
LZMA property bytes. The slot is *valid* only if the header parses and the payload CRC matches.

## Apply state machine

```
app: download → verify (CRC-32 + SHA-256) → write header → set ctrl = PENDING → reset
bootloader on every boot:
  ctrl PENDING (or: no valid app + valid slot, unless DFU was requested)
    → check the firmware tag by decoding the first KiB into RAM         (nothing erased yet)
    → erase the app region, LZMA-decode slot → app, feeding the watchdog
    → CRC-32 of the result must equal raw_crc32
    → write bootloader settings (app valid), ctrl = DONE, boot
```

Counters in the control page make an apply that keeps dying give up instead of looping: `boot_attempts` counts starts,
`fail_count` counts detected failures; more than 3 failures or 8 interrupted attempts → state FAILED, the bootloader
stops retrying and falls back to BLE OTA. `fw ctl` in the app shows `state`, `attempts`, `fails`, `last` (0 = ok,
11 = no firmware tag).

**Self-heal:** if the app is missing/corrupt but the slot holds a valid, tagged image, the bootloader applies it
without being asked – unless DFU was explicitly requested (`start ota`, DFU magics, double reset, DFU button, or a
SoftDevice swap in progress) or the apply already gave up.

## Safeguards worth knowing about

* **Power cut at any point is safe.** Verified by a host-side harness that compiles the *real* bootloader sources against
  simulated flash and cuts power at every single flash operation (`tools/fw_ota_hosttest`, `make test`; `make mutants`
  introduces 20 deliberate bugs and all 20 are caught), plus 12 random SWD resets and 8 real power pulls on hardware
  (`tools/hw_bench/powerpull.py`).
* Torn settings page → `sanitize_settings()` before the stock `bootloader_app_is_valid()` can bus-fault on it.
* Control record is CRC-protected; apply is idempotent; a version fence refuses control records from other layouts.
* `flash_nrf5x_write` buffers exactly one 4 KiB page, so all bootloader writes go through `flash_write_paged()`.
* DFU size clamp and slot-then-control invalidation after a raw DFU upload; a stock DFU quirk (block via
  0xFFFFFFFF wraps around) is replaced by an explicit `m_dfu_blocked` flag.

### The firmware tag

16 bytes at image offset 0x200: `{ magic 0x4C505746 ('FWPL'), u16 version = 1, u16 reserved, char board[8] = "RAK3401" }`,
placed by the app's linker script right after the vector table. The bootloader checks it

* on slot apply: by peek-decoding the first KiB **before** erasing anything (`last=11` if missing);
* on BLE/serial DFU: while the first KiB streams in; an untagged upload is aborted and never marked valid.

A rejected raw upload leaves the app's first page erased; a node with a valid tagged slot heals itself on the next reset,
otherwise it waits in BLE OTA for a correct upload.

## Building

```
export PATH=$HOME/.platformio/packages/toolchain-gccarmnoneeabi/bin:$PATH
make BOARD=wiscore_rak3401 all
python3 tools/check_vectors.py _build/build-wiscore_rak3401/*.elf _build/build-wiscore_rak3401/*.hex   # MUST pass before flashing
```

**LTO caveat (important, cost a bricked board once):** with `-flto` the linker silently dropped every C interrupt/SVC
handler (the weak `b .` versions were linked instead) and the bootloader hung after its first flash erase. The
Makefile therefore compiles `app_timer boards dfu_ble_svc main nrfx_power usb` with `-fno-lto` (`NOLTO_OBJS`), and
`tools/check_vectors.py` verifies that seven strong handlers are really wired. Never flash a bootloader that did
not pass that check.

## Flashing

Bootloader (needs an ST-Link/SWD; **never** `nrf5 mass_erase` – it wipes settings and the node identity):

```
openocd -f interface/stlink.cfg -c "transport select hla_swd" -f target/nrf52.cfg -c "reset_config none" \
  -c "adapter speed 1000" -c init -c halt -c "flash read_bank 0 backup.bin" \
  -c "program <bootloader>.hex verify" -c "reset run" -c shutdown
```

Take the backup first (it contains node identity keys – keep it private). The application is then loaded over BLE OTA
(`tools/hw_bench/ble_dfu.py` from a Linux laptop, or the Nordic DFU app) or with `adafruit-nrfutil dfu serial`.

## Updating the bootloader of a running node, and onboarding a stock device

`make BOARD=<board> all` also produces `<name>_bootloader_only.zip`: a DFU package that carries **only the bootloader** and **requires the
SoftDevice it was built for** (`--sd-req`: 0xB6 for S140 6.1.1). A node with another SoftDevice refuses the package at the very first step
(verified on hardware: a package requiring 0xA8 was rejected at INIT with the node untouched, the 0xB6 package was accepted and applied), instead
of being bricked by a bootloader that does not fit. The stock bootloader copies the new image with the MBR (power-cut safe).

Onboarding a node that runs a stock bootloader and a stock app (proven with a factory RAK bootloader 0.4.4 and with OTAFIX 2.3):

1. `start ota` on the stock app (remote admin or console), connect the phone's DFU app (the Android *DFU* app works) and send the
   **bootloader-only zip**. The transfer wipes the application, so the repeater is off the air from here until step 2 is done.
   When it is applied the node falls back into BLE OTA by itself (OTAFIX >= 2.1; the very old factory 0.4.4 has no BLE fallback, so do not
   interrupt the transfer on such a node).
2. Send the **tagged LoRa-OTA app zip** (raw write into the app region; it is checked for the firmware tag and must fit 448 KiB).

The node then runs the LoRa-OTA app with its own settings: the name, radio parameters, admin password and identity live in InternalFS
(0xED000-0xF4000), which neither update touches. The staging slot is **not** filled by a BLE upload - the slot stays empty until the node
gets an image with `fw pull` (LoRa, from a server you trust) or `fw stage` (USB); it can serve only after that. A freshly onboarded node never
pulls by itself. A pull asks for the admin password of the *serving* node (typed on the puller), so the new node's own password does not matter.

## Tests and tools in this repository

| Path | Purpose |
|---|---|
| `tools/fw_ota_hosttest/` | Power-cut-at-every-op simulation of the real apply code, mutation tests, contract check against the app's constants |
| `tools/check_vectors.py` | Refuses a build whose interrupt handlers were dropped by LTO |
| `tools/hw_bench/hwcut.py` | Interrupt an apply with SWD resets at random moments |
| `tools/hw_bench/powerpull.py` | Real USB power pulls, proven by a `GPREGRET2` marker |
| `tools/hw_bench/ble_dfu.py` | Linux BLE OTA client (legacy Nordic DFU) |
| `tools/hw_bench/LORA_TWO_BOARD_TEST.md` | Two-board LoRa procedure and lessons learned |

## Known limits / backlog

* No rollback (by design). No image signature yet (the tag is an accident guard, not authentication).
* Bootloader has ~2.2 KB free; the application ~9.8 KB.
* Backlog: signed manifests, version guard / canary rollout, BLE-to-slot upload, protection of the running app from a
  mistaken raw upload.
