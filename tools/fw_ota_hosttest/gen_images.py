#!/usr/bin/env python3
"""Generate slot/app test images for the fw_ota host harness.

The slot layout here follows the APPLICATION's writer (the source of truth for
what is really in flash): src/helpers/fw_ota/FwOtaSlots.{h,cpp} in the app repo
- a 36-byte SlotHeader alone in the slot's first 4 KiB page, payload starting at
slot + 0x1000, payload_crc32 over the payload only. Not the layout the
bootloader's comments describe.

Usage: gen_images.py OUTDIR [--real firmware.bin]
"""
import lzma, os, random, struct, sys, zlib

SLOT_MAGIC = 0x4F544C46
PAYLOAD_OFFSET = 0x1000
HDR_FMT = '<IIIIHHII5s3s'   # magic payload_len raw_len payload_crc comp hdrver fwver raw_crc props reserved
assert struct.calcsize(HDR_FMT) == 36
LC, LP, PB, DICT = 3, 0, 2, 8192

TAG = struct.pack('<IHH8s', 0x4C505746, 1, 0, b'RAK3401\0')   # fw_ota firmware tag, at image offset 0x200
TAG_OFF = 0x200

def fake_firmware(n, seed, tag=True):
    """Semi-compressible pseudo machine code (LZMA ratio ~0.6-0.7, like real ARM code)."""
    r = random.Random(seed)
    ops = [r.getrandbits(16) for _ in range(40)]
    blocks = [bytes(r.getrandbits(8) for _ in range(r.randint(16, 96))) for _ in range(48)]
    out = bytearray()
    while len(out) < n:
        x = r.random()
        if x < 0.06:
            out += bytes(r.getrandbits(8) for _ in range(r.randint(4, 16)))      # literals / constants
        elif x < 0.40:
            out += r.choice(blocks)                                              # repeated snippets
        else:
            op = r.choice(ops)
            out += struct.pack('<HH', op, (r.getrandbits(16) & 0x00FF) | (r.choice(ops) & 0xFF00))
    out = bytearray(out[:n])
    struct.pack_into('<II', out, 0, 0x20040000, 0x00026101)   # plausible initial SP / reset vector
    out[TAG_OFF:TAG_OFF + 16] = TAG if tag else b'\xff' * 16
    return bytes(out)

def lzma_raw(data):
    f = [{'id': lzma.FILTER_LZMA1, 'dict_size': DICT, 'lc': LC, 'lp': LP, 'pb': PB}]
    return lzma.compress(data, format=lzma.FORMAT_RAW, filters=f)

def props():
    return struct.pack('<BI', (PB * 5 + LP) * 9 + LC, DICT)

def make_slot(raw, payload=None, *, payload_crc=None, raw_crc=None, fw_version=1, magic=SLOT_MAGIC, raw_len=None):
    if payload is None:
        payload = lzma_raw(raw)
    pc = zlib.crc32(payload) & 0xFFFFFFFF if payload_crc is None else payload_crc
    rc = zlib.crc32(raw) & 0xFFFFFFFF if raw_crc is None else raw_crc
    hdr = struct.pack(HDR_FMT, magic, len(payload), len(raw) if raw_len is None else raw_len, pc, 2, 1, fw_version, rc, props(), b'\0\0\0')
    page = hdr + b'\xff' * (PAYLOAD_OFFSET - len(hdr))
    return page + payload

def write(d, name, data):
    with open(os.path.join(d, name), 'wb') as f:
        f.write(data)

def main():
    out = sys.argv[1]
    real = sys.argv[sys.argv.index('--real') + 1] if '--real' in sys.argv else None
    os.makedirs(out, exist_ok=True)

    for prefix, raw_len, old_len in (('small', 48 * 1024, 36 * 1024), ('real', None, 300 * 1024)):
        if prefix == 'real':
            if not real:
                continue
            raw = open(real, 'rb').read()
            if raw[TAG_OFF:TAG_OFF + 16] != TAG: sys.exit(f'--real image has no firmware tag at 0x{TAG_OFF:X}: rebuild the app with the tag')
        else:
            raw = fake_firmware(raw_len, 1)
        old = fake_firmware(old_len, 2)
        payload = lzma_raw(raw)
        write(out, f'{prefix}_app_old.bin', old)
        write(out, f'{prefix}_raw_new.bin', raw)
        write(out, f'{prefix}_slot_good.bin', make_slot(raw, payload))
        # payload bit flipped after the CRC was computed -> payload CRC mismatch
        bad = bytearray(payload); bad[len(bad) // 2] ^= 0x40
        write(out, f'{prefix}_slot_badpayload.bin', make_slot(raw, bytes(bad), payload_crc=zlib.crc32(payload) & 0xFFFFFFFF))
        # payload intact, but the raw-image CRC in the header is wrong
        write(out, f'{prefix}_slot_badraw.bin', make_slot(raw, payload, raw_crc=(zlib.crc32(raw) ^ 0xDEADBEEF) & 0xFFFFFFFF))
        # payload CRC is right but the bytes are not decodable LZMA
        junk = bytes(random.Random(7).getrandbits(8) for _ in range(len(payload)))
        write(out, f'{prefix}_slot_garbage.bin', make_slot(raw, junk))
        # header claims an image bigger than the app region (0x70000): must be refused, never erased for
        write(out, f'{prefix}_slot_oversize.bin', make_slot(raw, payload, raw_len=0x71000))
        # a perfectly valid slot (all CRCs right) whose image has NO firmware tag: must be refused
        untagged = raw[:TAG_OFF] + b'\xff' * 16 + raw[TAG_OFF + 16:]
        write(out, f'{prefix}_slot_untagged.bin', make_slot(untagged))
        print(f'{prefix}: raw={len(raw)} payload={len(payload)} ({100*len(payload)/len(raw):.0f}%) old_app={len(old)}')

if __name__ == '__main__':
    main()
