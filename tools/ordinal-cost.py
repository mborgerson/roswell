#!/usr/bin/env python3
"""
ordinal-cost.py: per-ordinal exclusive resident-.text cost of the export
surface.

For every exported ordinal, computes the EXCLUSIVE call-graph cone: the
set of resident .text functions reachable from that ordinal's entry but
from no other root (other ordinals, address-taken functions, the image
entry point).  This is the .text that would leave the image if the
ordinal reverted to a scaffold stub -- and the budget price of
implementing a new one.

The shared substrate (Io/Ob/Ke cores reached by many ordinals) shows up
in no ordinal's exclusive cone; the column answers "what does THIS
ordinal cost on the margin", not "what does the API surface cost".

Use when implementing a new ordinal: run before/after and record the
delta in the commit message (docs/footprint.md policy).

Usage:
    ordinal-cost.py [unstripped-exe] [--all]

Default prints ordinals with a non-zero exclusive cone, largest first.
--all includes zero-cost ordinals (entry shared or stub).

Env: NM / OBJDUMP override the i686-w64-mingw32 binutils.
"""

import os
import re
import struct
import subprocess
import sys
from bisect import bisect_right
from collections import deque

DEFAULT_EXE = "build/ntoskrnl/xboxkrnl.unstripped.exe"

args = sys.argv[1:]
show_all = "--all" in args
args = [a for a in args if a != "--all"]
exe = args[0] if args else DEFAULT_EXE
nm = os.environ.get("NM", "i686-w64-mingw32-nm")
objdump = os.environ.get("OBJDUMP", "i686-w64-mingw32-objdump")

data = open(exe, "rb").read()
e_lfanew = struct.unpack_from("<I", data, 0x3C)[0]
nsec, = struct.unpack_from("<H", data, e_lfanew + 6)
optsz, = struct.unpack_from("<H", data, e_lfanew + 20)
image_base, = struct.unpack_from("<I", data, e_lfanew + 24 + 28)
entry_rva, = struct.unpack_from("<I", data, e_lfanew + 24 + 16)
export_rva, _ = struct.unpack_from("<II", data, e_lfanew + 24 + 96)
secoff = e_lfanew + 24 + optsz
sections = []
for i in range(nsec):
    off = secoff + i * 40
    name = data[off:off + 8].rstrip(b"\0").decode()
    vsize, va_rva, rawsz, rawoff = struct.unpack_from("<IIII", data, off + 8)
    chars, = struct.unpack_from("<I", data, off + 36)
    sections.append((name, image_base + va_rva, max(vsize, rawsz), rawoff, chars))


def sec_of(va):
    for n, v, s, _, _ in sections:
        if v <= va < v + s:
            return n
    return "?"


def va_to_off(va):
    for n, v, s, ro, _ in sections:
        if v <= va < v + s:
            return ro + (va - v)
    return None


funcs = {}
for line in subprocess.check_output(
        [nm, "-S", "--size-sort", "--defined-only", exe],
        stderr=subprocess.DEVNULL).decode("latin-1").splitlines():
    p = line.split()
    if len(p) < 4 or p[2] not in ("t", "T"):
        continue
    try:
        a, s = int(p[0], 16), int(p[1], 16)
    except ValueError:
        continue
    n = p[3]
    if n in ("PAGE", "INIT", "PAGECONS", "PAGEDATA", ".text"):
        continue
    for pfx in (".text$", "INIT$"):
        if n.startswith(pfx):
            n = n[len(pfx):]
    if a in funcs and funcs[a][0] >= s:
        continue
    funcs[a] = (s, n)
starts = sorted(funcs)
fset = set(starts)


def func_at(va):
    i = bisect_right(starts, va) - 1
    if i >= 0 and va < starts[i] + funcs[starts[i]][0]:
        return starts[i]
    return None


# Direct-branch call graph + address-taken roots (same conservatism as
# init-audit.py: any address-taken function is a root).
edges = {a: set() for a in starts}
ataken = set()
dis = subprocess.run([objdump, "-d", "--no-show-raw-insn", exe],
                     capture_output=True).stdout.decode("latin-1", "replace")
insn_re = re.compile(r"^\s*([0-9a-f]+):\t(\w[\w ]*?)\s+(.*)$")
tgt_re = re.compile(r"^([0-9a-f]+)\b")
imm_re = re.compile(r"\$0x([0-9a-f]{7,8})")
for line in dis.splitlines():
    m = insn_re.match(line)
    if not m:
        continue
    iva = int(m.group(1), 16)
    fn = func_at(iva)
    if fn is None:
        continue
    mnem, ops = m.group(2).strip(), m.group(3)
    if mnem.startswith(("call", "jmp", "j", "loop")):
        t = tgt_re.match(ops)
        if t:
            tv = int(t.group(1), 16)
            tf = func_at(tv)
            if tf is not None and tf != fn:
                edges[fn].add(tf)
    for im in imm_re.finditer(ops):
        v = int(im.group(1), 16)
        if v in fset:
            ataken.add(v)
for n, v, s, ro, chars in sections:
    if chars & 0x20000000 or n.startswith(".debug"):
        continue
    blob = data[ro:ro + s]
    for o in range(0, len(blob) - 3, 4):
        w = struct.unpack_from("<I", blob, o)[0]
        if w in fset:
            ataken.add(w)

eoff = va_to_off(image_base + export_rva)
nfuncs, = struct.unpack_from("<I", data, eoff + 20)
aof, = struct.unpack_from("<I", data, eoff + 28)
aoff = va_to_off(image_base + aof)
ordroot = {}
for i in range(nfuncs):
    rva, = struct.unpack_from("<I", data, aoff + i * 4)
    if not rva:
        continue
    f = func_at(image_base + rva)
    if f:
        ordroot[i + 1] = f


def closure(roots):
    seen = set()
    q = deque(r for r in roots if r is not None)
    while q:
        f = q.popleft()
        if f in seen:
            continue
        seen.add(f)
        q.extend(edges.get(f, ()))
    return seen


other_roots = set(ataken) | {func_at(image_base + entry_rva)}
results = []
for o, f in ordroot.items():
    rest = other_roots | {v for k, v in ordroot.items() if k != o}
    excl = closure([f]) - closure(rest)
    b = sum(funcs[x][0] for x in excl if sec_of(x) == ".text")
    results.append((b, o, funcs[f][1], len(excl)))
results.sort(reverse=True)
tot = sum(b for b, _, _, _ in results)
print(f"{len([r for r in results if r[0]])} of {len(results)} ordinals have "
      f"exclusive .text cones; total exclusive {tot:,} B")
print(f"{'bytes':>7} {'ord':>4} {'fns':>4}  name")
for b, o, n, c in results:
    if not b and not show_all:
        continue
    print(f"{b:>7} {o:>4} {c:>4}  {n}")
