"""List / extract a PyInstaller one-file bundle (e.g. Steam's hardwareupdater.exe).

Read-only and static: nothing from the bundle is imported or executed. Code objects are
written as raw (decompressed) marshal blobs; use `strings` or a matching-version
decompiler on them later.

Usage:
  python -I tools/pyi_extract.py <bundle.exe>                 # list CArchive + PYZ contents
  python -I tools/pyi_extract.py <bundle.exe> <empty_out_dir> # also extract
"""
import marshal
import os
import struct
import sys
import zlib

MAGIC = b"MEI\x0c\x0b\x0a\x0b\x0e"
COOKIE21 = struct.Struct("!8sIIii64s")  # PyInstaller >= 2.1
TOC_ENTRY = struct.Struct("!iIIIBc")


def safe_name(name):
    name = name.replace("\\", "/").lstrip("/")
    parts = [p for p in name.split("/") if p not in ("", ".", "..")]
    return os.path.join(*parts) if parts else "_unnamed"


def read_carchive(data):
    pos = data.rfind(MAGIC)
    if pos < 0:
        sys.exit("not a PyInstaller bundle (no MEI cookie)")
    _, pkg_len, toc_off, toc_len, pyver, pylib = COOKIE21.unpack_from(data, pos)
    tail = len(data) - pos - COOKIE21.size
    overlay = len(data) - (pkg_len + tail)
    lib = pylib.rstrip(b"\0").decode(errors="replace")
    print(f"cookie @0x{pos:x}  python {pyver // 100}.{pyver % 100}  lib {lib}")
    entries, p, end = [], overlay + toc_off, overlay + toc_off + toc_len
    while p < end:
        size, epos, csize, usize, cflag, typ = TOC_ENTRY.unpack_from(data, p)
        name = data[p + TOC_ENTRY.size:p + size].rstrip(b"\0").decode(errors="replace")
        entries.append((name, overlay + epos, csize, usize, cflag, typ.decode()))
        p += size
    return entries, pyver


def blob(data, e):
    _, off, csize, _, cflag, _ = e
    raw = data[off:off + csize]
    return zlib.decompress(raw) if cflag else raw


def read_pyz(buf):
    if buf[:4] != b"PYZ\0":
        return []
    toc_pos = struct.unpack("!i", buf[8:12])[0]
    toc = marshal.loads(buf[toc_pos:])  # plain list/tuples/strings: version-independent
    items = toc.items() if isinstance(toc, dict) else toc
    out = []
    for name, (ispkg, pos, length) in items:
        out.append((name, ispkg, pos, length))
    return out


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    data = open(sys.argv[1], "rb").read()
    outdir = sys.argv[2] if len(sys.argv) > 2 else None
    if outdir:
        os.makedirs(outdir, exist_ok=True)
        if os.listdir(outdir):
            sys.exit(f"refusing: {outdir} is not empty")

    entries, pyver = read_carchive(data)
    print(f"{len(entries)} CArchive entries (type: s=entry script, z=PYZ, b=binary, x=data, m/M=module)")
    for e in entries:
        print(f"  {e[5]}  {e[3]:>9}  {e[0]}")
        if not outdir:
            continue
        payload = blob(data, e)
        suffix = ".code" if e[5] in "smM" else ""
        dest = os.path.join(outdir, "carchive", safe_name(e[0]) + suffix)
        os.makedirs(os.path.dirname(dest), exist_ok=True)
        open(dest, "wb").write(payload)
        if e[5] == "z":
            pyz = read_pyz(payload)
            print(f"     PYZ with {len(pyz)} modules")
            for name, ispkg, pos, length in pyz:
                try:
                    code = zlib.decompress(payload[pos:pos + length])
                except zlib.error:
                    code = payload[pos:pos + length]
                d = os.path.join(outdir, "pyz", safe_name(name.replace(".", "/")) + (".__init__" if ispkg else "") + ".code")
                os.makedirs(os.path.dirname(d), exist_ok=True)
                open(d, "wb").write(code)
    if not outdir:
        for e in entries:
            if e[5] == "z":
                pyz = read_pyz(blob(data, e))
                print(f"PYZ '{e[0]}' modules ({len(pyz)}), non-stdlib-looking first:")
                for name, *_ in sorted(pyz, key=lambda t: (t[0].split('.')[0] in sys.stdlib_module_names, t[0])):
                    print("   ", name)


if __name__ == "__main__":
    main()
