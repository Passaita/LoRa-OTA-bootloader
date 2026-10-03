/*
 * fw_ota_apply.c - bootloader-side "apply the staged slot", single-slot.
 *
 * Called from main() right after bootloader_init() and before check_dfu_mode().
 *
 * STATUS: NOT verified on hardware. 2026-09-18 rework adds: split retry
 * accounting (detected failures vs interrupted attempts, so a power outage
 * does not burn the failure budget), self-heal (apply a valid slot when there
 * is no valid app, unless DFU was explicitly requested), an idempotent
 * "already applied" skip, and slot/ctrl invalidation on raw DFU uploads.
 * Design and rationale: src/fw_ota/README.md.
 */
#include "fw_ota.h"

#include <string.h>
#include "nrf.h"
#include "nrfx_nvmc.h"
#include "boards.h"
#include "bootloader.h"
#include "bootloader_types.h"
#include "dfu_types.h"
#include "crc16.h"
#include "nrf_wdt.h"

#include "LzmaDec.h"

/* From flash_nrf5x.c in this bootloader. */
extern void flash_nrf5x_write(uint32_t dst, void const *src, int len, bool need_erase);
extern void flash_nrf5x_flush(bool need_erase);

/* From bootloader.c - writes the settings page (bank_0 = BANK_VALID_APP). */
extern void bootloader_dfu_update_process(dfu_update_status_t update_status);

/* ---------------------------------------------------------------------- */
/* CRC-32/IEEE - matches the application's helpers/fw_ota/FwOtaSlots.cpp   */
/* ---------------------------------------------------------------------- */
static const uint32_t kCrcNib[16] = {
  0x00000000, 0x1DB71064, 0x3B6E20C8, 0x26D930AC,
  0x76DC4190, 0x6B6B51F4, 0x4DB26158, 0x5005713C,
  0xEDB88320, 0xF00F9344, 0xD6D6A3E8, 0xCB61B38C,
  0x9B64C2B0, 0x86D3D2D4, 0xA00AE278, 0xBDBDF21C,
};

static uint32_t crc32_mem(const uint8_t *p, uint32_t len, uint32_t crc) {
  crc = ~crc;
  while (len--) {
    crc ^= *p++;
    crc = (crc >> 4) ^ kCrcNib[crc & 0x0F];
    crc = (crc >> 4) ^ kCrcNib[crc & 0x0F];
  }
  return ~crc;
}

static uint32_t crc32_flash(uint32_t addr, uint32_t len) {
  /* flash is memory-mapped on nRF52 - read directly */
  return crc32_mem((const uint8_t *)addr, len, 0);
}

/* ---------------------------------------------------------------------- */
/* flash helpers                                                          */
/* ---------------------------------------------------------------------- */
/* An application that started the WDT leaves it running across the reset into
 * the bootloader (it cannot be stopped). wait_for_events() feeds it during
 * stock DFU; the apply path (erase ~7 s + decode) must do the same or the
 * chip resets mid-apply. */
static void feed_wdt(void) {
  if (nrf_wdt_started(NRF_WDT)) {
    for (uint8_t i = 0; i < 8; i++) nrf_wdt_reload_request_set(NRF_WDT, i);
  }
}

/* flash_nrf5x_write() buffers exactly ONE 4 KiB page and does not split: a
 * write that runs past the page boundary overruns its buffer. Stock callers
 * pass small (<= 512 B) blocks; the decode loop below produces up to a full
 * 8 KiB dictionary window at a time, so split at page boundaries here. (Found
 * by the host-side harness as memory corruption, 2026-09-18.) */
static void flash_write_paged(uint32_t dst, const uint8_t *src, uint32_t len) {
  while (len) {
    uint32_t room = FW_OTA_PAGE_SIZE - (dst & (FW_OTA_PAGE_SIZE - 1));
    uint32_t n = len < room ? len : room;
    flash_nrf5x_write(dst, src, (int)n, false);
    dst += n; src += n; len -= n;
  }
}

