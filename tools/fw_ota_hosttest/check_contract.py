#!/usr/bin/env python3
"""Check that the bootloader (src/fw_ota/fw_ota.h) and the application
(helpers/fw_ota/FwOtaSlots.h) still agree on flash geometry, slot/control-page
layout and struct field order. The two are written independently and only
'must match' by comment - the slot payload-offset bug (2026-09-18) was exactly
this kind of drift.

Usage: check_contract.py [APP_REPO]   (default: $FW_OTA_APP_REPO or ~/mc_firmware/RAK3401_repeater)
Exit 0: agree (or the app repo is absent -> skipped).  Exit 1: mismatch.
"""
import os, re, sys

HERE = os.path.dirname(os.path.abspath(__file__))
app_repo = sys.argv[1] if len(sys.argv) > 1 else os.environ.get(
    'FW_OTA_APP_REPO', os.path.expanduser('~/mc_firmware/RAK3401_repeater'))
app_h = os.path.join(app_repo, 'src/helpers/fw_ota/FwOtaSlots.h')
bl_h = os.path.join(HERE, '../../src/fw_ota/fw_ota.h')
if not os.path.exists(app_h):
    print(f'contract: app header not found ({app_h}) - skipped'); sys.exit(0)

def strip(t):
    t = re.sub(r'/\*.*?\*/', '', t, flags=re.S)
    return re.sub(r'//[^\n]*', '', t)
app = strip(open(app_h).read()); bl = strip(open(bl_h).read())

def app_consts():
    """name -> value for 'static const uint32_t NAME = expr;' (evaluated with earlier names)."""
    env = {}
    for name, expr in re.findall(r'static const uint32_t\s+(\w+)\s*=\s*([^;]+);', app):
        env[name] = eval(expr, {}, dict(env))
    return env

def bl_consts():
    env = {}
    for name, expr in re.findall(r'#define\s+(FW_OTA_\w+)\s+\(?([^\n]+?)\)?\s*(?:$|\n)', bl):
        expr = re.sub(r'(\b0x[0-9A-Fa-f]+|\b\d+)[uU]\b', r'\1', expr)
        try: env[name] = eval(expr, {}, dict(env))
        except Exception: pass
    return env

def fields(text, opener):
    m = re.search(opener + r'.*?\{(.*?)\}', text, flags=re.S)
    out = []
    for line in m.group(1).split(';'):
        line = line.strip()
        mm = re.match(r'(u?int\d+_t|char)\s+(\w+)(?:\[(\d+)\])?$', line)
        if mm: out.append((mm.group(1), mm.group(2), int(mm.group(3) or 1)))
    return out

A, B = app_consts(), bl_consts()
pairs = [('APP_REGION_START', 'FW_OTA_APP_START'), ('APP_REGION_END', 'FW_OTA_APP_END'),
         ('SLOT_ADDR', 'FW_OTA_SLOT_ADDR'), ('SLOT_SIZE', 'FW_OTA_SLOT_SIZE'),
         ('CTRL_ADDR', 'FW_OTA_CTRL_ADDR'), ('CTRL_SIZE', 'FW_OTA_CTRL_SIZE'),
         ('FLASH_PAGE_SIZE', 'FW_OTA_PAGE_SIZE'), ('APP_MAX_SIZE', 'FW_OTA_APP_MAX'),
         ('SLOT_PAYLOAD_OFFSET', 'FW_OTA_SLOT_PAYLOAD_OFFSET'), ('SLOT_PAYLOAD_MAX', 'FW_OTA_SLOT_PAYLOAD_MAX'),
         ('SLOT_MAGIC', 'FW_OTA_SLOT_MAGIC'), ('CTRL_MAGIC', 'FW_OTA_CTRL_MAGIC'),
         ('TAG_MAGIC', 'FW_OTA_TAG_MAGIC'), ('TAG_VERSION', 'FW_OTA_TAG_VERSION')]
bad = 0
for a, b in pairs:
    if a not in A or b not in B: print(f'  ?  cannot evaluate {a} / {b}'); bad += 1; continue
    ok = A[a] == B[b]; bad += not ok
    print(f"  {'ok ' if ok else 'BAD'} {a:22s} app=0x{A[a]:X}  bootloader {b}=0x{B[b]:X}")

# compare field lists
# the bootloader has two packed structs; pick by content
def bl_struct(end_name):
    m = re.search(r'typedef struct __attribute__\(\(packed\)\)\s*\{([^}]*)\}\s*' + end_name, bl, flags=re.S)
    out = []
    for line in m.group(1).split(';'):
        mm = re.match(r'\s*(u?int\d+_t|char)\s+(\w+)(?:\[(\d+)\])?\s*$', line.strip())
        if mm: out.append((mm.group(1), mm.group(2), int(mm.group(3) or 1)))
    return out
for what, a_fields, b_fields in (
        ('SlotHeader  vs fw_ota_slot_hdr_t', fields(app, r'struct SlotHeader'), bl_struct('fw_ota_slot_hdr_t')),
        ('CtrlBlock   vs fw_ota_ctrl_t', fields(app, r'struct CtrlBlock'), bl_struct('fw_ota_ctrl_t')),
        ('FwTag       vs fw_ota_tag_t', fields(app, r'struct FwTag'), bl_struct('fw_ota_tag_t'))):
    # compare types + array sizes + order (names may differ in case only by convention; require equal)
    ok = a_fields == b_fields; bad += not ok
    print(f"  {'ok ' if ok else 'BAD'} {what}: {len(a_fields)} fields")
    if not ok:
        for x, y in zip(a_fields + [None] * 40, b_fields + [None] * 40):
            if x != y and (x or y): print(f'       app={x}  bootloader={y}')

# board id string in the tag
ab = re.search(r'#define\s+FW_OTA_TAG_BOARD\s+"([^"]*)"', app); bb = re.search(r'#define\s+FW_OTA_BOARD_ID\s+"([^"]*)"', bl)
ok = bool(ab and bb and ab.group(1) == bb.group(1)); bad += not ok
print(f"  {'ok ' if ok else 'BAD'} tag board id: app={ab.group(1) if ab else None!r} bootloader={bb.group(1) if bb else None!r}")
print('contract: ' + ('MISMATCH' if bad else 'app and bootloader agree'))
sys.exit(1 if bad else 0)
