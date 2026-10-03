#ifndef STUB_BOOTLOADER_H
#define STUB_BOOTLOADER_H
#include <stdint.h>
#include <stdbool.h>
#define MK_BOOTLOADER_VERSION ((0u << 16) + (9u << 8) + 2u)
bool bootloader_app_is_valid(void);
bool bootloader_dfu_sd_in_progress(void);
#endif