static void flash_erase_range(uint32_t addr, uint32_t len) {
  uint32_t a = addr & ~(FW_OTA_PAGE_SIZE - 1);
  for (; a < addr + len; a += FW_OTA_PAGE_SIZE) {
    feed_wdt();
    nrfx_nvmc_page_erase(a);
  }
}

/* ---------------------------------------------------------------------- */
/* LZMA decompress: slot payload -> app region                            */
/*                                                                         */
/* Standard low-memory LzmaDec usage (per the LZMA SDK's own examples):   */
/* dic[] is a ring buffer sized to the encoder's dictionary size, NOT the */
/* full output. LzmaDec tracks processed/checkDicSize internally so back- */
/* references still resolve correctly across a dicPos wrap, as long as    */
/* dicBufSize >= the dictionary size used at compress time (8 KiB here,   */
/* matching the host-side stager - both ends must agree).                 */
/* ---------------------------------------------------------------------- */
#define FW_OTA_LZMA_DIC_SIZE   (8u * 1024u)

/* NUM_BASE_PROBS (1984) and LZMA_LIT_SIZE (0x300) are private to LzmaDec.c
 * (LzmaProps_GetNumProbs there) - values confirmed by compiling a small
 * host-side probe against the same LzmaDec.c, not guessed. Sized for the
 * worst case lc+lp=4 (LZMA's own enforced max) regardless of what the
 * host-side stager actually picks, so this never overflows. */
#define FW_OTA_LZMA_NUM_BASE_PROBS  1984u
#define FW_OTA_LZMA_LIT_SIZE        0x300u
static uint8_t  s_dic[FW_OTA_LZMA_DIC_SIZE];   /* BSS - RAM is not the tight budget here */
static CLzmaProb s_probs[FW_OTA_LZMA_NUM_BASE_PROBS + (FW_OTA_LZMA_LIT_SIZE << 4)];

/* Set up an LZMA raw-stream decoder over the static probs/dictionary buffers. */
static bool lzma_dec_init(CLzmaDec *dec, const uint8_t *lzma_props) {
  memset(dec, 0, sizeof(*dec));
  dec->probs = s_probs;
  dec->probs_1664 = s_probs + 1664;   /* BUGFIX 2026-09-16: LzmaDec.c's hot decode path reads through
                                       * probs_1664, not probs, unconditionally (GET_PROBS macro); only
                                       * LzmaDec_AllocateProbs2() (the malloc path this code deliberately
                                       * avoids) sets it normally. Without it the first decode segfaults
                                       * (found via a host-side ASan run on the real 213 KB payload). */
  if (LzmaProps_Decode(&dec->prop, lzma_props, 5) != SZ_OK) return false;
  LzmaDec_Init(dec);
  dec->dic = s_dic;
  dec->dicBufSize = FW_OTA_LZMA_DIC_SIZE;
  dec->dicPos = 0;
  return true;
}

static uint32_t fw_ota_decompress_lzma(const uint8_t *payload, uint32_t payload_len,
                                        const uint8_t *lzma_props, uint32_t out_start,
                                        uint32_t out_max) {
  CLzmaDec dec;
  if (!lzma_dec_init(&dec, lzma_props)) return 0;

  uint32_t in_pos = 0;
  uint32_t out_pos = 0;      /* logical bytes produced so far */
  uint32_t flushed = 0;      /* logical bytes already written to flash */

  for (;;) {
    feed_wdt();
    SizeT dic_limit = FW_OTA_LZMA_DIC_SIZE;
    SizeT src_len = payload_len - in_pos;
    ELzmaStatus status;

    SRes res = LzmaDec_DecodeToDic(&dec, dic_limit, payload + in_pos, &src_len,
                                    LZMA_FINISH_ANY, &status);
    in_pos += (uint32_t)src_len;
    if (res != SZ_OK) return 0;

    uint32_t produced = (uint32_t)dec.dicPos;   /* bytes now sitting in dic[flushed_in_dic..dicPos) */
    if (produced > flushed % FW_OTA_LZMA_DIC_SIZE || dec.dicPos == FW_OTA_LZMA_DIC_SIZE
        || status == LZMA_STATUS_FINISHED_WITH_MARK || status == LZMA_STATUS_MAYBE_FINISHED_WITHOUT_MARK) {
      uint32_t chunk = produced - (uint32_t)(flushed % FW_OTA_LZMA_DIC_SIZE);
      if (out_pos + chunk > out_max) return 0;   /* refuse to write past the app region */
      flash_write_paged(out_start + out_pos, s_dic + (flushed % FW_OTA_LZMA_DIC_SIZE), chunk);
      out_pos += chunk;
      flushed += chunk;
    }

    if (dec.dicPos == FW_OTA_LZMA_DIC_SIZE) {
      dec.dicPos = 0;   /* wrap the ring buffer - see comment above */
    }

    if (status == LZMA_STATUS_FINISHED_WITH_MARK) break;
    if (in_pos >= payload_len && status != LZMA_STATUS_NEEDS_MORE_INPUT) break;
    if (in_pos >= payload_len) return 0;   /* ran out of input without a finish marker */
  }

  flash_nrf5x_flush(false);
  return out_pos;
}

