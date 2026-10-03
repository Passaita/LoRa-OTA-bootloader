/*
 * fw_ota - bootloader side of the LoRa firmware-pull feature for RAK3401.
 *
 * SINGLE-SLOT layout (2026-09-15 decision, supersedes the earlier A/B
 * version of this file): the application downloads an LZMA-compressed
 * MeshCore image over LoRa into ONE staging slot and marks the control page
 * APPLY_PENDING. On the next boot this bootloader validates that slot,
 * decompresses it over the application region, records a valid app in the
 * bootloader settings, and boots it.
 *
 * There is no second slot and no automatic rollback: a verified-but-broken
 * image has no previous copy to fall back to. Accepted risk (see the
 * project's design doc amendment) - mitigated by strict pre-apply
 * verification here and by keeping BLE DFU in this bootloader as a manual
 * recovery path.
 *
 * Flash geometry MUST match the application's helpers/fw_ota/FwOtaSlots.h
 * and the forked linker script boards/nrf52840_s140_v6_fwota.ld.
 */
#ifndef FW_OTA_H_
#define FW_OTA_H_

#include <stdint.h>
#include <stdbool.h>

#define FW_OTA_PAGE_SIZE    0x1000u

#define FW_OTA_APP_START    0x26000u
#define FW_OTA_APP_END      0x96000u          /* linker FLASH end == slot start (448 KiB app) */
#define FW_OTA_APP_MAX      (FW_OTA_APP_END - FW_OTA_APP_START)
#define FW_OTA_RAW_APP_MAX  (FW_OTA_CTRL_ADDR - FW_OTA_APP_START)   /* rev 6: a raw (BLE/serial DFU) app may use everything below the control
                                                                    page (792 KiB) - stock apps up to that size install (offboarding,
                                                                    recovery); an image applied from the LoRa slot stays <= FW_OTA_APP_MAX */

#define FW_OTA_SLOT_ADDR    0x96000u
#define FW_OTA_CTRL_ADDR    0xEC000u
#define FW_OTA_SLOT_SIZE    (FW_OTA_CTRL_ADDR - FW_OTA_SLOT_ADDR)   /* 344 KiB, single slot */
#define FW_OTA_BLE_IDLE_MS  (5UL * 60 * 1000)    /* rev 5 (owner): any BLE DFU (start ota, triple-click, fw pull-ble) leaves after 5 min without
                                                    a DFU packet: a valid app starts again; a damaged/missing app re-enters BLE DFU (5 min more) */
extern bool fw_ota_slot_dfu;                     /* rev 5: this DFU session writes into the slot (GPREGRET 0xA9) */
#define FW_OTA_CTRL_SIZE    FW_OTA_PAGE_SIZE  /* 0xEC000..0xED000, FS starts 0xED000 */

/* ---- firmware tag --------------------------------------------------------
 * An image applied from the LoRa staging slot must carry a tag near its start:
 * the first FW_OTA_TAG_SCAN bytes are decoded into RAM and checked BEFORE the app
 * region is erased (a node updating itself unattended never erases a working app
 * for an image it cannot run).
 * Raw app uploads (BLE OTA / serial DFU) are NOT tag-checked since bootloader rev 3,
 * so a node can be moved back to factory firmware (stock OTAFIX bootloader + stock app,
 * or a stock app directly); only the factory device-type / SoftDevice checks apply there.
 * The app places the tag with a linker section right after the vector table.
 * This is a guard against accidents, not against deliberate forgery. */
#define FW_OTA_TAG_MAGIC     0x4C505746u   /* bytes 'F','W','P','L' */
#define FW_OTA_TAG_VERSION   1u
#define FW_OTA_TAG_SCAN      1024u         /* tag must lie within the first KiB, 4-byte aligned */
#ifndef FW_OTA_BOARD_ID
#define FW_OTA_BOARD_ID      "RAK3401"     /* 7 chars + NUL = the 8-byte board field */
#endif

