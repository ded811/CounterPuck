"""Parse a USBPcap .pcap (linktype 249) without Wireshark.

Usage:
  python -I tools/usbpcap_parse.py <file.pcap> summary
      per device/endpoint: transfer type, direction, packet count, bytes, time span
  python -I tools/usbpcap_parse.py <file.pcap> dump [--dev N] [--ep 0xNN] [--max 64] [--limit 200]
      one line per packet that carries data (hex, truncated to --max bytes)
  python -I tools/usbpcap_parse.py <file.pcap> bulk --dev N --ep 0xNN --out <file.bin>
      concatenate bulk/interrupt payloads for one endpoint (OUT: submissions, IN: completions)
  python -I tools/usbpcap_parse.py <file.pcap> control [--dev N]
      decoded control transfers (setup packet + data), e.g. HID GET/SET_REPORT

Library use: `from usbpcap_parse import read_packets` yields Packet tuples.
"""
import argparse
import collections
import struct
import sys

LINKTYPE_USBPCAP = 249
XFER = {0: "ISO", 1: "INTR", 2: "CTRL", 3: "BULK", 0xFF: "OTHER"}
STAGE = {0: "SETUP", 1: "DATA", 2: "STATUS", 3: "COMPLETE"}

Packet = collections.namedtuple(
    "Packet", "idx ts irp status function completion bus dev ep xfer stage data")


def read_packets(path):
    with open(path, "rb") as f:
        gh = f.read(24)
        magic = struct.unpack("<I", gh[:4])[0]
        if magic not in (0xA1B2C3D4, 0xA1B23C4D):
            sys.exit("not a little-endian pcap")
        linktype = struct.unpack("<I", gh[20:24])[0]
        if linktype != LINKTYPE_USBPCAP:
            sys.exit(f"linktype {linktype}, expected {LINKTYPE_USBPCAP} (USBPcap)")
        nsdiv = 1e9 if magic == 0xA1B23C4D else 1e6
        t0, idx = None, 0
        while True:
            rh = f.read(16)
            if len(rh) < 16:
                return
            sec, frac, incl, _ = struct.unpack("<IIII", rh)
            rec = f.read(incl)
            ts = sec + frac / nsdiv
            t0 = ts if t0 is None else t0
            hlen, irp, status, func, info, bus, dev, ep, xfer, dlen = struct.unpack_from("<HQIHBHHBBI", rec, 0)
            stage = rec[27] if xfer == 2 and hlen >= 28 else None
            data = rec[hlen:hlen + dlen]
            idx += 1
            yield Packet(idx, ts - t0, irp, status, func, bool(info & 1), bus, dev, ep, xfer, stage, data)


def direction(ep):
    return "IN" if ep & 0x80 else "OUT"


def cmd_summary(pkts):
    stats = {}
    for p in pkts:
        k = (p.dev, p.ep, XFER.get(p.xfer, str(p.xfer)))
        s = stats.setdefault(k, {"n": 0, "bytes": 0, "t0": p.ts, "t1": p.ts})
        s["n"] += 1
        s["bytes"] += len(p.data)
        s["t1"] = p.ts
    print(f"{'dev':>4} {'ep':>5} {'type':>5} {'dir':>4} {'pkts':>7} {'data bytes':>11}  time span (s)")
    for (dev, ep, xt), s in sorted(stats.items()):
        print(f"{dev:>4} 0x{ep:02x} {xt:>5} {direction(ep):>4} {s['n']:>7} {s['bytes']:>11}  {s['t0']:8.3f} .. {s['t1']:8.3f}")


def cmd_dump(pkts, dev, ep, maxb, limit):
    shown = 0
    for p in pkts:
        if dev is not None and p.dev != dev:
            continue
        if ep is not None and p.ep != ep:
            continue
        if not p.data:
            continue
        tag = "C" if p.completion else "S"
        st = f" {STAGE.get(p.stage, p.stage)}" if p.stage is not None else ""
        h = p.data[:maxb].hex(" ")
        more = f" …(+{len(p.data) - maxb})" if len(p.data) > maxb else ""
        print(f"#{p.idx:<6} {p.ts:9.4f} dev{p.dev} 0x{p.ep:02x} {XFER.get(p.xfer)}{st} {tag} len={len(p.data):<5} {h}{more}")
        shown += 1
        if limit and shown >= limit:
            break


def cmd_bulk(pkts, dev, ep, out):
    want_completion = bool(ep & 0x80)  # IN data arrives on completion, OUT data on submission
    buf, n = bytearray(), 0
    for p in pkts:
        if p.dev == dev and p.ep == ep and p.xfer in (1, 3) and p.completion == want_completion and p.data:
            buf += p.data
            n += 1
    open(out, "wb").write(buf)
    print(f"wrote {len(buf)} bytes from {n} transfers -> {out}")


def cmd_control(pkts, dev):
    pending = {}
    for p in pkts:
        if p.xfer != 2 or (dev is not None and p.dev != dev):
            continue
        if not p.completion and p.stage == 0 and len(p.data) >= 8:
            bm, breq, wval, widx, wlen = struct.unpack("<BBHHH", p.data[:8])
            pending[p.irp] = [p, (bm, breq, wval, widx, wlen), p.data[8:]]
        elif not p.completion and p.stage == 1 and p.irp in pending:
            pending[p.irp][2] += p.data
        elif p.completion and p.irp in pending:
            sp, (bm, breq, wval, widx, wlen), outdata = pending.pop(p.irp)
            data = outdata if not (bm & 0x80) else p.data
            kind = ""
            if bm == 0x21 and breq == 0x09:
                kind = f"HID SET_REPORT type={wval >> 8} id={wval & 0xFF}"
            elif bm == 0xA1 and breq == 0x01:
                kind = f"HID GET_REPORT type={wval >> 8} id={wval & 0xFF}"
            elif bm == 0x80 and breq == 0x06:
                kind = f"GET_DESCRIPTOR type={wval >> 8} idx={wval & 0xFF}"
            elif bm == 0x21 and breq in (0x20, 0x22):
                kind = {0x20: "CDC SET_LINE_CODING", 0x22: "CDC SET_CONTROL_LINE_STATE"}[breq]
            print(f"#{sp.idx:<6} {sp.ts:9.4f} dev{p.dev} bm=0x{bm:02x} req=0x{breq:02x} wValue=0x{wval:04x} "
                  f"wIndex={widx} wLen={wlen} st=0x{p.status:08x} {kind}\n         {len(data)}B {data[:64].hex(' ')}"
                  + (f" …(+{len(data) - 64})" if len(data) > 64 else ""))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("pcap")
    ap.add_argument("cmd", choices=["summary", "dump", "bulk", "control"])
    ap.add_argument("--dev", type=int)
    ap.add_argument("--ep", type=lambda s: int(s, 0))
    ap.add_argument("--max", type=int, default=64)
    ap.add_argument("--limit", type=int, default=200)
    ap.add_argument("--out")
    a = ap.parse_args()
    pkts = read_packets(a.pcap)
    if a.cmd == "summary":
        cmd_summary(pkts)
    elif a.cmd == "dump":
        cmd_dump(pkts, a.dev, a.ep, a.max, a.limit)
    elif a.cmd == "bulk":
        if a.dev is None or a.ep is None or not a.out:
            sys.exit("bulk needs --dev, --ep and --out")
        cmd_bulk(pkts, a.dev, a.ep, a.out)
    else:
        cmd_control(pkts, a.dev)


if __name__ == "__main__":
    main()
