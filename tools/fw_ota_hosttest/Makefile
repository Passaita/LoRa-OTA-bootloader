# fw_ota host-side test harness. Requires: gcc, python3.  Run: make test
SRC   := ../../src
SDK   := ../../lib/sdk11/components/libraries/bootloader_dfu
CRC16 := ../../lib/sdk/components/libraries/crc16
IMG   ?= img
CC    ?= gcc
CFLAGS = -O1 -g -w -no-pie -D_LZMA_SIZE_OPT -Istubs -I$(SRC)/fw_ota -I$(SRC) -I$(SRC)/lzma -I$(CRC16) -I$(SDK)

hosttest: hosttest.c $(SRC)/fw_ota/fw_ota_apply.c $(SRC)/fw_ota/fw_ota.h $(SRC)/flash_nrf5x.c $(SRC)/lzma/LzmaDec.c $(CRC16)/crc16.c
	$(CC) $(CFLAGS) -o $@ hosttest.c $(SRC)/lzma/LzmaDec.c $(CRC16)/crc16.c

# REAL_BIN: the app's firmware.bin used for the "real" image set (optional)
images:
	python3 gen_images.py $(IMG) $(if $(REAL_BIN),--real $(REAL_BIN))

test: hosttest images
	python3 check_contract.py
	./hosttest $(IMG)

# re-introduce known bugs into a copy of fw_ota_apply.c; every one must be caught
mutants: images
	python3 mutants.py $(IMG)

clean:
	rm -f hosttest
	rm -rf $(IMG)
.PHONY: images test mutants clean
