#!/usr/bin/env python3
"""Mutation check for the fw_ota host harness: re-introduce known/plausible bugs
into a COPY of fw_ota_apply.c and confirm the harness fails on each one.
A mutant that the harness does not catch ('SURVIVED') is a hole in the tests.

Usage: mutants.py IMGDIR
"""
import os, re, subprocess, sys, tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.join(HERE, '../../src')
ORIG = open(os.path.join(SRC, 'fw_ota/fw_ota_apply.c')).read()

def sub(pattern, repl, count=1):
    def f(s):
        n, k = re.subn(pattern, repl, s, count=count, flags=re.S)
        assert k, f'mutation pattern not found: {pattern[:50]}'
        return n
    return f

MUTANTS = {
 'payload read at slot+32 (original layout bug)':
    lambda s: s.replace('FW_OTA_SLOT_PAYLOAD_OFFSET', '32u'),
 'decoder writes >4KiB chunks straight to flash_nrf5x_write (buffer overrun)':
    sub(r'flash_write_paged\(out_start \+ out_pos, s_dic \+ \(flushed % FW_OTA_LZMA_DIC_SIZE\), chunk\);',
        'flash_nrf5x_write(out_start + out_pos, s_dic + (flushed % FW_OTA_LZMA_DIC_SIZE), (int)chunk, false);'),
 'no settings sanitisation (torn settings -> bus fault)':
    sub(r'void fw_ota_apply_on_boot\(void\) \{\n  sanitize_settings\(\);', 'void fw_ota_apply_on_boot(void) {'),
 'control record has no CRC check (torn record aliases a state)':
    sub(r'\|\|\s*c->crc32 != crc32_mem\(\(const uint8_t \*\)c, FW_OTA_CTRL_CRC_LEN, 0\)\)', ')'),
 'interrupted attempts counted as failures (outage burns the budget)':
    sub(r'c->boot_attempts\+\+;\n  ctrl_write\(c\);', 'c->boot_attempts++; c->fail_count++;\n  ctrl_write(c);'),
 'no idempotent skip (good app re-erased)':
    sub(r'if \(crc32_flash\(FW_OTA_APP_START, h\.raw_len\) == h\.raw_crc32\) \{', 'if (0) {'),
 'raw DFU does not invalidate the stale slot/PENDING (hook does nothing)':
    sub(r'void fw_ota_after_dfu\(void\) \{\n  if \(!s_dfu_app_written\) return;', 'void fw_ota_after_dfu(void) {\n  return;'),
 'slot header raw_len not bounded by the app region (erase can reach slot/FS)':
    sub(r'if \(h->raw_len == 0 \|\| h->raw_len > FW_OTA_APP_MAX\) return false;', 'if (h->raw_len == 0) return false;'),
 'apply erase runs past the app region into the filesystem':
    sub(r'flash_erase_range\(FW_OTA_APP_START, h\.raw_len\);', 'flash_erase_range(FW_OTA_APP_START, 0xD0000);'),
 'slot apply does not check the firmware tag':
    sub(r'if \(!fw_ota_slot_image_tagged\(&h\)\) \{', 'if (0) {'),
 'tag checked only AFTER the app was erased':
    lambda s: sub(r'  /\* Refuse firmware without the tag BEFORE touching the app region.*?    return false;\n  \}\n\n', '')(s).replace(
        '  record_valid_app(h.raw_len);\n  c->last_result = FW_OTA_R_OK;', '  if (!fw_ota_slot_image_tagged(&h)) { c->last_result = FW_OTA_R_NO_TAG; return false; }\n  record_valid_app(h.raw_len);\n  c->last_result = FW_OTA_R_OK;'),
 'raw-upload tag check never rejects':
    sub(r'return fw_ota_tag_ok\(s_head, s_head_filled\) \? FW_OTA_HEAD_PASS : FW_OTA_HEAD_REJECT;', 'return FW_OTA_HEAD_PASS;'),
 'raw-upload tag check rejects only after the whole image':
    sub(r'if \(s_head_filled < FW_OTA_TAG_SCAN\) return FW_OTA_HEAD_PASS;', 'if (s_head_filled < FW_OTA_TAG_SCAN + 4) return FW_OTA_HEAD_PASS;'),
 'tag check ignores the board id':
    sub(r'memcmp\(t\.board, id, sizeof\(t\.board\)\) == 0', '1'),
 'watchdog never fed':
    sub(r'static void feed_wdt\(void\) \{', 'static void feed_wdt(void) { return;'),
 'self-heal ignores explicit DFU requests':
    sub(r'if \(explicit_dfu_requested\(\)\) return;', ''),
 'no self-heal at all':
    sub(r'  fw_ota_slot_hdr_t h;\n  if \(!slot_valid\(&h\)\) return;   /\* nothing to heal from', '  return;\n  fw_ota_slot_hdr_t h;\n  if (!slot_valid(&h)) return;   /* nothing to heal from'),
 'version fence removed':
    sub(r'if \(c\.requested_bl_version != \(uint32_t\)\(MK_BOOTLOADER_VERSION\)\) \{', 'if (0) {'),
 'failure cap ignored (retries forever)':
    sub(r'if \(c->fail_count >= FW_OTA_MAX_FAILURES\) \{', 'if (0) {'),
 'rev 6: slot page erased even under a big raw app (breaks a stock app > 448 KiB)':
    sub(r'if \(s_dfu_app_size <= FW_OTA_APP_MAX\) \{', 'if (1) {'),
 'rev 7: slot erased although it holds exactly the uploaded app':
    sub(r'if \(!same\) nrfx_nvmc_page_erase\(FW_OTA_SLOT_ADDR\);', 'nrfx_nvmc_page_erase(FW_OTA_SLOT_ADDR);'),
 'rev 7: slot kept although it holds a DIFFERENT image (stale slot survives)':
    sub(r'if \(!same\) nrfx_nvmc_page_erase\(FW_OTA_SLOT_ADDR\);', '(void)same;'),
 'DONE counted like an attempt (wear + false FAILED)':
    sub(r'  /\* No apply requested\. Nothing to do unless the app is missing\. \*/',
        '  if (have_ctrl && c.apply_state == FW_OTA_APPLY_DONE) { c.boot_attempts++; ctrl_write(&c); }\n  /* No apply requested. Nothing to do unless the app is missing. */'),
}