static uint32_t fw_ota_decompress(const fw_ota_slot_hdr_t *hdr, uint32_t slot_addr) {
  const uint8_t *payload = (const uint8_t *)(slot_addr + FW_OTA_SLOT_PAYLOAD_OFFSET);

  /* COMP_NONE dropped 2026-09-16 (bootloader-space sizing experiment): every
   * real slot will be LZMA-compressed, and this bootloader's ~38KB flash
   * partition is a hard enough limit (see the design doc amendment) that a
   * code path with no real production use isn't worth the bytes. The app
   * side (FwOtaSlots.h) still has COMP_NONE for bench/test builds - it's
   * only the BOOTLOADER's copy that can't afford to carry it too. */
  if (hdr->compression == FW_OTA_COMP_LZMA) {
    return fw_ota_decompress_lzma(payload, hdr->payload_len, hdr->lzma_props,
                                   FW_OTA_APP_START, FW_OTA_APP_MAX);
  }

  return 0;
}

/* ---------------------------------------------------------------------- */
/* firmware tag                                                           */
/* ---------------------------------------------------------------------- */
const fw_ota_bl_desc_t fw_ota_bl_desc __attribute__((used, aligned(4))) = {
  FW_OTA_BL_MAGIC, FW_OTA_BL_DESC_VER, 0, FW_OTA_BOARD_ID, FW_OTA_BL_REV,
  BOOTLOADER_REGION_START, BOOTLOADER_MBR_PARAMS_PAGE_ADDRESS, 0
};

bool fw_ota_tag_ok(const uint8_t *head, uint32_t len) {
  const char *id = fw_ota_bl_desc.board;   /* the one place the board id lives (keeps the descriptor linked in) */
  if (len > FW_OTA_TAG_SCAN) len = FW_OTA_TAG_SCAN;
  for (uint32_t o = 0; o + sizeof(fw_ota_tag_t) <= len; o += 4) {
    fw_ota_tag_t t;
    memcpy(&t, head + o, sizeof(t));
    if (t.magic == FW_OTA_TAG_MAGIC && t.version == FW_OTA_TAG_VERSION &&
        memcmp(t.board, id, sizeof(t.board)) == 0) {
      return true;
    }
  }
  return false;
}

/* Raw-upload tag check: the first FW_OTA_TAG_SCAN bytes are copied into RAM as they stream
 * past, then checked once complete. (aligned(4) is harmless here; nothing hands this buffer
 * to the SoftDevice any more.) */
static uint8_t  s_head[FW_OTA_TAG_SCAN] __attribute__((aligned(4)));
static uint32_t s_head_filled;
static bool     s_head_active;

void fw_ota_head_begin(bool check) {
  s_head_active = check;
  s_head_filled = 0;
}

int fw_ota_head_feed(const uint8_t *data, uint32_t len, uint32_t offset) {
  if (!s_head_active) return FW_OTA_HEAD_PASS;
  if (offset != s_head_filled) { s_head_active = false; return FW_OTA_HEAD_REJECT; }   /* out of order */

  uint32_t room = FW_OTA_TAG_SCAN - s_head_filled;
  uint32_t n = (len < room) ? len : room;
  memcpy(s_head + s_head_filled, data, n);
  s_head_filled += n;
  if (s_head_filled < FW_OTA_TAG_SCAN) return FW_OTA_HEAD_PASS;

  s_head_active = false;                       /* decided; later packets just pass */
  return fw_ota_tag_ok(s_head, s_head_filled) ? FW_OTA_HEAD_PASS : FW_OTA_HEAD_REJECT;
}

