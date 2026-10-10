"""Guess the flash load address of a raw Cortex-M image (no header) from its own pointers.

Method: collect the offsets where printable strings start, then for each candidate base
count how many 32-bit little-endian words in the image equal base + string_offset.
The right base lights up with far more hits than any other.

Usage:  python -I tools/find_base.py <image.bin | file.fw> [--step 0x100] [--max 0x80000]
        (.fw files: the 32-byte Valve header is skipped automatically)
"""
import argparse
import collections
import re
import struct


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("image")
    ap.add_argument("--step", type=lambda s: int(s, 0), default=0x100)
    ap.add_argument("--max", type=lambda s: int(s, 0), default=0x80000)
    a = ap.parse_args()

    data = open(a.image, "rb").read()
    if a.image.lower().endswith(".fw"):
        data = data[0x20:]

    # String starts: printable runs of >= 6 chars preceded by a NUL (or image start).
    starts = {m.start() for m in re.finditer(rb"[\x20-\x7e]{6,}\x00", data)
              if m.start() == 0 or data[m.start() - 1] == 0}
    words = struct.unpack_from(f"<{len(data) // 4}I", data)

    # For each candidate base, count image words that point exactly at a string start.
    wordset = collections.Counter(words)
    votes = collections.Counter()
    for base in range(0, a.max + 1, a.step):
        votes[base] = sum(wordset[base + s] for s in starts if base + s in wordset)

    print(f"{len(starts)} candidate strings, {len(words)} words")
    for base, n in votes.most_common(8):
        print(f"  base 0x{base:08x}: {n} pointer hits")


if __name__ == "__main__":
    main()