def main():
    img = sys.argv[1]
    survived = []
    for name, fn in MUTANTS.items():
        with tempfile.TemporaryDirectory() as d:
            os.makedirs(os.path.join(d, 'fw_ota'))
            mutated = fn(ORIG)
            assert mutated != ORIG, name
            open(os.path.join(d, 'fw_ota_apply.c'), 'w').write(mutated)
            exe = os.path.join(d, 'ht')
            cmd = ['gcc', '-O1', '-g', '-w', '-no-pie', '-D_LZMA_SIZE_OPT', '-I' + d, '-I' + os.path.join(HERE, 'stubs'),
                   '-I' + os.path.join(SRC, 'fw_ota'), '-I' + SRC, '-I' + os.path.join(SRC, 'lzma'),
                   '-I' + os.path.join(SRC, '../lib/sdk/components/libraries/crc16'),
                   '-I' + os.path.join(SRC, '../lib/sdk11/components/libraries/bootloader_dfu'),
                   '-o', exe, os.path.join(HERE, 'hosttest.c'), os.path.join(SRC, 'lzma/LzmaDec.c'),
                   os.path.join(SRC, '../lib/sdk/components/libraries/crc16/crc16.c')]
            cc = subprocess.run(cmd, capture_output=True, text=True)
            if cc.returncode:
                print(f'  BUILD-ERROR  {name}\n{cc.stderr[:400]}'); survived.append(name); continue
            env = dict(os.environ, HOSTTEST_SEEDS='3')
            try:
                r = subprocess.run([exe, img, 'small'], capture_output=True, text=True, timeout=300, env=env)
                caught = r.returncode != 0
                detail = (re.findall(r'(\d+) checks, (\d+) failed', r.stdout) or [('?', '?')])[-1]
                if r.returncode < 0: detail = ('crash', f'signal {-r.returncode}')
            except subprocess.TimeoutExpired:
                caught, detail = True, ('timeout', '-')
            print(f"  {'KILLED  ' if caught else 'SURVIVED'}  {name}   [{detail[1]} failed of {detail[0]}]")
            if not caught: survived.append(name)
    print(f'\n{len(MUTANTS) - len(survived)}/{len(MUTANTS)} mutants caught')
    return 1 if survived else 0

if __name__ == '__main__':
    sys.exit(main())
