#!/usr/bin/env python3
#
# fix-edata.py -- verify (and under LTO, repair) the PE export address
# table against the linked image's symbol table.
#
# GNU ld builds the .def-driven PE export directory from symbol values
# as they are known before LTRANS runs; symbols defined in LTO bytecode
# read as 0 at that point, so every such export gets RVA
# (0 - ImageBase) -- e.g. 0x7c000000 at IMAGEBASE 0x84000000.  A title
# thunk patched from that RVA wraps back to VA 0 and the first ordinal
# call jumps to NULL.  This runs post-link, pre-strip (it needs the
# COFF symbol table) and rewrites each ordinal's RVA from the real
# symbol address.  On non-LTO builds everything already matches and
# this is a pure verifier.
#
# usage: fix-edata.py <xboxkrnl.exe> <xboxkrnl.def>
#
import re
import struct
import subprocess
import sys
import os


def parse_def(def_path):
    """ordinal -> decorated COFF symbol name of the backing definition."""
    targets = {}
    in_exports = False
    for raw in open(def_path):
        line = raw.strip()
        if not line or line.startswith(';'):
            continue
        if line == 'EXPORTS':
            in_exports = True
            continue
        if not in_exports:
            continue
        parts = line.split()
        head = parts[0]
        m = re.search(r'@(\d+)\b', ' '.join(parts[1:]))
        if not m:
            continue
        ordinal = int(m.group(1))
        sym = head.split('=', 1)[1] if '=' in head else head
        if '.' in sym:
            continue
        targets[ordinal] = sym if sym.startswith('@') else '_' + sym
    return targets


def main():
    exe, def_path = sys.argv[1], sys.argv[2]
    targets = parse_def(def_path)

    nm = os.environ.get('NM', 'i686-w64-mingw32-nm')
    out = subprocess.check_output([nm, '--defined-only', exe],
                                  stderr=subprocess.DEVNULL).decode('latin-1')
    symva = {}
    for line in out.splitlines():
        parts = line.split()
        if len(parts) >= 3:
            symva[parts[2]] = int(parts[0], 16)

    data = bytearray(open(exe, 'rb').read())
    pe, = struct.unpack_from('<I', data, 0x3c)
    nsec, = struct.unpack_from('<H', data, pe + 6)
    optsz, = struct.unpack_from('<H', data, pe + 20)
    base, = struct.unpack_from('<I', data, pe + 24 + 28)
    erva, = struct.unpack_from('<I', data, pe + 24 + 96)
    secoff = pe + 24 + optsz

    def v2o(rva):
        for i in range(nsec):
            o = secoff + i * 40
            vs, va, rs, ro = struct.unpack_from('<IIII', data, o + 8)
            if va <= rva < va + max(vs, rs):
                return ro + rva - va
        return None

    built = 0
    if erva == 0:
        # ld did not synthesize an export directory (the .def is no longer
        # passed to the link).  Point the export DataDirectory at the
        # reserved NxkExportDirectory (gen-exports.py) and fill its header;
        # the function-RVA loop below populates the zeroed table.
        dirva = symva.get('_NxkExportDirectory')
        if dirva is None:
            sys.exit('fix-edata: no export directory and no '
                     '_NxkExportDirectory in the image')
        erva = dirva - base
        eo = v2o(erva)
        nf, = struct.unpack_from('<I', data, eo + 20)
        if nf != max(targets):
            sys.exit('fix-edata: reserved export table has %d slots but the '
                     'def\'s highest ordinal is %d -- regenerate xb-exports.c'
                     % (nf, max(targets)))
        struct.pack_into('<I', data, eo + 28, erva + 40)  # AddressOfFunctions
        struct.pack_into('<II', data, pe + 24 + 96, erva, 40 + 4 * nf)
        built = 1

    eo = v2o(erva)
    ordbase, = struct.unpack_from('<I', data, eo + 16)
    nf, = struct.unpack_from('<I', data, eo + 20)
    aof, = struct.unpack_from('<I', data, eo + 28)
    ao = v2o(aof)

    patched = missing = 0
    for i in range(nf):
        ordinal = i + ordbase
        sym = targets.get(ordinal)
        rva, = struct.unpack_from('<I', data, ao + i * 4)
        if sym is None:
            continue  # gap ordinal (scaffold-less), leave as-is
        va = symva.get(sym)
        if va is None:
            sys.stderr.write('fix-edata: ordinal %d: symbol %s missing '
                             'from image\n' % (ordinal, sym))
            missing += 1
            continue
        want = va - base
        if rva != want:
            struct.pack_into('<I', data, ao + i * 4, want)
            patched += 1

    if missing:
        sys.exit('fix-edata: %d export symbols missing -- the export '
                 'roots anchor is not doing its job' % missing)
    if patched or built:
        open(exe, 'wb').write(data)
    sys.stderr.write('fix-edata: %s%d/%d export RVAs %s\n'
                     % ('directory built, ' if built else '', patched, nf,
                        'patched' if patched else 'verified, all correct'))


if __name__ == '__main__':
    main()
