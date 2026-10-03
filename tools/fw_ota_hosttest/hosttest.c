/*
 * fw_ota host-side test harness.
 *
 * Compiles the UNMODIFIED bootloader sources on the host:
 *   - src/fw_ota/fw_ota_apply.c   (the logic under test, included directly so
 *                                  its statics can be reset between "boots")
 *   - src/flash_nrf5x.c           (the real page-buffering flash writer)
 *   - src/lzma/LzmaDec.c          (the real decoder)
 *   - lib/sdk/.../crc16.c         (the real CRC-16 used for app validity)
 * against a simulated nRF52840 flash mapped at its REAL addresses (0x26000..
 * 0x100000), so the code's raw-pointer flash reads work as written.
 *
 * Flash model: erase sets a page to 0xFF; programming can only clear bits
 * (writing a 1 over a 0 is counted as a violation). Power can be cut at any
 * erase or program call; the interrupted operation leaves a worst-case partial
 * result (garbage / half-erased page, or a prefix of words plus a partly
 * programmed word). After a cut RAM is lost (GPREGRET, double-reset magic and
 * all statics are reset), as on real power loss.
 *
 * What is NOT modelled: the SoftDevice, BLE, USB, timing other than a coarse
 * watchdog estimate, and the real check_dfu_mode() (a faithful reduction of it
 * is in boot_once()). This tests the state machine and the power-cut safety of
 * the apply path, not the radio or the hardware.
 *
 * Slot images follow the APPLICATION's layout (header page + payload at +0x1000)
 * - see gen_images.py.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdarg.h>
#include <setjmp.h>
#include <sys/mman.h>

#include "nrf.h"
#include "nrfx_nvmc.h"
#include "boards.h"
#include "bootloader.h"
#include "bootloader_types.h"
#include "dfu_types.h"
#include "crc16.h"
#include "nrf_wdt.h"
#include "fw_ota.h"

#define FLASH_LO 0x26000u
#define FLASH_HI 0x100000u

/* ===================================================================== */
/* simulated hardware                                                     */
/* ===================================================================== */
sim_power_t g_sim_power;
sim_wdt_t   g_sim_wdt;
static uint32_t g_sim_dbl;
uint32_t *dbl_reset_mem = &g_sim_dbl;

static bool g_button, g_sd_in_progress, g_wdt_started;
bool button_pressed(uint32_t p) { (void)p; return g_button; }
bool bootloader_dfu_sd_in_progress(void) { return g_sd_in_progress; }
bool nrf_wdt_started(sim_wdt_t const *w) { (void)w; return g_wdt_started; }

static long g_unfed_us, g_max_unfed_us;
static bool g_wdt_fired;
#define WDT_LIMIT_US 2000000L          /* assume a conservative 2 s app timeout */
static void tick_us(long us) {
  g_unfed_us += us;
  if (g_unfed_us > g_max_unfed_us) g_max_unfed_us = g_unfed_us;
  if (g_wdt_started && g_unfed_us > WDT_LIMIT_US) g_wdt_fired = true;
}
void nrf_wdt_reload_request_set(sim_wdt_t *w, uint8_t rr) { (void)w; (void)rr; g_unfed_us = 0; }

typedef enum { E_NONE, E_CUT, E_RESET, E_FAULT } ev_t;
static jmp_buf g_jb;
static ev_t g_ev;
static int g_op, g_cut_at = -1;
static uint64_t g_rng = 88172645463325252ull;
static uint32_t rnd(void) {
  g_rng ^= g_rng << 13; g_rng ^= g_rng >> 7; g_rng ^= g_rng << 17;
  return (uint32_t)(g_rng >> 11);
}

static long st_erase_app, st_erase_slot, st_erase_ctrl, st_erase_set, st_violations;

void NVIC_SystemReset(void) { g_ev = E_RESET; longjmp(g_jb, 1); }

void nrfx_nvmc_page_erase(uint32_t a) {
  if ((a & 0xFFF) || a < FLASH_LO || a >= FLASH_HI) { fprintf(stderr, "BAD erase 0x%08x\n", a); abort(); }
  g_op++;
  tick_us(85000);
  uint8_t *p = (uint8_t *)(uintptr_t)a;
  if (g_op == g_cut_at) {
    switch (rnd() % 3) {
      case 0: for (int i = 0; i < 4096; i++) p[i] = (uint8_t)rnd(); break;   /* garbage      */
      case 1: memset(p, 0xFF, rnd() % 4096); break;                          /* half erased  */
      default: break;                                                        /* not started  */
    }
    g_ev = E_CUT; longjmp(g_jb, 1);
  }
  memset(p, 0xFF, 4096);
  if (a >= FW_OTA_APP_START && a < FW_OTA_APP_END) st_erase_app++;
  else if (a >= FW_OTA_SLOT_ADDR && a < FW_OTA_CTRL_ADDR) st_erase_slot++;
  else if (a == FW_OTA_CTRL_ADDR) st_erase_ctrl++;
  else if (a == BOOTLOADER_SETTINGS_ADDRESS) st_erase_set++;
}

void nrfx_nvmc_words_write(uint32_t a, void const *src, uint32_t n) {
  if ((a & 3) || a < FLASH_LO || (uint64_t)a + 4ull * n > FLASH_HI) { fprintf(stderr, "BAD write 0x%08x n=%u\n", a, n); abort(); }
  g_op++;
  tick_us((long)n * 41);
  uint32_t *dst = (uint32_t *)(uintptr_t)a;
  const uint32_t *s = (const uint32_t *)src;
  bool cut = (g_op == g_cut_at);
  uint32_t k = cut ? rnd() % (n + 1) : n;           /* words completed before power died */
  for (uint32_t i = 0; i < k; i++) {
    if ((dst[i] & s[i]) != s[i]) st_violations++;   /* tried to set a 0 bit back to 1     */
    dst[i] &= s[i];
  }
  if (cut) {
    if (k < n) dst[k] &= (s[k] | (rnd() & rnd()));  /* a partly programmed word            */
    g_ev = E_CUT; longjmp(g_jb, 1);
  }
}

