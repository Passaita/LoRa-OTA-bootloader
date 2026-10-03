# fw_ota — OTAFIX bootloader fork for LoRa firmware-pull (RAK3401)

Status (2026-09-18): **builds and links (30,780 of 38,912 B, 79.1%); NOT yet verified on hardware.** The
first real flash of a build with this code (2026-09-16) bricked a unit; the
cause is not yet established. Do not flash to a unit without an ST-Link
attached for recovery. The LZMA decode loop has been exercised host-side only.

Base: `oltaco/Adafruit_nRF52_Bootloader_OTAFIX`, tag `0.9.2-OTAFIX2.3-BP1.4`
(first release with a `wiscore_rak3401` board target, so the device advertises
as `3401_DFU` and reports `WisBlock RAK3401` over BLE), plus the fw_ota commits
on top. SoftDevice S140 6.1.1.

## What this adds

The application keeps running normally. Separately, it downloads an
LZMA-compressed MeshCore image over LoRa into a **single staging slot** and,
on request, marks the control page `APPLY_PENDING` and reboots. Before the
normal DFU checks, this bootloader then:

1. checks the version fence (see below),
2. validates the slot (magic, lengths, CRC-32 of the payload),
3. erases the app region and LZMA-decompresses the slot payload over it,
4. verifies the CRC-32 of the decompressed image,
5. records a valid application in the bootloader settings,
6. lets `main()` boot it.

**The fw_ota apply path only ever reads the slot, never writes or erases it.**
(Stock DFU could overwrite it with an oversized app upload; that is now
rejected — see "Other safeguards" below.) It therefore survives the apply,
which gives two things:

- the node can keep serving the compressed image to the next repeater over
  LoRa (updates walk outward from the base station, one hop at a time), and
- if an apply is interrupted (e.g. power outage), the slot is still intact and
  the apply restarts on the next boot from the same slot.

## Boot requirements (BLE-OTA)

- **No valid application → BLE OTA mode**, not serial/UF2 DFU. This is
  inherited unchanged from stock OTAFIX (`check_dfu_mode()`: when
  `!valid_app` and no explicit serial/UF2/double-reset request, `_ota_dfu` is
  forced true, with no timeout). Unlike RAK's factory bootloader, a blank or
  invalidated board therefore stays reachable over BLE.
- **A valid slot is applied before falling back to BLE OTA (self-heal;
  implemented 2026-09-18, build-verified only).** If there is no valid app but the slot
  passes its CRC, the bootloader applies it (self-heal after a power cut that
  left the app half-erased). This is skipped when the user explicitly asked
  for DFU (`start ota` sets `GPREGRET=0xA8`; the same rule covers the other
  DFU magics, the double-reset pattern and the DFU button) and once the apply
  has given up (`FW_OTA_MAX_FAILURES` / `FW_OTA_MAX_INTERRUPTED`, see below).
  With no valid slot, or after giving up, the board goes to BLE OTA as above.
- **Double-reset behaviour is left at the OTAFIX default.** A double reset
  still enters serial/UF2 DFU as stock does; nothing here changes it.
- **BLE DFU stays in the bootloader** as the last-resort recovery path for an
  image that verified but is broken at runtime. (Earlier drafts of this
  project planned to strip BLE DFU to make room; that was reversed.)
- When retries are exhausted **and the app region was actually destroyed**,
  `mark_failed()` sets `GPREGRET = 0xA8` (`DFU_MAGIC_OTA_RESET`) and resets, so
  the next boot lands in BLE OTA explicitly rather than relying on stock logic.
  If the app is still valid it is left alone and boots.

## Getting an image onto a node

| Path | Writes to | Status |
|---|---|---|
| LoRa `fw pull` from a node holding a valid slot | slot (by the app) | built, tested (M2a) |
| USB serial `fw stage` (host LZMA stager) | slot (by the app) | built, tested; **the ingest path for the bench device** |
| Stock BLE OTA / serial DFU (`start ota`, or no app) | **raw app region** (0x26000+), not the slot | stock OTAFIX; recovery path |
| BLE OTA straight into the slot | slot | **goal, not built** — needs a slot-target DFU mode in this bootloader |

