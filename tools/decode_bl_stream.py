"""Decode Valve's bootloader serial protocol from a USBPcap capture and rebuild the image.

Framing (observed 2026-10-09, puck bootloader 28DE:1007, CDC bulk EP 0x01 OUT / 0x82 IN):
  0xAD <msg id> [payload] 0xAE      (host -> device payload starts with a constant 0x12)
  escape: 0xAC k  ->  0xAC + k      (so AC->AC 00, AD->AC 01, AE->AC 02)

Usage:
  python -I tools/decode_bl_stream.py <capture.pcap> --dev 5 [--fw <file.fw>] [--out <image.bin>]
"""
import argparse
import os
import struct
import sys
import zlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from usbpcap_parse import read_packets  # noqa: E402

SOF, EOF, ESC = 0xAD, 0xAE, 0xAC
MSG = {0x33: "INFO", 0x34: "FW_BEGIN", 0x35: "FW_DATA", 0x36: "FW_END", 0x37: "RESET"}


def frames(stream):
    """Split a raw byte stream into unescaped frames (bytes between SOF and EOF)."""
    out, cur, esc = [], None, False
    for b in stream:
        if b == SOF:
            cur, esc = bytearray(), False
        elif b == EOF and cur is not None:
            out.append(bytes(cur))
            cur = None
        elif cur is not None:
            if esc:
                cur.append(ESC + b)
                esc = False
            elif b == ESC:
                esc = True
            else:
                cur.append(b)
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("pcap")
    ap.add_argument("--dev", type=int, required=True)
    ap.add_argument("--ep-out", type=lambda s: int(s, 0), default=0x01)
    ap.add_argument("--ep-in", type=lambda s: int(s, 0), default=0x82)
    ap.add_argument("--fw", help="Steam .fw file to compare against")
    ap.add_argument("--out", help="write the reassembled image payload here")
    a = ap.parse_args()

    # Keep host and device traffic in time order so requests pair with replies.
    events = []
    for p in read_packets(a.pcap):
        if p.dev != a.dev or p.xfer != 3 or not p.data:
            continue
        if p.ep == a.ep_out and not p.completion:
            events.append((p.ts, "H>D", p.data))
        elif p.ep == a.ep_in and p.completion:
            events.append((p.ts, "D>H", p.data))

    image, header = bytearray(), None
    for ts, way, raw in events:
        for f in frames(raw):
            if way == "H>D":
                mid, body = f[0], f[1:]
                name = MSG.get(mid, f"0x{mid:02x}")
                extra = ""
                if mid == 0x35:
                    (n,) = struct.unpack_from("<H", body, 1)
                    chunk = body[3:]
                    extra = f" len-field={n} chunk={len(chunk)}" + ("" if n == len(chunk) else " MISMATCH")
                    image += chunk
                elif mid == 0x36:
                    header = body[1:]
                    magic, length, crc = struct.unpack_from("<III", header)
                    extra = f" header magic=0x{magic:08x} len={length} crc32=0x{crc:08x}"
                print(f"{ts:9.4f} H>D {name:<8} prefix=0x{body[0]:02x} raw={len(raw)}B unescaped={len(f)}B{extra}")
            else:
                status = f[0] if f else None
                print(f"{ts:9.4f} D>H reply status=0x{status:02x} {len(f) - 1}B payload")
                if len(f) > 8:
                    body = f[1:]
                    print("          hex:", body.hex(" "))
                    words = struct.unpack_from(f"<{min(len(body) // 4, 12)}I", body)
                    print("          u32:", " ".join(f"{w:08x}" for w in words))

    print(f"\nreassembled image: {len(image)} bytes, crc32=0x{zlib.crc32(image) & 0xFFFFFFFF:08x}")
    if a.out:
        open(a.out, "wb").write(image)
        print(f"wrote {a.out}")
    if a.fw:
        fw = open(a.fw, "rb").read()
        print(f"{os.path.basename(a.fw)}: payload {len(fw) - 32} bytes")
        print(f"  payload identical to wire image : {fw[32:] == bytes(image)}")
        print(f"  header identical to FW_END body : {header is not None and fw[:32] == bytes(header[:32])}")


if __name__ == "__main__":
    main()