/* stock bootloader_dfu_update_process(APP_COMPLETE) as it runs when is_ota() is false */
static bootloader_settings_t g_stub_settings;
void bootloader_dfu_update_process(dfu_update_status_t s) {
  g_stub_settings.bank_0_crc  = s.app_crc;
  g_stub_settings.bank_0_size = s.app_size;
  g_stub_settings.bank_0      = BANK_VALID_APP;
  g_stub_settings.bank_1      = BANK_INVALID_APP;
  nrfx_nvmc_page_erase(BOOTLOADER_SETTINGS_ADDRESS);
  nrfx_nvmc_words_write(BOOTLOADER_SETTINGS_ADDRESS, &g_stub_settings, sizeof(g_stub_settings) / 4);
}

/* stock bootloader_app_is_valid(), with a bus fault where the real one would
 * walk off the end of flash (bank_0_size garbage after a partial settings write) */
bool bootloader_app_is_valid(void) {
  uint32_t const app = FW_OTA_APP_START;
  if (*(uint32_t *)(uintptr_t)app == 0xFFFFFFFFu && *(uint32_t *)(uintptr_t)(app + 4) == 0xFFFFFFFFu) return false;
  bootloader_settings_t const *s = (bootloader_settings_t const *)(uintptr_t)BOOTLOADER_SETTINGS_ADDRESS;
  if (s->bank_0 != BANK_VALID_APP) return false;
  uint16_t crc = 0;
  if (s->bank_0_crc != 0) {
    if ((uint64_t)app + s->bank_0_size > FLASH_HI) { g_ev = E_FAULT; longjmp(g_jb, 1); }
    crc = crc16_compute((uint8_t *)(uintptr_t)app, s->bank_0_size, NULL);
  }
  return crc == s->bank_0_crc;
}

/* ===================================================================== */
/* code under test                                                        */
/* ===================================================================== */
#include "flash_nrf5x.c"     /* real page-buffering writer (its statics reset per boot) */
#include "fw_ota_apply.c"    /* the logic under test                                    */

/* ===================================================================== */
/* test utilities                                                         */
/* ===================================================================== */
static int g_checks, g_fail;
static char g_ctx[200];
#define CHECK(cond, ...) do { g_checks++; if (!(cond)) { g_fail++; \
    printf("  FAIL [%s] ", g_ctx); printf(__VA_ARGS__); printf("\n"); } } while (0)

static uint8_t *load(const char *dir, const char *name, size_t *len) {
  char path[512]; snprintf(path, sizeof path, "%s/%s", dir, name);
  FILE *f = fopen(path, "rb"); if (!f) return NULL;
  fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
  uint8_t *b = malloc(n); if (fread(b, 1, n, f) != (size_t)n) { fclose(f); free(b); return NULL; }
  fclose(f); *len = (size_t)n; return b;
}

typedef struct {
  const char *name;
  uint8_t *old, *raw, *good, *badpayload, *badraw, *garbage, *oversize, *untagged;
  size_t old_len, raw_len, good_len, badpayload_len, badraw_len, garbage_len, oversize_len, untagged_len;
} imgset_t;

