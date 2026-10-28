# LoRa-OTA bootloader - revisions

`fw bl` on a node shows the running revision, size and CRC of the served image. Every revision is built on OTAFIX
`0.9.2-OTAFIX2.3-BP1.4` for the RAK3401 (`BOARD=wiscore_rak3401`, SoftDevice S140 6.1.1).

| Rev | Date | Change | Served image crc32 |
|---|---|---|---|
| **8** | 2026-10-06 | **Current.** The RAK13302 1 W booster stays OFF while the bootloader runs: P0.21 (SKY66122 CSD+CPS) and P1.02 (WB_IO2: 3V3_S rail + 5 V boost) are driven LOW at bootloader start and again at the hand-over to the app (rev 1-7 left them floating). Fixes Bluetooth DFU requests lost to a brownout on weak batteries. The app switches the booster on as before. | E0BC335E |
| 7 | 2026-09-30 | A Bluetooth/serial app upload keeps a slot that holds exactly that app (same raw length and CRC-32), so the node goes on serving it; a different slot image is still erased as stale. | CBD1064D |
| 6 | 2026-09-30 | Stock firmware installs over it: raw app uploads may use everything below the control page (up to 792 KiB; the stock RAK3401 repeater is 517 KiB). Control page and file system stay protected; images from the LoRa slot stay limited to 448 KiB. | 0A03600F |
| 5 | 2026-09-29 | Bluetooth upload **into the slot** (`fw pull-ble`, GPREGRET 0xA9; app/bootloader packages refused in that mode). 5-minute inactivity timeout for every Bluetooth DFU. | 03790E24 |
| 4 | 2026-09-27 | Triple-click RST = Bluetooth DFU (`3401_DFU`) without the app, for a frozen node on battery; double-click = USB DFU as before (~0.5 s later). | 5F63DDD5 |
| 3 | 2026-09-24 | Raw Bluetooth/serial app uploads no longer require the LoRa-OTA tag (offboarding to stock firmware); the tag is still required for images applied from the LoRa slot. | C8B802A4 |
| 2 | 2026-09-20 | Bootloader images can be delivered over LoRa (`fw blupdate`) and are copied by the MBR; the app is kept. | 48E114C0 |
| 1 | 2026-09-18 | First release: apply an LZMA image from the staging slot, control page, power-cut safe apply. | |

Updating: over LoRa with `fw blupdate` (app kept), or by Bluetooth with the `bootloader_only.zip` followed **immediately** by the app
zip (a Bluetooth/USB bootloader update erases the app). Onboarding a stock node: the bootloader-only zip, then the app zip.