/* Decode just the first FW_OTA_TAG_SCAN bytes of the slot image into RAM and
 * look for the tag - before anything in the app region is erased. */
static bool fw_ota_slot_image_tagged(const fw_ota_slot_hdr_t *h) {
  CLzmaDec dec;
  if (!lzma_dec_init(&dec, h->lzma_props)) return false;
  SizeT src_len = h->payload_len;
  ELzmaStatus status;
  SRes res = LzmaDec_DecodeToDic(&dec, FW_OTA_TAG_SCAN,
                                 (const uint8_t *)(FW_OTA_SLOT_ADDR + FW_OTA_SLOT_PAYLOAD_OFFSET),
                                 &src_len, LZMA_FINISH_ANY, &status);
  if (res != SZ_OK) return false;
  return fw_ota_tag_ok(s_dic, (uint32_t)dec.dicPos);
}

/* ---------------------------------------------------------------------- */
/* control page                                                           */
/* ---------------------------------------------------------------------- */
#define FW_OTA_CTRL_CRC_LEN  (sizeof(fw_ota_ctrl_t) - sizeof(uint32_t))

/* Returns true if the control page holds a valid fw_ota block (magic and CRC).
 * When it does not - blank, erased, garbage, or torn by a power cut between
 * ctrl_write()'s erase and program - *c is zeroed so callers see a clean IDLE
 * state. */
static bool ctrl_read(fw_ota_ctrl_t *c) {
  memcpy(c, (const void *)FW_OTA_CTRL_ADDR, sizeof(*c));
  if (c->magic != FW_OTA_CTRL_MAGIC ||
      c->crc32 != crc32_mem((const uint8_t *)c, FW_OTA_CTRL_CRC_LEN, 0)) {
    memset(c, 0, sizeof(*c));
    return false;
  }
  return true;
}

static void ctrl_write(const fw_ota_ctrl_t *c) {
  fw_ota_ctrl_t tmp = *c;
  tmp.magic = FW_OTA_CTRL_MAGIC;
  tmp.ctrl_version = 5;
  tmp.crc32 = crc32_mem((const uint8_t *)&tmp, FW_OTA_CTRL_CRC_LEN, 0);
  nrfx_nvmc_page_erase(FW_OTA_CTRL_ADDR);
  nrfx_nvmc_words_write(FW_OTA_CTRL_ADDR, (const uint32_t *)&tmp,
                        (sizeof(tmp) + 3) / 4);
}

/* ---------------------------------------------------------------------- */
/* slot validation                                                        */
/* ---------------------------------------------------------------------- */
static bool slot_hdr_ok(const fw_ota_slot_hdr_t *h) {
  if (h->magic != FW_OTA_SLOT_MAGIC) return false;
  if (h->payload_len == 0 ||
      h->payload_len > FW_OTA_SLOT_PAYLOAD_MAX) return false;
  if (h->raw_len == 0 || h->raw_len > FW_OTA_APP_MAX) return false;
  if (h->compression == FW_OTA_COMP_NONE && h->raw_len != h->payload_len) return false;
  return true;
}

static bool slot_valid(fw_ota_slot_hdr_t *out) {
  memcpy(out, (const void *)FW_OTA_SLOT_ADDR, sizeof(*out));
  if (!slot_hdr_ok(out)) return false;
  uint32_t c = crc32_flash(FW_OTA_SLOT_ADDR + FW_OTA_SLOT_PAYLOAD_OFFSET, out->payload_len);
  return c == out->payload_crc32;
}