static bool load_set(imgset_t *s, const char *dir, const char *prefix) {
  char n[128]; memset(s, 0, sizeof *s); s->name = prefix;
#define L(field, suffix) snprintf(n, sizeof n, "%s_%s.bin", prefix, suffix); \
    s->field = load(dir, n, &s->field##_len); if (!s->field) return false;
  L(old, "app_old") L(raw, "raw_new") L(good, "slot_good") L(badpayload, "slot_badpayload")
  L(badraw, "slot_badraw") L(garbage, "slot_garbage") L(oversize, "slot_oversize") L(untagged, "slot_untagged")
#undef L
  return true;
}

static uint32_t ref_crc32(const uint8_t *p, size_t n) {   /* independent bytewise CRC-32/IEEE */
  static uint32_t t[256]; static bool init;
  if (!init) { for (uint32_t i = 0; i < 256; i++) { uint32_t c = i; for (int k = 0; k < 8; k++) c = (c & 1) ? (c >> 1) ^ 0xEDB88320u : c >> 1; t[i] = c; } init = true; }
  uint32_t c = 0xFFFFFFFFu; while (n--) c = t[(c ^ *p++) & 0xFF] ^ (c >> 8); return ~c;
}

/* Guard: 0xED000..0xFE000 covers the InternalFS (settings, node identity) and the
 * bootloader's own area. No update path may modify it, whatever happens. */
#define GUARD_LO 0xED000u
#define GUARD_HI 0xFE000u
static uint8_t g_guard[GUARD_HI - GUARD_LO];
static bool g_guard_on;
static void guard_init(void) { uint64_t x = 0x243F6A8885A308D3ull; for (size_t i = 0; i < sizeof g_guard; i++) { x ^= x << 13; x ^= x >> 7; x ^= x << 17; g_guard[i] = (uint8_t)(x >> 24); } }
static void put_guard(void) { memcpy((void *)GUARD_LO, g_guard, sizeof g_guard); g_guard_on = true; }
static bool guard_ok(void) { return !g_guard_on || !memcmp((void *)GUARD_LO, g_guard, sizeof g_guard); }
static void flash_clear(void) { memset((void *)FLASH_LO, 0xFF, FLASH_HI - FLASH_LO); g_guard_on = false; }

static void put_settings(const uint8_t *app, size_t len) {
  bootloader_settings_t s; memset(&s, 0, sizeof s);
  s.bank_0 = BANK_VALID_APP; s.bank_1 = BANK_INVALID_APP; s.bank_0_size = (uint32_t)len;
  s.bank_0_crc = crc16_compute(app, (uint32_t)len, NULL);
  memcpy((void *)BOOTLOADER_SETTINGS_ADDRESS, &s, sizeof s);
}
static void put_app(const uint8_t *img, size_t len, bool settings) {
  memcpy((void *)FW_OTA_APP_START, img, len);
  if (settings) put_settings(img, len);
}
static void put_slot(const uint8_t *img, size_t len) { memcpy((void *)FW_OTA_SLOT_ADDR, img, len); }
static void put_ctrl(uint32_t state, uint32_t attempts, uint32_t fails, uint32_t blver) {
  fw_ota_ctrl_t c; memset(&c, 0, sizeof c);
  c.magic = FW_OTA_CTRL_MAGIC; c.ctrl_version = 5; c.apply_state = state;
  c.boot_attempts = attempts; c.fail_count = fails; c.requested_bl_version = blver;
  c.crc32 = ref_crc32((const uint8_t *)&c, sizeof c - 4);
  memcpy((void *)FW_OTA_CTRL_ADDR, &c, sizeof c);
}
static bool get_ctrl(fw_ota_ctrl_t *c) { memcpy(c, (void *)FW_OTA_CTRL_ADDR, sizeof *c); return c->magic == FW_OTA_CTRL_MAGIC && c->crc32 == ref_crc32((const uint8_t *)c, sizeof *c - 4); }
#define BLV ((uint32_t)MK_BOOTLOADER_VERSION)

static void stats_reset(void) { st_erase_app = st_erase_slot = st_erase_ctrl = st_erase_set = st_violations = 0; }
static void power_cycle(void) { g_sim_power.GPREGRET = 0; g_sim_power.RESETREAS = 0; g_sim_dbl = 0; }

/* fresh device: old app installed and valid, good slot, PENDING request */
static void state_pending(const imgset_t *s, const uint8_t *slot, size_t slot_len) {
  flash_clear(); power_cycle(); g_button = g_sd_in_progress = false;
  put_app(s->old, s->old_len, true); put_slot(slot, slot_len); put_ctrl(FW_OTA_APPLY_PENDING, 0, 0, BLV);
  put_guard(); stats_reset();
}
/* app absent (erased), good slot, blank control page */
static void state_noapp(const imgset_t *s) {
  flash_clear(); power_cycle(); g_button = g_sd_in_progress = false;
  put_slot(s->good, s->good_len); put_guard(); stats_reset();
}

/* 0 = neither, 1 = old image, 2 = new image, at the current flash contents */
static int app_state(const imgset_t *s) {
  if (!memcmp((void *)FW_OTA_APP_START, s->raw, s->raw_len)) return 2;
  if (!memcmp((void *)FW_OTA_APP_START, s->old, s->old_len)) return 1;
  return 0;
}
static bool slot_unchanged_generic(const uint8_t *img, size_t len) { return !memcmp((void *)FW_OTA_SLOT_ADDR, img, len); }
static bool slot_unchanged(const imgset_t *s) { return slot_unchanged_generic(s->good, s->good_len); }

typedef enum { OUT_APP, OUT_BLE, OUT_CUT, OUT_RESET, OUT_FAULT, OUT_LOOP } outcome_t;
static const char *oname(outcome_t o) {
  static const char *n[] = { "APP", "BLE_OTA", "CUT", "RESET", "FAULT", "LOOP" }; return n[o];
}

static void reset_ram(void) {                   /* everything a power loss / reset wipes */
  _fl_addr = FLASH_CACHE_INVALID_ADDR;
  memset(&g_stub_settings, 0, sizeof g_stub_settings);
  s_dfu_app_written = false;
  g_unfed_us = 0;
}

/* One bootloader boot: fw_ota_apply_on_boot() followed by a faithful reduction
 * of stock check_dfu_mode()/main(). cut_at < 0: no power cut. */
static outcome_t boot_once_inner(int cut_at) {
  g_op = 0; g_cut_at = cut_at; g_ev = E_NONE;
  reset_ram();
  if (setjmp(g_jb)) {
    if (g_ev == E_CUT)   { power_cycle(); return OUT_CUT; }
    if (g_ev == E_RESET) { g_sim_power.RESETREAS |= POWER_RESETREAS_SREQ_Msk; return OUT_RESET; }
    return OUT_FAULT;                            /* bus fault: stock handler hangs/resets forever */
  }
  fw_ota_apply_on_boot();

  uint32_t g = g_sim_power.GPREGRET;
  bool ota_magic = (g == 0xA8 || g == 0xB1), serial = (g == 0x4E), uf2 = (g == 0x57), skip = (g == 0x6D);
  bool pin = (g_sim_power.RESETREAS & POWER_RESETREAS_RESETPIN_Msk) != 0;
  bool dbl = (g_sim_dbl == 0x5A1AD5u) && pin;
  bool dfu_start = ota_magic || serial || uf2 || dbl;
  if (dfu_start || skip) g_sim_power.GPREGRET = 0;
  bool valid = bootloader_app_is_valid();
  if (!skip) {
    dfu_start = dfu_start || g_button;
    if (dfu_start || !valid) return OUT_BLE;     /* (serial/UF2 also land here; treated alike) */
  }
  if (valid) return OUT_APP;
  g_sim_power.GPREGRET = 0xA8;                   /* stock: no app -> reset into OTA DFU */
  g_ev = E_RESET; g_sim_power.RESETREAS |= POWER_RESETREAS_SREQ_Msk;
  return OUT_RESET;
}

static outcome_t boot_once(int cut_at) {
  outcome_t o = boot_once_inner(cut_at);
  CHECK(guard_ok(), "filesystem / bootloader area (0x%X-0x%X) was modified (outcome %s, cut_at %d)", GUARD_LO, GUARD_HI, oname(o), cut_at);
  return o;
}

/* boot repeatedly, no cuts, until the device settles into the app or waits in BLE OTA */
static int g_boots;
static outcome_t settle(int max) {
  for (g_boots = 1; g_boots <= max; g_boots++) {
    outcome_t o = boot_once(-1);
    if (o == OUT_APP || o == OUT_BLE || o == OUT_FAULT) return o;
  }
  return OUT_LOOP;
}

/* run fn() with a power cut at op cut_at; returns the event */
static ev_t guarded(void (*fn)(void), int cut_at) {
  g_op = 0; g_cut_at = cut_at; g_ev = E_NONE; reset_ram();
  ev_t r = E_NONE;
  if (setjmp(g_jb)) { if (g_ev == E_CUT) power_cycle(); r = g_ev; }
  else fn();
  CHECK(guard_ok(), "filesystem / bootloader area was modified during a guarded operation (cut_at %d)", cut_at);
  return r;
}

static int env_int(const char *k, int d) { const char *v = getenv(k); return v ? atoi(v) : d; }

/* ===================================================================== */
/* scenarios                                                              */
/* ===================================================================== */
static int g_seeds = 4, g_stride = 1;

static void t_normal(const imgset_t *s) {
  snprintf(g_ctx, sizeof g_ctx, "%s/normal", s->name);
  state_pending(s, s->good, s->good_len);
  outcome_t o = settle(5);
  fw_ota_ctrl_t c; bool ok = get_ctrl(&c);
  CHECK(o == OUT_APP, "outcome %s, want APP", oname(o));
  CHECK(app_state(s) == 2, "app region is not the new image");
  CHECK(slot_unchanged(s), "slot was modified");
  CHECK(ok && c.apply_state == FW_OTA_APPLY_DONE, "ctrl state %u want DONE", ok ? c.apply_state : 99);
  CHECK(c.boot_attempts == 0 && c.fail_count == 0, "counters %u/%u want 0/0", c.boot_attempts, c.fail_count);
  CHECK(st_violations == 0, "%ld flash write violations", st_violations);
  printf("  normal apply: %d flash ops, erases app/slot/ctrl/settings = %ld/%ld/%ld/%ld\n", g_op, st_erase_app, st_erase_slot, st_erase_ctrl, st_erase_set);
}

static int count_ops(const imgset_t *s, void (*setup)(const imgset_t *)) {
  setup(s); (void)settle(5); return g_op;
}
static void setup_pending(const imgset_t *s) { state_pending(s, s->good, s->good_len); }
static void setup_noapp(const imgset_t *s)   { state_noapp(s); }

static long g_dropped;
/* allow_drop: a cut inside the very first control-page write (attempt-started
 * record) may lose the update REQUEST. That is a safe outcome - the old app is
 * intact and keeps running, and the update can simply be requested again - and
 * is inherent to an erase-then-program record, so it is tolerated there only. */
static void check_converged(const imgset_t *s, const char *what, int cut, int seed, bool allow_drop) {
  snprintf(g_ctx, sizeof g_ctx, "%s/%s cut=%d seed=%d", s->name, what, cut, seed);
  outcome_t o = settle(6);
  CHECK(o == OUT_APP, "after power cut: outcome %s (boots=%d), want APP", oname(o), g_boots);
  if (o == OUT_APP) {
    if (allow_drop && app_state(s) == 1) g_dropped++;
    else {
      CHECK(app_state(s) == 2, "settled on APP but region is not the new image (state %d)", app_state(s));
      CHECK(g_boots <= 2, "took %d boots to settle", g_boots);
    }
  }
  CHECK(slot_unchanged(s), "slot was modified");
  CHECK(st_violations == 0, "%ld flash write violations", st_violations);
}

static void t_cut_sweep(const imgset_t *s, const char *what, void (*setup)(const imgset_t *)) {
  int n = count_ops(s, setup);
  int done = 0;
  for (int cut = 1; cut <= n; cut += g_stride) {
    for (int seed = 0; seed < g_seeds; seed++) {
      g_rng = 0x9E3779B97F4A7C15ull * (uint64_t)(cut * 131 + seed + 1);
      setup(s);
      outcome_t o = boot_once(cut);
      snprintf(g_ctx, sizeof g_ctx, "%s/%s cut=%d seed=%d", s->name, what, cut, seed);
      CHECK(o == OUT_CUT, "cut did not trigger (outcome %s)", oname(o));
      /* the interrupted boot must never leave the device running garbage */
      check_converged(s, what, cut, seed, cut <= 2 && setup == setup_pending);
      done++;
    }
  }
  printf("  %s sweep: %d ops x %d seeds (stride %d) = %d cut runs\n", what, n, g_seeds, g_stride, done);
}

static void t_multi_cut(const imgset_t *s) {
  int n = count_ops(s, setup_pending), runs = 300;
  for (int it = 0; it < runs; it++) {
    g_rng = 0xC0FFEEull * (uint64_t)(it + 7);
    setup_pending(s);
    int ncuts = 1 + (int)(rnd() % 4);
    for (int k = 0; k < ncuts; k++) (void)boot_once(1 + (int)(rnd() % (n + 4)));
    check_converged(s, "multicut", it, ncuts, true);
  }
  printf("  multi-cut: %d random sequences of 1-4 cuts\n", runs);
  printf("  (update request dropped, old app intact, in %ld cut runs so far)\n", g_dropped);
}

static void t_selfheal(const imgset_t *s) {
  snprintf(g_ctx, sizeof g_ctx, "%s/selfheal-blank", s->name);
  state_noapp(s); outcome_t o = settle(5);
  CHECK(o == OUT_APP && app_state(s) == 2, "no-app + valid slot: outcome %s state %d", oname(o), app_state(s));

  snprintf(g_ctx, sizeof g_ctx, "%s/selfheal-half-erased-app-DONE", s->name);
  state_noapp(s); put_app(s->old, s->old_len, true); memset((void *)(FW_OTA_APP_START + 0x3000), 0xFF, 0x2000);
  put_ctrl(FW_OTA_APPLY_DONE, 0, 0, BLV); stats_reset();
  o = settle(5);
  CHECK(o == OUT_APP && app_state(s) == 2, "half-erased app: outcome %s state %d", oname(o), app_state(s));

  snprintf(g_ctx, sizeof g_ctx, "%s/selfheal-garbage-ctrl", s->name);
  state_noapp(s); memset((void *)FW_OTA_CTRL_ADDR, 0x5A, 64); stats_reset();
  o = settle(5);
  CHECK(o == OUT_APP && app_state(s) == 2, "garbage ctrl page: outcome %s state %d", oname(o), app_state(s));

  snprintf(g_ctx, sizeof g_ctx, "%s/selfheal-app-ok-settings-lost", s->name);
  state_pending(s, s->good, s->good_len); put_app(s->raw, s->raw_len, false); memset((void *)BOOTLOADER_SETTINGS_ADDRESS, 0xFF, 4096); stats_reset();
  o = settle(5);
  CHECK(o == OUT_APP && app_state(s) == 2, "app written, settings lost: outcome %s state %d", oname(o), app_state(s));
  CHECK(st_erase_app == 0, "%ld app-region erases; the good app must not be erased again", st_erase_app);
}

static void t_explicit_dfu(const imgset_t *s) {
  struct { const char *n; uint32_t gpregret; bool dbl, button; } c[] = {
    {"start-ota(0xA8)", 0xA8, 0, 0}, {"appjump(0xB1)", 0xB1, 0, 0}, {"serial(0x4E)", 0x4E, 0, 0},
    {"uf2(0x57)", 0x57, 0, 0}, {"skip(0x6D)", 0x6D, 0, 0}, {"double-reset", 0, 1, 0}, {"dfu-button", 0, 0, 1},
  };
  for (unsigned i = 0; i < sizeof c / sizeof c[0]; i++) {
    snprintf(g_ctx, sizeof g_ctx, "%s/explicit-%s", s->name, c[i].n);
    state_noapp(s);
    g_sim_power.GPREGRET = c[i].gpregret;
    if (c[i].dbl) { g_sim_dbl = 0x5A1AD5u; g_sim_power.RESETREAS = POWER_RESETREAS_RESETPIN_Msk; }
    g_button = c[i].button;
    outcome_t o = boot_once(-1);
    CHECK(o == OUT_BLE || o == OUT_RESET, "outcome %s, want BLE_OTA/RESET (no auto-apply)", oname(o));
    CHECK(g_op == 0, "%d flash ops during an explicit DFU request (must be 0)", g_op);
    CHECK(app_state(s) == 0 && *(uint32_t *)FW_OTA_APP_START == 0xFFFFFFFFu, "app region was touched");
    g_button = false;
  }
  snprintf(g_ctx, sizeof g_ctx, "%s/no-valid-slot-goes-to-BLE", s->name);
  flash_clear(); power_cycle(); stats_reset();
  outcome_t o = settle(5);
  CHECK(o == OUT_BLE && g_op == 0, "blank device: outcome %s ops %d, want BLE_OTA/0", oname(o), g_op);
  snprintf(g_ctx, sizeof g_ctx, "%s/sd-swap-in-progress", s->name);
  state_noapp(s); g_sd_in_progress = true; o = boot_once(-1);
  CHECK(g_op == 0, "%d flash ops while a SoftDevice swap is in progress", g_op);
  g_sd_in_progress = false;
}

static void t_bad_slots(const imgset_t *s) {
  fw_ota_ctrl_t c; outcome_t o;

  snprintf(g_ctx, sizeof g_ctx, "%s/corrupt-slot(payload CRC)", s->name);
  state_pending(s, s->badpayload, s->badpayload_len);
  for (int i = 1; i <= 4; i++) { o = boot_once(-1); CHECK(o == OUT_APP, "boot %d: %s, want APP (old app must keep running)", i, oname(o)); }
  get_ctrl(&c);
  CHECK(app_state(s) == 1, "old app was modified");
  CHECK(st_erase_app == 0, "%ld app erases for a slot that fails validation", st_erase_app);
  CHECK(c.apply_state == FW_OTA_APPLY_FAILED && c.last_result == FW_OTA_R_GAVE_UP_FAILED, "ctrl state %u last %u", c.apply_state, c.last_result);

  snprintf(g_ctx, sizeof g_ctx, "%s/oversize-slot(raw_len > app region)", s->name);
  state_pending(s, s->oversize, s->oversize_len);
  for (int i = 1; i <= 4; i++) { o = boot_once(-1); CHECK(o == OUT_APP, "boot %d: %s, want APP (old app must keep running)", i, oname(o)); }
  CHECK(app_state(s) == 1, "old app was modified by an oversize slot");
  CHECK(st_erase_app == 0 && st_erase_slot == 0 && st_erase_set == 0, "erases app/slot/settings = %ld/%ld/%ld for a slot that must be refused", st_erase_app, st_erase_slot, st_erase_set);

  snprintf(g_ctx, sizeof g_ctx, "%s/untagged-slot(stock firmware staged by mistake)", s->name);
  state_pending(s, s->untagged, s->untagged_len);
  for (int i = 1; i <= 4; i++) { o = boot_once(-1); CHECK(o == OUT_APP, "boot %d: %s, want APP (old app must keep running)", i, oname(o)); }
  get_ctrl(&c);
  CHECK(app_state(s) == 1, "old app was modified by an untagged slot");
  CHECK(st_erase_app == 0, "%ld app erases for an untagged slot (must be refused BEFORE erasing)", st_erase_app);
  CHECK(c.apply_state == FW_OTA_APPLY_FAILED && c.fail_count == FW_OTA_MAX_FAILURES, "ctrl state %u fails %u", c.apply_state, c.fail_count);
  state_pending(s, s->untagged, s->untagged_len); (void)boot_once(-1); get_ctrl(&c);
  CHECK(c.last_result == FW_OTA_R_NO_TAG, "last_result %u want NO_TAG", c.last_result);
  snprintf(g_ctx, sizeof g_ctx, "%s/untagged-slot-selfheal", s->name);
  flash_clear(); power_cycle(); put_slot(s->untagged, s->untagged_len); put_guard(); stats_reset();
  o = boot_once(-1);
  CHECK(o == OUT_BLE && app_state(s) == 0, "no app + untagged slot: %s (must not self-heal from stock firmware)", oname(o));

  /* k=0: valid slot whose raw-image CRC is wrong - only detectable after decoding the whole image,
   *      so the app IS erased and the device ends in BLE OTA.
   * k=1: payload that cannot be decoded - the pre-erase peek (tag check) refuses it, so the
   *      RUNNING APP IS NEVER ERASED and it keeps booting. */
  const char *nm[2] = { "bad raw CRC", "undecodable LZMA" };
  const uint8_t *sl[2] = { s->badraw, s->garbage }; size_t sn[2] = { s->badraw_len, s->garbage_len };
  for (int k = 0; k < 2; k++) {
    snprintf(g_ctx, sizeof g_ctx, "%s/%s", s->name, nm[k]);
    state_pending(s, sl[k], sn[k]);
    outcome_t want = (k == 0) ? OUT_BLE : OUT_APP;
    /* boot until the device gives up; a user power-cycles it after each BLE OTA wait */
    outcome_t last = OUT_LOOP; int nb;
    for (nb = 1; nb <= 10; nb++) {
      last = boot_once(-1);
      get_ctrl(&c);
      if (last == want && c.apply_state == FW_OTA_APPLY_FAILED) break;
      if (last == OUT_BLE) power_cycle();
    }
    get_ctrl(&c);
    CHECK(last == want && nb <= 6, "did not settle into %s within 6 boots (last %s after %d)", oname(want), oname(last), nb);
    CHECK(c.apply_state == FW_OTA_APPLY_FAILED, "ctrl state %u after repeated failures, want FAILED", c.apply_state);
    CHECK(c.last_result == FW_OTA_R_GAVE_UP_FAILED, "last_result %u want GAVE_UP_FAILED", c.last_result);
    CHECK(c.fail_count == FW_OTA_MAX_FAILURES, "fail_count %u want %d", c.fail_count, FW_OTA_MAX_FAILURES);
    if (k == 1) {
      CHECK(app_state(s) == 1, "an undecodable slot modified the running app");
      CHECK(st_erase_app == 0, "%ld app erases for an undecodable slot (must be refused BEFORE erasing)", st_erase_app);
    }
    stats_reset();
    for (int i = 0; i < 4; i++) {
      power_cycle(); o = boot_once(-1);
      CHECK(o == want && g_op == 0, "after give-up: %s ops %d, want %s/0 (no more attempts)", oname(o), g_op, oname(want));
    }
    CHECK(slot_unchanged_generic(sl[k], sn[k]), "slot was modified");
  }
}

static void t_fence_idempotent_noop(const imgset_t *s) {
  fw_ota_ctrl_t c; outcome_t o;

  snprintf(g_ctx, sizeof g_ctx, "%s/version-fence", s->name);
  state_pending(s, s->good, s->good_len); put_ctrl(FW_OTA_APPLY_PENDING, 0, 0, BLV + 1);
  o = boot_once(-1); get_ctrl(&c);
  CHECK(o == OUT_APP && app_state(s) == 1, "stale request must leave the old app running (outcome %s)", oname(o));
  CHECK(c.apply_state == FW_OTA_APPLY_FAILED && c.last_result == FW_OTA_R_STALE_VERSION, "ctrl %u/%u", c.apply_state, c.last_result);
  CHECK(st_erase_app == 0 && c.boot_attempts == 0, "fence touched the app or moved a counter");

  snprintf(g_ctx, sizeof g_ctx, "%s/idempotent", s->name);
  state_pending(s, s->good, s->good_len); put_app(s->raw, s->raw_len, false); memcpy((void *)FW_OTA_APP_START, s->raw, s->raw_len);
  memset((void *)BOOTLOADER_SETTINGS_ADDRESS, 0xFF, 4096); put_settings(s->raw, s->raw_len); stats_reset();
  o = settle(3); get_ctrl(&c);
  CHECK(o == OUT_APP && c.apply_state == FW_OTA_APPLY_DONE && c.last_result == FW_OTA_R_ALREADY_APPLIED, "outcome %s state %u last %u", oname(o), c.apply_state, c.last_result);
  CHECK(st_erase_app == 0, "%ld app erases although the app already matched the slot", st_erase_app);

  snprintf(g_ctx, sizeof g_ctx, "%s/done-and-failed-are-no-ops", s->name);
  state_pending(s, s->good, s->good_len); put_ctrl(FW_OTA_APPLY_DONE, 0, 0, BLV);
  o = boot_once(-1); CHECK(o == OUT_APP && g_op == 0, "DONE + valid app: %s, %d flash ops (want APP/0: no wear)", oname(o), g_op);
  state_pending(s, s->good, s->good_len); put_ctrl(FW_OTA_APPLY_FAILED, 3, 3, BLV);
  o = boot_once(-1); CHECK(o == OUT_APP && g_op == 0, "FAILED + valid app: %s, %d flash ops (want APP/0)", oname(o), g_op);
}

static void t_interrupt_cap(const imgset_t *s) {
  fw_ota_ctrl_t c; outcome_t o;
  snprintf(g_ctx, sizeof g_ctx, "%s/interrupt-cap", s->name);
  state_pending(s, s->good, s->good_len);
  for (int i = 1; i <= FW_OTA_MAX_INTERRUPTED; i++) {
    g_rng = 12345 + i;
    o = boot_once(3);                              /* op 3 = first app-region erase, right after 'attempt started' */
    get_ctrl(&c);
    CHECK(o == OUT_CUT, "boot %d: %s want CUT", i, oname(o));
    CHECK(c.boot_attempts == (uint32_t)i && c.fail_count == 0, "after cut %d: attempts %u fails %u", i, c.boot_attempts, c.fail_count);
  }
  o = boot_once(-1); get_ctrl(&c);
  CHECK(o == OUT_RESET, "9th boot: %s, want RESET into BLE OTA (gave up)", oname(o));
  CHECK(c.apply_state == FW_OTA_APPLY_FAILED && c.last_result == FW_OTA_R_GAVE_UP_INTR, "ctrl %u/%u want FAILED/GAVE_UP_INTR", c.apply_state, c.last_result);
  o = boot_once(-1); CHECK(o == OUT_BLE, "next boot: %s want BLE_OTA", oname(o));
  printf("  interrupt cap: gave up after %d interrupted attempts, failure budget untouched\n", FW_OTA_MAX_INTERRUPTED);

  snprintf(g_ctx, sizeof g_ctx, "%s/outage-does-not-spend-failure-budget", s->name);
  state_pending(s, s->good, s->good_len);
  for (int i = 0; i < FW_OTA_MAX_FAILURES + 1; i++) { g_rng = 999 + i; (void)boot_once(3); }
  o = settle(4); get_ctrl(&c);
  CHECK(o == OUT_APP && app_state(s) == 2, "%d outages then power stays: %s state %d, want APP/new", FW_OTA_MAX_FAILURES + 1, oname(o), app_state(s));
}

/* raw-upload tag check: random packet sizes (multiples of 4, incl. ones straddling the 1 KiB
 * boundary). A tagged image must pass every packet; an untagged one must be rejected exactly at
 * the packet that completes the first KiB, with fewer than a KiB of it having been passed on. */
static int head_run(const uint8_t *img, size_t n, size_t *passed_before_reject) {
  fw_ota_head_begin(true);
  size_t off = 0, passed = 0;
  while (off < n) {
    uint32_t len = 4 * (1 + rnd() % 128); if (off + len > n) len = (uint32_t)(n - off);
    int r = fw_ota_head_feed(img + off, len, (uint32_t)off);
    if (r == FW_OTA_HEAD_REJECT) { *passed_before_reject = passed; CHECK(off + len >= FW_OTA_TAG_SCAN && off < FW_OTA_TAG_SCAN, "rejected at offset %zu (must be the packet completing the first KiB)", off); return 1; }
    passed += len; off += len;
  }
  *passed_before_reject = passed;
  return 0;
}

static void t_head_check(const imgset_t *s) {
  snprintf(g_ctx, sizeof g_ctx, "%s/raw-upload-tag-check", s->name);
  for (int i = 0; i < 60; i++) {
    g_rng = 777 + i; size_t p = 0;
    CHECK(head_run(s->raw, s->raw_len, &p) == 0 && p == s->raw_len, "tagged image was rejected or truncated (seed %d)", i);
  }
  uint8_t *un = malloc(s->raw_len); memcpy(un, s->raw, s->raw_len); memset(un + 0x200, 0xFF, 16);
  for (int i = 0; i < 60; i++) {
    g_rng = 991 + i; size_t p = 0;
    CHECK(head_run(un, s->raw_len, &p) == 1, "untagged image was accepted (seed %d)", i);
    CHECK(p < FW_OTA_TAG_SCAN, "%zu bytes were passed before an untagged image was rejected (seed %d)", p, i);
  }
  /* after a decision, later packets pass (the DFU handler blocks them itself) */
  CHECK(fw_ota_head_feed(un, 8, 4096) == FW_OTA_HEAD_PASS, "feed after the decision did not pass");
  free(un);
  fw_ota_head_begin(false);
  CHECK(fw_ota_head_feed(s->raw, 64, 0) == FW_OTA_HEAD_PASS, "check active after begin(false)");
  fw_ota_head_begin(true);
  CHECK(fw_ota_head_feed(s->raw, 64, 8) == FW_OTA_HEAD_REJECT, "an out-of-order first packet was accepted");
  /* tag placement: wrong board / wrong version / tag beyond the scan window must not pass */
  uint8_t t[2048]; memset(t, 0xFF, sizeof t);
  memcpy(t + 0x200, s->raw + 0x200, 16); CHECK(fw_ota_tag_ok(t, sizeof t), "valid tag not accepted");
  t[0x200 + 8] = 'X'; CHECK(!fw_ota_tag_ok(t, sizeof t), "wrong board id accepted");
  memcpy(t + 0x200, s->raw + 0x200, 16); t[0x200 + 4] = 9; CHECK(!fw_ota_tag_ok(t, sizeof t), "wrong tag version accepted");
  memset(t, 0xFF, sizeof t); memcpy(t + 0x400, s->raw + 0x200, 16); CHECK(!fw_ota_tag_ok(t, sizeof t), "tag beyond the scan window accepted");
}

static void t_wdt(const imgset_t *s) {
  snprintf(g_ctx, sizeof g_ctx, "%s/watchdog", s->name);
  state_pending(s, s->good, s->good_len);
  g_wdt_started = true; g_max_unfed_us = 0; g_wdt_fired = false;
  outcome_t o = settle(3);
  CHECK(o == OUT_APP && app_state(s) == 2, "apply with WDT running: %s", oname(o));
  CHECK(!g_wdt_fired, "watchdog would have fired: %ld ms without a feed (limit %ld ms)", g_max_unfed_us / 1000, (long)WDT_LIMIT_US / 1000);
  /* independent of image size: the app's real WDT timeout is unknown, so require
   * feeding often enough that any timeout of 1 s or more is safe */
  CHECK(g_max_unfed_us < 1000000L, "longest stretch without a feed is %ld ms (want < 1000 ms)", g_max_unfed_us / 1000);
  printf("  watchdog: longest modelled interval without a feed = %ld ms (limit assumed %ld ms)\n", g_max_unfed_us / 1000, (long)WDT_LIMIT_US / 1000);
  g_wdt_started = false;
}

/* raw DFU upload finished: stale slot must go, whatever the timing of a power cut */
static const imgset_t *g_cur;
static void raw_dfu_finish(void) { fw_ota_on_dfu_app_complete(g_cur ? g_cur->old_len : 1); fw_ota_after_dfu(); }
static void state_after_raw_dfu(const imgset_t *s) {
  /* a raw upload just replaced the app with the OLD image while a stale slot
   * (holding the NEW image) and a PENDING request are still in flash */
  flash_clear(); power_cycle(); g_button = g_sd_in_progress = false;
  put_app(s->old, s->old_len, true); put_slot(s->good, s->good_len); put_ctrl(FW_OTA_APPLY_PENDING, 0, 0, BLV);
  put_guard(); stats_reset();
}
static void t_raw_dfu(const imgset_t *s) {
  state_after_raw_dfu(s); guarded(raw_dfu_finish, -1);
  int n = g_op; fw_ota_ctrl_t c; get_ctrl(&c);
  snprintf(g_ctx, sizeof g_ctx, "%s/raw-dfu-invalidation", s->name);
  CHECK(*(uint32_t *)FW_OTA_SLOT_ADDR == 0xFFFFFFFFu, "slot header not erased");
  CHECK(c.apply_state == FW_OTA_APPLY_IDLE, "ctrl state %u want IDLE", c.apply_state);
  outcome_t o = boot_once(-1); CHECK(o == OUT_APP && g_op == 0, "next boot: %s %d ops, want APP/0", oname(o), g_op);
  /* only 3 ops, so use plenty of seeds: each cut point has three erase outcomes
   * (garbage / half-erased / not started) and all must be exercised */
  int seeds = g_seeds > 24 ? g_seeds : 24;
  long window = 0;
  uint8_t snap[8192];
  for (int cut = 1; cut <= n; cut++) for (int seed = 0; seed < seeds; seed++) {
    g_rng = 4242 + cut * 17 + seed;
    state_after_raw_dfu(s);
    memcpy(snap, (void *)FW_OTA_SLOT_ADDR, 4096); memcpy(snap + 4096, (void *)FW_OTA_CTRL_ADDR, 4096);
    guarded(raw_dfu_finish, cut);
    snprintf(g_ctx, sizeof g_ctx, "%s/raw-dfu cut=%d seed=%d", s->name, cut, seed);
    if (!memcmp(snap, (void *)FW_OTA_SLOT_ADDR, 4096) && !memcmp(snap + 4096, (void *)FW_OTA_CTRL_ADDR, 4096)) {
      /* power died before the invalidation changed anything: PENDING + a valid
       * stale slot are still there. Indistinguishable from a reset before the
       * hook ran; documented, cannot be closed from this hook. */
      window++; continue;
    }
    o = settle(4);
    CHECK(o == OUT_APP, "outcome %s want APP", oname(o));
    CHECK(app_state(s) == 1, "the raw-uploaded app was replaced by the stale slot's image (state %d)", app_state(s));
  }
  printf("  (%ld cut runs changed nothing yet - the known 'reset before invalidation' window - and were not checked)\n", window);
  printf("  raw-DFU invalidation: %d ops, cut at each with %d seeds\n", n, seeds);
}

/* rev 7 (C10): a raw upload of EXACTLY the image in the slot keeps the slot (the node serves what it runs); ctrl still reset */
static void t_raw_dfu_same(const imgset_t *s) {
  flash_clear(); power_cycle(); g_button = g_sd_in_progress = false;
  put_app(s->raw, s->raw_len, true); put_slot(s->good, s->good_len); put_ctrl(FW_OTA_APPLY_PENDING, 0, 0, BLV);   /* app = the slot's raw image */
  put_guard(); stats_reset();
  const uint32_t before = *(uint32_t *)FW_OTA_SLOT_ADDR;
  fw_ota_on_dfu_app_complete(s->raw_len); fw_ota_after_dfu();
  fw_ota_ctrl_t c; get_ctrl(&c);
  snprintf(g_ctx, sizeof g_ctx, "%s/raw-dfu-same-app", s->name);
  CHECK(*(uint32_t *)FW_OTA_SLOT_ADDR == before && before != 0xFFFFFFFFu, "slot holding the uploaded app was erased");
  CHECK(c.apply_state == FW_OTA_APPLY_IDLE, "ctrl state %u want IDLE", c.apply_state);
  outcome_t o = boot_once(-1); CHECK(o == OUT_APP && g_op == 0, "next boot: %s %d ops, want APP/0", oname(o), g_op);
}

/* rev 6: a raw (stock) app LARGER than the app region lies over the slot. The bootloader must not erase the "slot header" page -
 * it is app code now - but must still reset the control page (a stale PENDING must never apply anything). */
static void t_raw_dfu_big(const imgset_t *s) {
  state_after_raw_dfu(s);
  const uint32_t before = *(uint32_t *)FW_OTA_SLOT_ADDR;
  fw_ota_on_dfu_app_complete(FW_OTA_APP_MAX + FW_OTA_PAGE_SIZE); fw_ota_after_dfu();
  fw_ota_ctrl_t c; get_ctrl(&c);
  snprintf(g_ctx, sizeof g_ctx, "%s/raw-dfu-big-app", s->name);
  CHECK(*(uint32_t *)FW_OTA_SLOT_ADDR == before && before != 0xFFFFFFFFu, "page at the slot start was erased under a big raw app");
  CHECK(c.apply_state == FW_OTA_APPLY_IDLE, "ctrl state %u want IDLE", c.apply_state);
  outcome_t o = boot_once(-1); CHECK(o == OUT_APP && g_op == 0, "next boot: %s %d ops, want APP/0 (nothing applied)", oname(o), g_op);
  /* and the size limits: raw uploads up to the control page, slot images never beyond the app region */
  CHECK(FW_OTA_RAW_APP_MAX == FW_OTA_CTRL_ADDR - FW_OTA_APP_START && FW_OTA_RAW_APP_MAX > FW_OTA_APP_MAX, "raw app limit");
  CHECK(FW_OTA_APP_START + FW_OTA_RAW_APP_MAX <= FW_OTA_CTRL_ADDR, "a raw app would reach the control page");
}

int main(int argc, char **argv) {
  guard_init();
  const char *dir = argc > 1 ? argv[1] : "img";
  const char *only = argc > 2 ? argv[2] : NULL;
  g_seeds = env_int("HOSTTEST_SEEDS", 4);
  int stride_real = env_int("HOSTTEST_STRIDE_REAL", 3);

  void *m = mmap((void *)FLASH_LO, FLASH_HI - FLASH_LO, PROT_READ | PROT_WRITE,
                 MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
  if (m != (void *)FLASH_LO) { perror("mmap flash at real address"); return 2; }

  const char *sets[] = { "small", "real" };
  for (int i = 0; i < 2; i++) {
    if (only && strcmp(only, sets[i])) continue;
    imgset_t s;
    if (!load_set(&s, dir, sets[i])) { printf("[%s] images not found in %s - skipped\n", sets[i], dir); continue; }
    g_stride = (i == 1) ? stride_real : 1;
    printf("== image set: %s (raw %zu B, slot %zu B)\n", s.name, s.raw_len, s.good_len);
    t_normal(&s);
    t_cut_sweep(&s, "pending", setup_pending);
    t_cut_sweep(&s, "selfheal", setup_noapp);
    t_multi_cut(&s);
    t_selfheal(&s);
    t_explicit_dfu(&s);
    t_bad_slots(&s);
    t_head_check(&s);
    t_fence_idempotent_noop(&s);
    t_interrupt_cap(&s);
    t_wdt(&s);
    t_raw_dfu(&s);
    t_raw_dfu_big(&s);
    t_raw_dfu_same(&s);
  }
  printf("\n%d checks, %d failed\n", g_checks, g_fail);
  return g_fail ? 1 : 0;
}