BLE OTA is the field recovery path: a phone can flash a raw trimmed app (must
fit 448 KiB) onto a node that has no usable app or slot, and once it runs it
can be given a slot again (`fw pull` from a neighbour, or `fw stage`). A raw
upload does not update the slot, so the node cannot serve until it has one.

Bench workflow: `fw stage` over USB → apply → test → serve the same slot over
LoRa, so the bytes tested are the bytes shipped.

## Layout: one slot, no A/B, no automatic rollback

There is **no second slot and no restore of a previous image.** A verified but
functionally broken image has nothing to fall back to. This is an accepted
risk (design-doc amendment 2026-09-15). Mitigations: strict verification before
and during apply, BLE OTA recovery (above), canary rollout, and the optional
per-node `fw hold`. Compression is kept for LoRa transfer time (roughly halves
the bytes) and growth margin, not because the image would not otherwise fit.

## Geometry (must stay in sync with the app's `FwOtaSlots.h` + linker script)

```
0x26000 - 0x96000   Application            448 KiB   (linker FLASH end == slot start)
0x96000 - 0xEC000   Staging slot (LZMA)    344 KiB   36-byte header page + 340 KiB payload
0xEC000 - 0xED000   Control page           4 KiB
0xED000 - 0xF4000   InternalFS             (framework)
0xF4000 - 0x100000  Bootloader             38,912 B of code space + settings/UICR
```

Slot layout (as the **application** writes it - the source of truth): the
36-byte `fw_ota_slot_hdr_t` (magic `FLTO`) sits alone in the slot's first 4 KiB
page; the payload starts at slot + `FW_OTA_SLOT_PAYLOAD_OFFSET` (0x1000), so
payload writes are full aligned pages. `payload_crc32` covers the payload only.
The bootloader only implements `FW_OTA_COMP_LZMA` (raw LZMA1 stream, 5-byte
props); `COMP_NONE` exists on the app side for bench builds only. The decoder
uses an 8 KiB dictionary — the host-side stager must match it.

## Control page and retry behaviour

`fw_ota_ctrl_t` (ctrl_version 5): `apply_state` (IDLE / PENDING / DONE /
FAILED), `boot_attempts`, `fail_count`, `last_result` (`FW_OTA_R_*`),
`requested_bl_version`, and a trailing `crc32` over the record. A record whose
CRC does not match (blank, garbage, or torn by a power cut) is treated as
blank. The app-side twin is `CtrlBlock` in `FwOtaSlots.h`;
`tools/fw_ota_hosttest/check_contract.py` checks the two agree.

**Two kinds of unsuccessful attempt are counted separately:**

- `boot_attempts` counts attempts *started*. The bootloader persists it
  before touching the app region.
- `fail_count` counts attempts that ended in a *detected* failure (bad slot,
  decode error, CRC mismatch).
- `interrupted = boot_attempts - fail_count` is what is left: attempts that
  never reached a recorded outcome (power loss, hang, crash).
- Give up (-> `FAILED`, then BLE OTA if the app was destroyed) at
  `FW_OTA_MAX_FAILURES = 3` failures or `FW_OTA_MAX_INTERRUPTED = 8`
  interruptions. A power outage mid-apply therefore does not spend the
  failure budget; the interruption cap is only a crash-loop breaker.
- Success zeroes both counters. A new request from the app zeroes them too.

**Boot decision (`fw_ota_apply_on_boot`, before `check_dfu_mode`):**

1. SoftDevice swap in progress -> do nothing.
2. `PENDING` (app requested) -> version fence, then `run_apply`.
3. App valid -> do nothing (`IDLE`/`DONE`/`FAILED` all just boot).
4. No valid app: skip if DFU was explicitly requested (`GPREGRET` DFU magics
   incl. `start ota`, double-reset pattern, DFU button) or state is `FAILED`;
   otherwise, if the slot passes its CRC, start a fresh apply (self-heal).
5. Otherwise fall through to `check_dfu_mode()` -> BLE OTA.

**Other safeguards:**