/* record a valid application so bootloader_app_is_valid() passes */
static void record_valid_app(uint32_t raw_len) {
  dfu_update_status_t s;
  memset(&s, 0, sizeof(s));
  s.status_code = DFU_UPDATE_APP_COMPLETE;
  s.app_size    = raw_len;
  s.app_crc     = crc16_compute((const uint8_t *)FW_OTA_APP_START, raw_len, NULL);
  bootloader_dfu_update_process(s);
}

/* DFU magic values as defined (file-local) in main.c - must stay in sync by
 * inspection, there is no shared header. */
#define FW_OTA_DFU_MAGIC_OTA_RESET    0xA8u   /* also what `start ota` sets */
#define FW_OTA_DFU_MAGIC_OTA_APPJUM   0xB1u
#define FW_OTA_DFU_MAGIC_SERIAL_ONLY  0x4Eu
#define FW_OTA_DFU_MAGIC_UF2          0x57u
#define FW_OTA_DFU_MAGIC_SKIP         0x6Du
#define FW_OTA_DBL_RESET_MAGIC        0x5A1AD5u

extern uint32_t *dbl_reset_mem;   /* main.c */

/* True when the user (or the app on the user's behalf) explicitly asked for a
 * DFU mode. Self-heal must not override that. Mirrors the conditions
 * check_dfu_mode() uses to decide dfu_start, without clearing anything. */
static bool explicit_dfu_requested(void) {
  uint32_t const g = NRF_POWER->GPREGRET;
  if (g == FW_OTA_DFU_MAGIC_OTA_RESET  || g == FW_OTA_DFU_MAGIC_OTA_APPJUM ||
      g == FW_OTA_DFU_MAGIC_SERIAL_ONLY || g == FW_OTA_DFU_MAGIC_UF2 ||
      g == FW_OTA_DFU_MAGIC_SKIP) {
    return true;
  }
  if ((*dbl_reset_mem) == FW_OTA_DBL_RESET_MAGIC &&
      (NRF_POWER->RESETREAS & POWER_RESETREAS_RESETPIN_Msk)) {
    return true;   /* double-reset pattern */
  }
  if (button_pressed(BUTTON_DFU)) return true;
  return false;
}

/* The stock settings page is written by erase + a multi-word program. A power
 * cut part-way through can leave bank_0 == VALID and bank_0_crc set while
 * bank_0_size is still erased (0xFFFFFFFF); the stock bootloader_app_is_valid()
 * then CRCs bank_0_size bytes from the app start and runs off the end of flash
 * (bus fault -> boot hang). Repair it BEFORE anything calls that function: a
 * size that cannot describe an app that fits below the bootloader means the
 * record is torn, so erase the settings page (the app then reads as invalid
 * and is either re-applied from the slot or left to BLE OTA). Sizes that are
 * merely wrong but in range are harmless - the CRC-16 check just fails. */
static void sanitize_settings(void) {
  bootloader_settings_t const *s = (bootloader_settings_t const *)BOOTLOADER_SETTINGS_ADDRESS;
  if (s->bank_0 == BANK_VALID_APP && s->bank_0_crc != 0 &&
      s->bank_0_size > (BOOTLOADER_REGION_START - FW_OTA_APP_START)) {
    feed_wdt();
    nrfx_nvmc_page_erase(BOOTLOADER_SETTINGS_ADDRESS);
  }
}

/* Transition to FAILED. If the app region was actually destroyed (no valid
 * app now), force the same BLE-OTA fallback stock main() uses on its own
 * "no valid app" path (GPREGRET = 0xA8) immediately. If the app is still
 * valid (e.g. every attempt failed before erasing anything) leave it alone -
 * a bad pending update must not kick out a perfectly good running app. */
static void mark_failed(fw_ota_ctrl_t *c, uint32_t why) {
  c->apply_state = FW_OTA_APPLY_FAILED;
  c->last_result = why;
  ctrl_write(c);
  if (!bootloader_app_is_valid()) {
    NRF_POWER->GPREGRET = FW_OTA_DFU_MAGIC_OTA_RESET;
    NVIC_SystemReset();
  }
}

/* ---------------------------------------------------------------------- */
/* apply                                                                  */
/* ---------------------------------------------------------------------- */

/* Returns true on success (including "already applied"). On failure sets
 * c->last_result. Nothing here touches the counters - the caller does. */
