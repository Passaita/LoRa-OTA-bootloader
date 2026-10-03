/* Mirrors the definitions in lib/sdk11/.../dfu_types.h that fw_ota_apply.c uses. */
#ifndef STUB_DFU_TYPES_H
#define STUB_DFU_TYPES_H
#include <stdint.h>
#define BOOTLOADER_SETTINGS_ADDRESS 0x000FF000u
#define BOOTLOADER_REGION_START     0x000F4000u
#define BOOTLOADER_MBR_PARAMS_PAGE_ADDRESS 0x000FE000u
typedef enum { DFU_UPDATE_APP_COMPLETE = 1 } dfu_update_status_code_t;
typedef struct {
  dfu_update_status_code_t status_code;
  uint16_t app_crc;
  uint32_t sd_size, bl_size, app_size, sd_image_start;
} dfu_update_status_t;
void bootloader_dfu_update_process(dfu_update_status_t update_status);
#endif