- **Version fence.** The app stamps the running bootloader version when it
  requests PENDING. A PENDING request stamped by a different bootloader build
  is refused (`FW_OTA_R_STALE_VERSION`, no flash touched, no counter moved).
  A bootloader-only flash does not clear the control page, so without this a
  stale request would be applied destructively on the new bootloader's first
  boot. Suspected cause of the 2026-09-16 brick; not confirmed.
- **Idempotent apply.** If the app region already matches the slot's
  `raw_crc32`, nothing is erased; only a missing settings record is repaired
  (`FW_OTA_R_ALREADY_APPLIED`). Covers a power cut after the app was written
  but before the `DONE`/settings write.
- **Lost or torn control page.** `ctrl_write()` is erase-then-program, so a
  power cut can leave the page blank or half-written; the CRC makes a torn
  record read as blank. A blank page with a valid app just boots; with no valid
  app and a valid slot, self-heal rebuilds the state and applies. The one
  consequence: a cut during the *first* control write of an apply (before the
  app region is touched) can drop the update **request**. The old app is
  intact and keeps running; the update just has to be requested again.
- **Torn settings page.** A power cut mid-write of the stock settings page can
  leave `bank_0` valid with `bank_0_size` still erased; stock
  `bootloader_app_is_valid()` would then CRC ~4 GB and run off the end of
  flash (bus fault). `sanitize_settings()` runs first on every boot and erases
  a settings record whose size cannot describe an app below the bootloader.
- **Page-split flash writes.** `flash_nrf5x_write()` buffers one 4 KiB page and
  does not split; the decode loop writes through `flash_write_paged()`.
- **Watchdog.** An app-started WDT survives the reset into the bootloader.
  The apply loops feed it, as stock `wait_for_events()` does during DFU.
- **DFU size clamp.** Stock DFU accepted apps up to ~784 KiB; `dfu_single_bank.c`
  now rejects an app image larger than `FW_OTA_APP_MAX` (448 KiB), so an
  oversized upload cannot erase the slot.
- **Raw DFU invalidates the slot.** When a raw app upload completes, the slot
  header page is erased and the control page reset to `IDLE` (done after DFU
  ends and the SoftDevice is disabled), so the node does not serve or re-apply
  an image that is not what it runs.
- `DONE` is informational: the bootloader no longer counts boots in `DONE`
  (the post-boot self-test that used to clear it is out of scope).

## Known gaps / open items

- [ ] **Not hardware-verified.** Everything above is build-verified only. Test
      on a bench unit: BLE OTA with no app, app boot, apply from a staged slot,
      corrupt-slot refusal, power cut mid-apply (repeatedly), exhausted-retry
      fallback, self-heal, `start ota` overriding self-heal, oversized DFU
      rejection, slot invalidation after a raw DFU upload.
- [x] **Host-side harness** (`tools/fw_ota_hosttest`, `make test`): the real
      apply code against simulated flash with a power cut at every flash
      operation. It found and we fixed four real bugs (see its README): wrong
      payload offset, page-buffer overrun in the decode loop, torn-settings bus
      fault, torn control record. 23k checks pass; 13/13 mutants are caught.
      It does not model the SoftDevice, BLE, USB or real timing.
- [ ] **A power loss or reset between DFU completion and `fw_ota_after_dfu()`
      starting** leaves a stale slot and any old PENDING request in place for
      one boot. Not closable from a hook that runs after DFU; tiny window.
- [ ] **A cleanly applied but runtime-broken image has no fallback** other than
      BLE OTA by hand. Accepted: images are bench-tested first, on a unit
      upgraded from an older build with its existing filesystem/config.
- [ ] **`PENDING` overrides an explicit DFU request.** If `start ota` is used
      while a PENDING request is outstanding, the apply runs first.
- [ ] Build needs `-flto` (already in the Makefile): without it the bootloader
      does not fit its region. Compiler is gcc 7.2.1 (PlatformIO toolchain); a
      bit-identical rebuild of the official release is not expected with a
      different compiler.
- [ ] Lock the 8 KiB dictionary-size contract with the host stager
      (`tools/fw_ota_stage.py`) and the app's transfer code.
- [ ] Resume of an interrupted LoRa pull across reboots is not supported yet
      (app side).
- [ ] BLE OTA straight into the slot (goal): needs a slot-target DFU mode.