static bool try_apply(fw_ota_ctrl_t *c) {
  fw_ota_slot_hdr_t h;
  if (!slot_valid(&h)) {
    c->last_result = FW_OTA_R_PAYLOAD_CRC;
    return false;
  }

  /* Idempotence: if the app region already holds exactly this image (a
   * previous apply finished but its DONE write - or the settings-page write
   * - was lost to a power cut), don't erase a good app just to rewrite it.
   * Only repair the settings record if it is missing. */
  if (crc32_flash(FW_OTA_APP_START, h.raw_len) == h.raw_crc32) {
    if (!bootloader_app_is_valid()) record_valid_app(h.raw_len);
    c->last_result = FW_OTA_R_ALREADY_APPLIED;
    return true;
  }

  /* Refuse firmware without the tag BEFORE touching the app region, so an
   * accidentally staged stock image costs nothing but a recorded failure. */
  if (!fw_ota_slot_image_tagged(&h)) {
    c->last_result = FW_OTA_R_NO_TAG;
    return false;
  }

  /* From here on we destructively overwrite the current app with no fallback
   * copy. Interrupted or failed: the slot is only read, so it stays intact
   * and a later attempt restarts from it.
   *
   * bootloader_app_is_valid() needs no proactive invalidation before the
   * erase: it recomputes a CRC-16 over the live app flash every boot and has
   * an independent "first words erased" check, so a half-written app reads as
   * invalid on its own (checked against bootloader.c, 2026-09-16).
   * record_valid_app() below takes the synchronous NVMC path because the
   * async pstorage path only triggers when is_ota() is set, which
   * check_dfu_mode() does later than this function. */
  flash_erase_range(FW_OTA_APP_START, h.raw_len);

  uint32_t written = fw_ota_decompress(&h, FW_OTA_SLOT_ADDR);
  if (written != h.raw_len) {
    c->last_result = FW_OTA_R_DECOMP_FAIL;
    return false;
  }

  if (crc32_flash(FW_OTA_APP_START, h.raw_len) != h.raw_crc32) {
    c->last_result = FW_OTA_R_RAW_CRC;
    return false;
  }

  record_valid_app(h.raw_len);
  c->last_result = FW_OTA_R_OK;
  return true;
}

/* One apply cycle with the split retry accounting. c->apply_state is PENDING
 * on entry (requested by the app, or synthesized by self-heal). */
static void run_apply(fw_ota_ctrl_t *c) {
  uint32_t const interrupted =
      (c->boot_attempts > c->fail_count) ? (c->boot_attempts - c->fail_count) : 0;

  if (c->fail_count >= FW_OTA_MAX_FAILURES) {
    mark_failed(c, FW_OTA_R_GAVE_UP_FAILED);
    return;
  }
  if (interrupted >= FW_OTA_MAX_INTERRUPTED) {
    mark_failed(c, FW_OTA_R_GAVE_UP_INTR);
    return;
  }

  /* Persist "attempt started" BEFORE touching the app region. If power dies
   * from here on, the next boot sees boot_attempts > fail_count and counts an
   * interruption, not a failure. */
  c->boot_attempts++;
  ctrl_write(c);

  if (try_apply(c)) {
    c->apply_state   = FW_OTA_APPLY_DONE;
    c->boot_attempts = 0;
    c->fail_count    = 0;
    ctrl_write(c);
    /* fall through - main() finds a valid app and boots it */
  } else {
    /* Detected failure: record it. State stays PENDING so the next boot
     * retries (up to FW_OTA_MAX_FAILURES). If the app region was already
     * erased, main() drops to BLE OTA for this boot. */
    c->fail_count++;
    ctrl_write(c);
  }
}