typedef struct __attribute__((packed)) {
  uint32_t magic;       /* FW_OTA_TAG_MAGIC                       */
  uint16_t version;     /* FW_OTA_TAG_VERSION                     */
  uint16_t reserved;
  char     board[8];    /* NUL-padded board id, e.g. "RAK3401"    */
} fw_ota_tag_t;

_Static_assert(sizeof(fw_ota_tag_t) == 16, "firmware tag layout must match the app's FwOtaTag");

/* ---- bootloader identity descriptor ---------------------------------------
 * A 32-byte constant inside THIS bootloader image, so that a node can serve its bootloader to
 * other nodes over LoRa and the receiver can check what it got before copying it over its own
 * bootloader (app side: helpers/fw_ota/BlImage.h - keep in sync). The bootloader itself only
 * reads .board from it (fw_ota_tag_ok). Found by scanning the image for the magic (4-byte aligned). */
#define FW_OTA_BL_MAGIC      0x4C424F4Cu   /* bytes 'L','O','B','L' */
#define FW_OTA_BL_DESC_VER   1u
#ifndef FW_OTA_BL_REV
#define FW_OTA_BL_REV        1u            /* monotonic bootloader revision; bump for every released bootloader (Makefile: FW_OTA_BL_REV=) */
#endif
typedef struct {
  uint32_t magic;         /* FW_OTA_BL_MAGIC                              */
  uint16_t desc_version;  /* FW_OTA_BL_DESC_VER                           */
  uint16_t flags;         /* 0                                            */
  char     board[8];      /* FW_OTA_BOARD_ID, NUL padded                  */
  uint32_t bl_rev;        /* FW_OTA_BL_REV                                */
  uint32_t bl_addr;       /* BOOTLOADER_REGION_START this image was built for   */
  uint32_t mbr_params;    /* BOOTLOADER_MBR_PARAMS_PAGE_ADDRESS it was built for */
  uint32_t reserved;
} fw_ota_bl_desc_t;
_Static_assert(sizeof(fw_ota_bl_desc_t) == 32, "bootloader descriptor layout must match the app's BlImage.h");
extern const fw_ota_bl_desc_t fw_ota_bl_desc;

/* ---- slot layout ---------------------------------------------------------
 * Must match the application's writer (FwOtaSlots.h/.cpp), which is the source
 * of truth for what is really in flash: the header (fw_ota_slot_hdr_t, 36
 * bytes) sits ALONE in the slot's first 4 KiB page, and the payload starts on
 * the next page boundary so every payload write is a full, page-aligned
 * program. payload_crc32 covers the payload only. (This file previously
 * assumed the payload followed a 32-byte header directly, so no genuine slot
 * ever validated; found by the host-side harness, 2026-09-18.) */
#define FW_OTA_SLOT_MAGIC           0x4F544C46u   /* "FLTO" */
#define FW_OTA_SLOT_PAYLOAD_OFFSET  0x1000u       /* payload at FW_OTA_SLOT_ADDR + this */
#define FW_OTA_SLOT_PAYLOAD_MAX     (FW_OTA_SLOT_SIZE - FW_OTA_SLOT_PAYLOAD_OFFSET)
#define FW_OTA_COMP_NONE        0u
#define FW_OTA_COMP_LZMA        2u            /* raw LZMA1 stream, 5-byte props prefix; see fw_ota_apply.c */

typedef struct __attribute__((packed)) {
  uint32_t magic;         /* FW_OTA_SLOT_MAGIC when a complete image is present */
  uint32_t payload_len;   /* bytes of payload after this header                */
  uint32_t raw_len;       /* uncompressed image size                           */
  uint32_t payload_crc32; /* CRC-32/IEEE over payload_len payload bytes        */
  uint16_t compression;   /* FW_OTA_COMP_*                                     */
  uint16_t hdr_version;   /* = 1                                               */
  uint32_t fw_version;    /* monotonic image version                           */
  uint32_t raw_crc32;     /* CRC-32/IEEE over the decompressed raw_len bytes   */
  uint8_t  lzma_props[5]; /* LZMA lc/lp/pb + dict size, raw-stream format      */
  uint8_t  reserved[3];
} fw_ota_slot_hdr_t;

