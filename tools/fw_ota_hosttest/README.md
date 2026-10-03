# fw_ota host-side test harness

Runs the bootloader's **unmodified** `fw_ota_apply.c` (plus the real
`flash_nrf5x.c`, `LzmaDec.c` and `crc16.c`) on the host against a simulated
nRF52840 flash mapped at its real addresses, with power cuts injected at every
erase/program call. Needs only gcc and python3.

```
make test REAL_BIN=/path/to/app/firmware.bin   # contract check + full suite (REAL_BIN optional)
make mutants                                   # every re-introduced bug must be caught
HOSTTEST_STRIDE_REAL=1 HOSTTEST_SEEDS=8 ./hosttest img     # exhaustive: every op, 8 seeds
```

`REAL_BIN` is the app's raw `firmware.bin` (the `.zip` from `pio run` contains
it); without it only the synthetic "small" image set runs.

## What it checks
- normal apply; power cut at **every** flash operation (x seeds), then reboot
  until settled: must end running the new image, slot unchanged, no writes onto
  unerased flash; also random sequences of 1-4 cuts
- self-heal (no app + valid slot), incl. half-erased app, garbage/torn control
  page, app written but settings lost
- explicit DFU requests (each GPREGRET magic, double reset, button, SoftDevice
  swap) never trigger an apply and never touch flash
- corrupt / undecodable / wrong-CRC slots; give-up after 3 failures; 8-interruption
  cap; a power outage does not spend the failure budget
- version fence, idempotent apply, `DONE`/`FAILED` boots do no flash writes
- watchdog: longest stretch without a feed < 1 s
- raw DFU upload invalidates the stale slot and pending request

`check_contract.py` compares the app's `FwOtaSlots.h` with the bootloader's
`fw_ota.h` (geometry, magics, struct field order). `mutants.py` proves the
suite has teeth.

## Not modelled
SoftDevice, BLE, USB, real timing (only a coarse watchdog estimate), and the
real `check_dfu_mode()` (a reduced version is in `boot_once()`). The settings
page write is a model of the stock non-OTA path. Passing here says the state
machine is power-cut safe; it says nothing about the hardware.

## Found by this harness (2026-09-18)
1. The bootloader read the slot payload at slot+32; the app writes it at
   slot+0x1000, so no genuine slot ever validated.
2. The decode loop passed up to 8 KiB blocks to `flash_nrf5x_write()`, which
   buffers one 4 KiB page and does not split: RAM corruption on hardware.
3. A torn settings-page write could leave `bank_0_size` erased; the stock
   `bootloader_app_is_valid()` then walks off the end of flash (bus fault).
4. A torn control-page record could alias a different valid state.
