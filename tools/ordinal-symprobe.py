#!/usr/bin/env python3
"""List unmapped kernel ordinals whose symbol is already in the image.

An ordinal in xboxkrnl.exe.def that ordinals.map does not name resolves
to a bugcheck stub.  Some of those need real work; some need only a map
line, because the routine is already compiled with a decoration that
matches the export byte for byte.  This tells the two apart: it reports
every unmapped ordinal whose exact decorated name is present in the
linked kernel.

A hit is a candidate, not a verdict -- signatures can agree while the
structures they carry do not, so confirm behavior on the retail kernel
with `tools/api-regression-run --official` before mapping.  A miss is
not a verdict either: a routine that nothing else references is
collected away, and adding the map line would re-root it.

Usage: tools/ordinal-symprobe.py [build-dir]   (default: build)
"""

import os
import re
import subprocess
import sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
NM = os.environ.get("NM", "i686-w64-mingw32-nm")


def mapped_ordinals(path):
    out = set()
    with open(path) as f:
        for line in f:
            line = line.split("#")[0].strip()
            m = re.match(r"^(\d+)\s+\S", line)
            if m:
                out.add(int(m.group(1)))
    return out


def exported_ordinals(path):
    out = {}
    with open(path) as f:
        for line in f:
            m = re.match(r"\s*(\S+)\s+@\s*(\d+)\s+NONAME(\s+DATA)?", line)
            if m:
                out[int(m.group(2))] = (m.group(1), bool(m.group(3)))
    return out


def image_symbols(path):
    try:
        out = subprocess.run([NM, path], capture_output=True, text=True,
                             check=True).stdout
    except FileNotFoundError:
        sys.exit("ordinal-symprobe: %s not found (set NM=)" % NM)
    except subprocess.CalledProcessError as e:
        sys.exit("ordinal-symprobe: %s failed: %s" % (NM, e))
    syms = set()
    for line in out.splitlines():
        parts = line.split()
        if len(parts) == 3 and parts[1] in "TtDdBbRr":
            syms.add(parts[2])
    return syms


def main():
    build = sys.argv[1] if len(sys.argv) > 1 else os.path.join(REPO, "build")
    image = os.path.join(build, "ntoskrnl", "xboxkrnl.unstripped.exe")
    if not os.path.exists(image):
        sys.exit("ordinal-symprobe: %s missing -- build the kernel first"
                 % image)

    xb = os.path.join(REPO, "ntoskrnl", "xb")
    mapped = mapped_ordinals(os.path.join(xb, "ordinals.map"))
    exports = exported_ordinals(os.path.join(xb, "xboxkrnl.exe.def"))
    syms = image_symbols(image)

    hits = 0
    for ordinal in sorted(exports):
        if ordinal in mapped:
            continue
        name, is_data = exports[ordinal]
        # stdcall/cdecl exports carry a leading underscore; fastcall
        # exports are already spelled with the leading '@'.
        candidates = [name] if name.startswith("@") else ["_" + name, name]
        found = next((c for c in candidates if c in syms), None)
        if found:
            hits += 1
            print("%4d %s %-42s -> %s"
                  % (ordinal, "DATA" if is_data else "    ", name, found))

    unmapped = sum(1 for o in exports if o not in mapped)
    print("\n%d of %d unmapped ordinals already have a matching symbol."
          % (hits, unmapped), file=sys.stderr)


if __name__ == "__main__":
    main()