_Static_assert(sizeof(fw_ota_slot_hdr_t) == 36, "slot header layout must match the app's SlotHeader");
_Static_assert(sizeof(fw_ota_slot_hdr_t) <= FW_OTA_SLOT_PAYLOAD_OFFSET, "slot header must fit in its own page");

/* ---- control page --------------------------------------------------------
 * Simplified for single-slot: no active_slot/pending_slot A-vs-B anymore,
 * just "is an apply pending" + retry/result bookkeeping. */
#define FW_OTA_CTRL_MAGIC       0x4F544343u   /* "CCTO" */

enum {
  FW_OTA_APPLY_IDLE    = 0,   /* nothing to do                                */
  FW_OTA_APPLY_PENDING = 1,   /* app -> bl: apply the slot on next boot       */
  FW_OTA_APPLY_DONE    = 2,   /* bl -> app: slot applied, now running         */
  FW_OTA_APPLY_FAILED  = 3,   /* bl -> app: slot invalid, app left untouched  */
};

/* last_result detail codes */
enum {
  FW_OTA_R_OK              = 0,
  FW_OTA_R_NO_MAGIC        = 1,
  FW_OTA_R_BAD_LEN         = 2,
  FW_OTA_R_PAYLOAD_CRC     = 3,
  FW_OTA_R_DECOMP_FAIL     = 4,
  FW_OTA_R_RAW_CRC         = 5,
  FW_OTA_R_FLASH_FAIL      = 6,
  FW_OTA_R_STALE_VERSION   = 7,   /* PENDING was requested under a different
                                    * bootloader build than the one now
                                    * running - refused, not applied. See
                                    * requested_bl_version below. */
  FW_OTA_R_GAVE_UP_FAILED  = 8,   /* fail_count reached FW_OTA_MAX_FAILURES     */
  FW_OTA_R_GAVE_UP_INTR    = 9,   /* too many attempts started but never
                                    * finished (power loss / hang / crash), see
                                    * FW_OTA_MAX_INTERRUPTED                     */
  FW_OTA_R_ALREADY_APPLIED = 10,  /* app region already matches the slot's
                                    * raw_crc32 - nothing erased or written      */
  FW_OTA_R_NO_TAG          = 11,  /* image has no valid firmware tag - refused
                                    * before anything was erased                 */
};

typedef struct __attribute__((packed)) {
  uint32_t magic;         /* FW_OTA_CTRL_MAGIC                                */
  uint32_t ctrl_version;  /* = 5 (v5 adds crc32; v4 fail_count; v3 requested_bl_version) */
  uint32_t apply_state;   /* FW_OTA_APPLY_*                                   */
  uint32_t boot_attempts; /* apply attempts STARTED; bl bumps it before each
                           * attempt, zeroed on success and on a new request  */
  uint32_t last_result;   /* FW_OTA_R_*                                       */
  /* App stamps this with its own `bootloaderVersion` (Adafruit core global,
   * == NRF_TIMER2->CC[0] == this bootloader's MK_BOOTLOADER_VERSION) at the
   * moment it requests PENDING. The bootloader refuses to act on a PENDING
   * request stamped with a version other than its own currently-running
   * one - see fw_ota_apply_on_boot(). This exists because a bootloader-only
   * DFU flash (nrfutil dfu serial, bootloader+SD package) does NOT touch
   * this control page: without this fence, a stale PENDING/DONE state left
   * over from testing under a PREVIOUS bootloader build would be acted on
   * automatically by a newly-flashed bootloader before it ever reaches
   * check_dfu_mode() - suspected root cause of the 2026-09-16 bricked-unit
   * incident (see project notes). 0 means "never stamped" (older ctrl_version
   * or a fresh/blank page) and never matches any real build, so it's
   * automatically treated as stale too. */
  uint32_t requested_bl_version;
  /* Apply attempts that ended in a DETECTED failure (bad slot, decode error,
   * CRC mismatch, flash error). Bumped by the bootloader when it records the
   * failure. An attempt that was started (boot_attempts) but never reached a
   * recorded outcome was interrupted - power loss, hang, or crash - and is
   * counted separately: interrupted = boot_attempts - fail_count. This split
   * exists so that a power outage during an apply does not use up the
   * retry budget meant for genuinely bad images. */
  uint32_t fail_count;
  uint8_t  reserved[32];
  /* CRC-32/IEEE over the first 60 bytes. A record torn by a power cut (partly
   * programmed words) can otherwise alias a different valid state - e.g. a
   * half-written apply_state PENDING(1) reading back as FAILED(3). A record
   * whose CRC does not match is treated as blank. */
  uint32_t crc32;
} fw_ota_ctrl_t;

