#!/usr/bin/env python3
#
# dump-xbe.py -- inspect an Xbox executable (XBE): header, sections, TLS, and the
# kernel ordinals it imports.
#
# usage: dump-xbe.py FILE.xbe
#
# The XBE format is public (xboxdevwiki; nxdk's tools/cxbe).  This is a
# from-spec reader -- it tells the XBE loader (drivers/nxbe) what a given
# title actually needs: which xboxkrnl ordinals, whether it has a TLS
# directory, how many sections, which nxdk libraries it was linked against.
#
# Ordinal numbers are named from nxdk's xboxkrnl.exe.def when that submodule
# is present (third_party/nxdk); otherwise ordinals print unnamed.
#
import re, os, struct, sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
KRNL_DEF = os.path.join(REPO, "third_party/nxdk/lib/xboxkrnl/xboxkrnl.exe.def")

XOR_EP = (0xA8FC57AB, 0x94859D4B)   # entry point      (retail, debug)
XOR_KT = (0x5B6D40B6, 0xEFB1F152)   # kernel thunk tbl (retail, debug)
THUNK_ORDINAL = 0x80000000


def load_ordinal_names():
    """ordinal -> (name, is_data) from nxdk's xboxkrnl.exe.def, if available."""
    names = {}
    if not os.path.exists(KRNL_DEF):
        return names
    # e.g.  "DbgPrint @ 8 NONAME"  /  "@ExInterlockedCompareExchange64@12 @ 21 ..."
    pat = re.compile(r'\s*@?([A-Za-z_]\w*)(?:@\d+)?\s+@\s*(\d+)\s+NONAME(\s+DATA)?')
    for line in open(KRNL_DEF):
        m = pat.match(line)
        if m:
            names[int(m.group(2))] = (m.group(1), bool(m.group(3)))
    return names


def main():
    if len(sys.argv) != 2:
        sys.exit("usage: dump-xbe.py FILE.xbe")
    data = open(sys.argv[1], "rb").read()
    u32 = lambda off: struct.unpack_from("<I", data, off)[0]

    if len(data) < 0x178 or data[:4] != b"XBEH":
        sys.exit("dump-xbe.py: not an XBE (bad magic)")

    base      = u32(0x104)
    img_size  = u32(0x10C)
    hdr_size  = u32(0x108)
    cert_addr = u32(0x118)
    nsections = u32(0x11C)
    sec_hdrs  = u32(0x120)
    initflags = u32(0x124)
    enc_entry = u32(0x128)
    tls_addr  = u32(0x12C)
    enc_thunk = u32(0x158)
    nlibs     = u32(0x160)
    libs_addr = u32(0x164)

    # An XBE is non-relocatable: every *Addr in the header is an absolute VA
    # in the image mapped at `base`.  Map a VA back to a file offset via the
    # section that contains it (or the headers region for low VAs).
    sections = []
    for i in range(nsections):
        s = sec_hdrs - base + i * 0x38
        sections.append(dict(
            flags=u32(s), va=u32(s + 4), vsize=u32(s + 8),
            raw=u32(s + 0xC), rsize=u32(s + 0x10), name_addr=u32(s + 0x14)))

    def va_to_off(va):
        if base <= va < base + hdr_size:
            return va - base
        for s in sections:
            if s["va"] <= va < s["va"] + s["vsize"]:
                d = va - s["va"]
                return s["raw"] + d if d < s["rsize"] else None
        return None

    def cstr(va):
        off = va_to_off(va)
        if off is None:
            return "?"
        end = data.index(b"\0", off)
        return data[off:end].decode("latin-1")

    def decode(enc, keys):
        """Pick the retail/debug key whose result lands inside the image."""
        for k in keys:
            if base <= (enc ^ k) < base + img_size:
                return enc ^ k
        return enc ^ keys[0]

    entry = decode(enc_entry, XOR_EP)
    thunk = decode(enc_thunk, XOR_KT)

    print("== %s (%d bytes) ==" % (os.path.basename(sys.argv[1]), len(data)))
    print("  base            %#010x" % base)
    print("  size of image   %#010x" % img_size)
    print("  size of headers %#010x" % hdr_size)
    print("  init flags      %#010x" % initflags)
    print("  entry point     %#010x" % entry)
    print("  kernel thunks   %#010x" % thunk)
    print("  TLS directory   %#010x" % tls_addr)

    # --- certificate (title name / id) ---
    if cert_addr:
        c = va_to_off(cert_addr)
        if c is not None:
            tid = u32(c + 8)
            name = data[c + 12:c + 12 + 80].decode("utf-16-le").split("\0")[0]
            print("  title           %r  (id %#010x)" % (name, tid))

    # --- sections ---
    print("\n-- %d section(s) --" % nsections)
    fbits = [(1, "W"), (2, "preload"), (4, "X"), (8, "inserted"),
             (0x10, "head-ro"), (0x20, "tail-ro")]
    for i, s in enumerate(sections):
        fl = ",".join(n for b, n in fbits if s["flags"] & b)
        print("  %-14s va=%#010x vsize=%#09x raw=%#09x rsize=%#09x  [%s]" %
              (cstr(s["name_addr"]), s["va"], s["vsize"], s["raw"],
               s["rsize"], fl))

    # --- TLS directory (IMAGE_TLS_DIRECTORY_32) ---
    if tls_addr:
        t = va_to_off(tls_addr)
        if t is not None:
            print("\n-- TLS directory --")
            print("  raw data        %#010x .. %#010x" % (u32(t), u32(t + 4)))
            print("  index addr      %#010x" % u32(t + 8))
            print("  callbacks       %#010x" % u32(t + 0xC))
            print("  zero fill       %#010x" % u32(t + 0x10))

    # --- linked nxdk libraries ---
    if nlibs and libs_addr:
        print("\n-- %d linked librar(ies) --" % nlibs)
        lo = va_to_off(libs_addr)
        for i in range(nlibs):
            e = lo + i * 0x10
            nm = data[e:e + 8].decode("latin-1").rstrip("\0")
            maj, mnr, bld, flg = struct.unpack_from("<HHHH", data, e + 8)
            print("  %-9s %d.%d.%d  (flags %#06x)" % (nm, maj, mnr, bld, flg))

    # --- kernel imports (the thunk table) ---
    names = load_ordinal_names()
    print("\n-- kernel imports (xboxkrnl ordinals) --")
    to = va_to_off(thunk)
    if to is None:
        print("  (thunk table VA not resolvable)")
        return
    ords = []
    while True:
        v = struct.unpack_from("<I", data, to)[0]
        to += 4
        if v == 0:
            break
        if v & THUNK_ORDINAL:
            ords.append(v & ~THUNK_ORDINAL)
        if len(ords) > 4096:
            break
    for o in ords:
        nm, is_data = names.get(o, ("?", False))
        print("  %4d  %s%s" % (o, nm, "   [DATA]" if is_data else ""))
    print("\n  %d ordinal(s) imported" % len(ords))


if __name__ == "__main__":
    main()
