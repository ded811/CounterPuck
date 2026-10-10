"""Inspect a Valve hardwareupdater .fw image (PROTEUS = puck, IBEX = controller).

Read-only: parses the 32-byte header, checks candidate checksums, decodes the
Cortex-M vector table, and reports entropy so we can tell plaintext from encrypted.

Usage:  python -I tools/fw_header.py <file.fw> [<file.fw> ...]
"""
import binascii
import datetime
import math
import re
import struct
import sys
import zlib

HEADER_LEN = 0x20

# Header word 0 is a per-product magic, hard-coded in Steam's hardwareupdater.
KNOWN_MAGICS = {
    0x2E795631: "PROTEUS (puck)",
    0xD2D86467: "IBEX/TRITON (controller)",
}


def entropy(buf):
    if not buf:
        return 0.0
    counts = [0] * 256
    for b in buf:
        counts[b] += 1
    n = len(buf)
    return -sum(c / n * math.log2(c / n) for c in counts if c)


def ts(v):
    return datetime.datetime.fromtimestamp(v, datetime.timezone.utc).strftime("%Y-%m-%d %H:%M:%S UTC")


def inspect(path):
    data = open(path, "rb").read()
    hdr, body = data[:HEADER_LEN], data[HEADER_LEN:]
    words = struct.unpack("<8I", hdr)
    print(f"== {path}")
    print(f"   file size         : {len(data)} bytes, payload {len(body)} bytes")
    print("   header words      : " + " ".join(f"{w:08x}" for w in words))
    print(f"   word1 == payload? : {words[1] == len(body)}")

    m = re.search(r"_FW_([0-9A-Fa-f]{8})", path)
    if m:
        print(f"   filename build ts : 0x{m.group(1)} = {ts(int(m.group(1), 16))}")

    # Candidate checksums for word0 / word2.
    cands = {
        "crc32(payload)": zlib.crc32(body) & 0xFFFFFFFF,
        "crc32(file[4:])": zlib.crc32(data[4:]) & 0xFFFFFFFF,
        "crc32(file[8:])": zlib.crc32(data[8:]) & 0xFFFFFFFF,
        "crc32(file[12:])": zlib.crc32(data[12:]) & 0xFFFFFFFF,
        "sum32(payload)": sum(body) & 0xFFFFFFFF,
        "crc_hqx(payload)": binascii.crc_hqx(body, 0),
    }
    print(f"   word0 (magic)     : 0x{words[0]:08x} = {KNOWN_MAGICS.get(words[0], 'UNKNOWN product magic')}")
    hits = [k for k, v in cands.items() if v == words[2]]
    print(f"   word2 (checksum)  : {hits or 'does not match any simple checksum'}")

    vec = struct.unpack("<16I", body[:64])
    sp, reset = vec[0], vec[1]
    handlers = [v for v in vec[1:] if v]
    print(f"   initial SP        : 0x{sp:08x}  ({'RAM' if 0x20000000 <= sp <= 0x20020000 else 'unexpected'})")
    print(f"   reset handler     : 0x{reset:08x}  (thumb bit {'set' if reset & 1 else 'CLEAR'})")
    print(f"   handler range     : 0x{min(handlers):08x} .. 0x{max(handlers):08x}")

    # Find the link base: vectors must point inside [base, base+len(body)).
    lo, hi = min(handlers) & ~1, max(handlers) & ~1
    base_min = max(0, hi - len(body) + 1)
    print(f"   link base must be : 0x{base_min:08x} .. 0x{lo:08x} (page-aligned candidates below)")
    cands_base = [b for b in range(base_min & ~0xFFF, lo + 1, 0x1000) if b <= lo]
    print("                       " + ", ".join(f"0x{b:05x}" for b in cands_base[:12]))

    block = 4096
    ents = [entropy(body[i:i + block]) for i in range(0, len(body), block)]
    print(f"   entropy (4K blks) : min {min(ents):.2f}  avg {sum(ents) / len(ents):.2f}  max {max(ents):.2f} bits/byte")
    print(f"   verdict           : {'PLAINTEXT code (low/varied entropy)' if sum(ents) / len(ents) < 7.2 else 'likely ENCRYPTED/compressed'}")
    print()


if __name__ == "__main__":
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    for p in sys.argv[1:]:
        inspect(p)