_Static_assert(sizeof(fw_ota_ctrl_t) == 64, "control page record layout must match the app's CtrlBlock");

/* Give up (-> FAILED, then BLE OTA if the app region was destroyed) when:
 *  - this many attempts ended in a detected failure, or
 *  - this many attempts were started but never finished (crash-loop breaker;
 *    generous because a genuine power cut mid-apply is rare but legitimate). */
#define FW_OTA_MAX_FAILURES       3
#define FW_OTA_MAX_INTERRUPTED    8

/* Called from main() immediately after bootloader_init(), before check_dfu_mode().
 * Applies the slot when either
 *   (a) the app requested it (apply_state == PENDING, version fence passing), or
 *   (b) self-heal: there is no valid app but the slot passes its CRC - e.g. a
 *       power cut left the app half-erased, or the control page was lost.
 * (b) is skipped when the user explicitly asked for DFU (GPREGRET DFU magics
 * incl. `start ota`, the double-reset pattern, or the DFU button), when a
 * SoftDevice swap is in progress, and once the apply has given up (FAILED).
 * Returns normally in every case; if the app is not valid afterwards, main()
 * falls into BLE OTA. */
void fw_ota_apply_on_boot(void);

/* True if [head, head+len) contains a valid firmware tag (4-byte aligned, within
 * the first FW_OTA_TAG_SCAN bytes, matching version and board id). */
bool fw_ota_tag_ok(const uint8_t *head, uint32_t len);

/* Tag check for a raw APP upload (BLE OTA / serial DFU), used by the stock DFU data handler.
 * Call fw_ota_head_begin() at every start packet (check=true only for an app-only update),
 * then feed every data packet, in order, to fw_ota_head_feed(). Packets are written as
 * usual; the first FW_OTA_TAG_SCAN bytes are also copied into RAM, and once they have all
 * arrived FW_OTA_HEAD_REJECT is returned if they carry no valid tag - the caller must then
 * abort the update and refuse every later packet, so the image is never marked valid.
 * (BLE OTA clears the app's first page at the start packet and serial DFU erases the whole
 * app up front, so the running app is already gone by then; holding the head back would only
 * add a burst of large writes that the SoftDevice-backed pstorage queue cannot take.) */
enum {
  FW_OTA_HEAD_PASS   = 0,   /* write the packet as usual */
  FW_OTA_HEAD_REJECT = 1,   /* first KiB complete and untagged (or out of order): abort, write nothing more */
};
void fw_ota_head_begin(bool check);   /* check=true only for app-only updates */
int  fw_ota_head_feed(const uint8_t *data, uint32_t len, uint32_t offset);

/* Called from the stock DFU code (dfu_activate_app) when a raw application
 * image has been fully received. Only records that it happened. */
void fw_ota_on_dfu_app_complete(uint32_t app_size);   /* rev 6: the raw app's size (does it reach into the slot?) */

/* Called from check_dfu_mode() once DFU has ended and the SoftDevice is
 * disabled. If a raw app was written, the slot and control page no longer
 * describe the running app: erase the slot header page and reset the control
 * page to IDLE. */
void fw_ota_after_dfu(void);

#endif /* FW_OTA_H_ */