/* ---------------------------------------------------------------------- */
/* entry points                                                           */
/* ---------------------------------------------------------------------- */
void fw_ota_apply_on_boot(void) {
  sanitize_settings();

  /* A SoftDevice swap is mid-flight: the app's validity is not meaningful and
   * main() will continue the swap. Leave everything alone. */
  if (bootloader_dfu_sd_in_progress()) return;

  fw_ota_ctrl_t c;
  bool const have_ctrl = ctrl_read(&c);

  if (have_ctrl && c.apply_state == FW_OTA_APPLY_PENDING) {
    /* Version fence (2026-09-17): refuse a PENDING request stamped while a
     * DIFFERENT bootloader build was running. A bootloader-only DFU flash
     * does not touch this page, so without the fence a stale request would
     * be applied - destructively - by a brand-new bootloader on its first
     * boot. This is a refusal, not a failed attempt: no flash is touched and
     * no counter moves. */
    if (c.requested_bl_version != (uint32_t)(MK_BOOTLOADER_VERSION)) {
      c.apply_state = FW_OTA_APPLY_FAILED;
      c.last_result = FW_OTA_R_STALE_VERSION;
      ctrl_write(&c);
      return;
    }
    run_apply(&c);
    return;
  }

  /* No apply requested. Nothing to do unless the app is missing. */
  if (bootloader_app_is_valid()) return;

  /* Self-heal: no valid app. Respect an explicit DFU request, and respect a
   * previous give-up. */
  if (explicit_dfu_requested()) return;
  if (have_ctrl && c.apply_state == FW_OTA_APPLY_FAILED) return;

  fw_ota_slot_hdr_t h;
  if (!slot_valid(&h)) return;   /* nothing to heal from -> BLE OTA */

  /* Start a fresh cycle from the (valid) slot. IDLE/DONE state carries no
   * useful counters; a blank page is already zeroed by ctrl_read(). */
  c.apply_state          = FW_OTA_APPLY_PENDING;
  c.boot_attempts        = 0;
  c.fail_count           = 0;
  c.requested_bl_version = (uint32_t)(MK_BOOTLOADER_VERSION);
  run_apply(&c);
}

/* Set by the stock DFU code (dfu_activate_app) when a raw app image has been
 * fully received. Acting on it must wait until DFU has ended: during BLE OTA
 * the SoftDevice is enabled and direct NVMC access is not safe. */
static bool s_dfu_app_written = false;

static uint32_t s_dfu_app_size;
void fw_ota_on_dfu_app_complete(uint32_t app_size) {
  s_dfu_app_written = true;
  s_dfu_app_size = app_size;
}

void fw_ota_after_dfu(void) {
  if (!s_dfu_app_written) return;
  s_dfu_app_written = false;

  /* A raw app has just been written over the app region by stock DFU. The
   * slot no longer describes what is running, and the control page may carry
   * a PENDING request from before. Erase the slot header first, then reset the
   * control page. Either order is safe against applying a stale image after a
   * power cut (a PENDING request with no valid slot just fails validation
   * without touching the app; an IDLE page with a valid slot is never
   * applied), but slot-first never leaves a stale slot behind for the node to
   * SERVE to others. The one unavoidable exposure is a power loss before this
   * function starts at all, which leaves both untouched. Runs after
   * sd_softdevice_disable(), so direct NVMC access is fine. (Verified by the
   * host harness, tools/fw_ota_hosttest.) */
  feed_wdt();
  /* rev 6: a raw app larger than the app region already lies over the slot - its header page is now APP code, not a slot header:
   * erasing it would break the app just written. (The app's own bytes there are no valid slot header, so nothing stale remains.)
   * rev 7 (owner, C10): keep a slot that holds EXACTLY the app just uploaded (same length and raw CRC-32) - it describes what runs,
   * so the node goes on serving it (2026-09-30: an app update by phone erased board 2's fw-284 slot). Any other slot image is stale. */
  if (s_dfu_app_size <= FW_OTA_APP_MAX) {
    fw_ota_slot_hdr_t h;
    const bool same = slot_valid(&h) && h.raw_len == s_dfu_app_size && crc32_flash(FW_OTA_APP_START, h.raw_len) == h.raw_crc32;
    feed_wdt();
    if (!same) nrfx_nvmc_page_erase(FW_OTA_SLOT_ADDR);
  }
  fw_ota_ctrl_t c;
  memset(&c, 0, sizeof(c));
  c.apply_state = FW_OTA_APPLY_IDLE;
  ctrl_write(&c);
}
