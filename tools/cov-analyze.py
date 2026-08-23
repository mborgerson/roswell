#!/usr/bin/env python3
"""
cov-analyze.py: cross-reference an xemu `-d in_asm,nochain` trace log with
the kernel's symbol table to identify which functions are/aren't reached
during boot.

Usage:
    cov-analyze.py [cov-log...] [unstripped-exe]         # dead-code mode
    cov-analyze.py --init [cov-log...] [unstripped-exe]  # INIT-candidate mode

Multiple cov logs union: the title boundary is detected per log, and a
function is an INIT candidate only if no log hit it post-boot.

INIT mode splits the trace at the first title-space TB (low VA, e.g.
0x11000) and reports kernel functions hit BEFORE the boundary but NOT
after -- safe candidates for `#pragma alloc_text(INIT, ...)`.
"""

import os
import re
import subprocess
import sys
TOP = int(os.environ.get("COV_TOP", "30"))
from bisect import bisect_right

DEFAULT_LOG = "/tmp/cov.log"
DEFAULT_EXE = "build/ntoskrnl/xboxkrnl.unstripped.exe"

args = sys.argv[1:]
init_mode = False
if args and args[0] == "--init":
    init_mode = True
    args = args[1:]
# Any arg ending in .exe is the kernel binary; everything else is a cov
# log.  Multiple logs are unioned: a function counts as post-boot-hit if
# ANY trace hit it after its title boundary, so candidates survive only
# when every workload agrees they are boot-only.
cov_logs = [a for a in args if not a.endswith(".exe")] or [DEFAULT_LOG]
exes = [a for a in args if a.endswith(".exe")]
exe = exes[0] if exes else DEFAULT_EXE
nm = os.environ.get("NM", "i686-w64-mingw32-nm")

# Title VA window. nxdk XBEs link at 0x11000; .text+.rdata+.data+.tls
# rarely exceeds 0x100000.  nxldr (boot loader) lives at 0x100000+, so
# we exclude that range from the "title started" signal.
TITLE_VA_LOW  = 0x00010000
TITLE_VA_HIGH = 0x00100000

# 1. Parse the cov log: extract every TB start address. In INIT mode,
# also record which side of the boot/title boundary each kernel TB
# falls on. The boundary is the first time we see a TB whose address
# is in the title VA space.
addr_re = re.compile(r"^0x([0-9a-fA-F]+):")
tb_addrs = set()         # dead-code mode: union of all addresses
boot_addrs = set()       # INIT mode: kernel addresses before boundary
post_addrs = set()       # INIT mode: kernel addresses after boundary
for cov_log in cov_logs:
    print(f"reading cov log {cov_log}...", file=sys.stderr)
    saw_title = False    # boundary is per-trace
    with open(cov_log) as f:
        for line in f:
            m = addr_re.match(line)
            if not m:
                continue
            addr = int(m.group(1), 16)
            tb_addrs.add(addr)
            if not init_mode:
                continue
            if TITLE_VA_LOW <= addr < TITLE_VA_HIGH:
                saw_title = True
                continue
            # Kernel TB (or nxldr/bootrom -- we only care about kernel)
            if addr < 0x80000000:
                continue
            if saw_title:
                post_addrs.add(addr)
            else:
                boot_addrs.add(addr)
print(f"  {len(tb_addrs)} unique TB starts", file=sys.stderr)
if init_mode:
    print(f"  boot phase: {len(boot_addrs)} kernel TBs",
          file=sys.stderr)
    print(f"  post-boot:  {len(post_addrs)} kernel TBs",
          file=sys.stderr)

# 2. Read the kernel's symbol table.  LTO images carry plain `_Foo@N` /
# `Foo.lto_priv.N` symbols rather than the pre-LTO `.text$X` function-
# section aliases, so accept any sized t/T symbol (same logic as
# tools/init-audit.py).
print(f"reading symbols from {exe}...", file=sys.stderr)
out = subprocess.check_output(
    [nm, "-S", "--size-sort", "--defined-only", exe]).decode().splitlines()
funcmap = {}  # addr -> (size, name)
for line in out:
    parts = line.split()
    if len(parts) < 4:
        continue
    try:
        addr = int(parts[0], 16)
        size = int(parts[1], 16)
    except ValueError:
        continue
    kind = parts[2]
    name = parts[3]
    if kind not in ("t", "T"):
        continue
    if size == 0:
        continue
    if name in ("PAGE", "INIT", "PAGECONS", "PAGEDATA", ".text"):
        continue
    for pfx in (".text$", "INIT$"):
        if name.startswith(pfx):
            name = name[len(pfx):]
    if name.startswith("PAGE") and "$" in name:
        name = name.split("$", 1)[1]
    if addr in funcmap and funcmap[addr][0] >= size:
        continue
    funcmap[addr] = (size, name)
functions = [(a, s, n) for a, (s, n) in funcmap.items()]

print(f"  {len(functions)} kernel functions", file=sys.stderr)

# 3. For each function, mark hit/unhit.
# Build sorted address list for fast bisect.
functions.sort()
starts = [f[0] for f in functions]


def mark_hits(addr_set):
    """Return per-function `hit` boolean array for the given TB set."""
    h = [False] * len(functions)
    for tb in addr_set:
        idx = bisect_right(starts, tb) - 1
        if idx < 0:
            continue
        a, s, _ = functions[idx]
        if tb < a + s:
            h[idx] = True
    return h


if init_mode:
    boot_hit = mark_hits(boot_addrs)
    post_hit = mark_hits(post_addrs)

    # A function "hit only at boot" is only worth moving to INIT if it is
    # currently RESIDENT -- functions already in a discardable section
    # (INIT et al) are freed after Phase 1 already, so reporting them is
    # noise.  Read the section table (DISCARDABLE flag from the shipped
    # sibling, where pefixup re-applies it post-strip) and filter.
    def _resident_filter(path):
        shipped = os.path.join(os.path.dirname(path), "xboxkrnl.exe")
        src = shipped if os.path.exists(shipped) else path
        d = open(src, "rb").read()
        e = int.from_bytes(d[0x3c:0x40], "little")
        nsec = int.from_bytes(d[e+6:e+8], "little")
        optsz = int.from_bytes(d[e+20:e+22], "little")
        base = int.from_bytes(d[e+24+28:e+24+32], "little")
        secoff = e + 24 + optsz
        disc = []   # (rva_start, rva_end) of discardable sections
        for i in range(nsec):
            off = secoff + i * 40
            vsize = int.from_bytes(d[off+8:off+12], "little")
            vaddr = int.from_bytes(d[off+12:off+16], "little")
            chars = int.from_bytes(d[off+36:off+40], "little")
            if chars & 0x02000000:  # IMAGE_SCN_MEM_DISCARDABLE
                disc.append((vaddr, vaddr + max(vsize, 1)))
        def resident(addr):
            rva = addr - base
            return not any(s <= rva < e2 for s, e2 in disc)
        return resident
    is_resident = _resident_filter(exe)

    all_boot_only = [(functions[i][0], functions[i][1], functions[i][2])
                     for i in range(len(functions))
                     if boot_hit[i] and not post_hit[i]]
    candidates = [(sz, name) for addr, sz, name in all_boot_only
                  if is_resident(addr)]
    already_init = [(sz, name) for addr, sz, name in all_boot_only
                    if not is_resident(addr)]
    candidates.sort(reverse=True)

    total_init = sum(sz for sz, _ in candidates)
    skipped = sum(sz for sz, _ in already_init)
    print(f"\nINIT-candidate functions (RESIDENT, boot-only): "
          f"{len(candidates)} totalling {total_init} bytes "
          f"({total_init/1024.0:.1f} KB)")
    print(f"  (excluded {len(already_init)} boot-only funcs / {skipped} bytes "
          f"already in a discardable section)")
    print(f"\nTop 60 LARGEST INIT candidates "
          f"(hit during boot, NOT during steady-state):")
    for sz, name in candidates[:60]:
        print(f"  {sz:>6}  {name}")

    print(f"\nINIT-candidate by prefix bucket:")
    buckets = {}
    for sz, name in candidates:
        bare = name.lstrip("_@")
        m = re.match(r"([A-Z][a-z]+|[A-Z]+)", bare)
        prefix = m.group(1) if m else "(other)"
        buckets[prefix] = buckets.get(prefix, 0) + sz
    for prefix, sz in sorted(buckets.items(), key=lambda x: -x[1])[:25]:
        print(f"  {sz:>6}  {prefix}*")
    sys.exit(0)


# Dead-code mode (original behavior)
hit = mark_hits(tb_addrs)
hit_count = sum(1 for x in hit if x)

# 4. Report.
total_text = sum(s for _, s, _ in functions)
hit_text = sum(functions[i][1] for i in range(len(functions)) if hit[i])
unhit_text = total_text - hit_text

print(f"\nCoverage:")
print(f"  functions:  {hit_count:>5} / {len(functions):>5}"
      f"  ({100.0*hit_count/len(functions):.1f}%)")
print(f"  .text:      {hit_text:>5} / {total_text:>5} bytes"
      f"  ({100.0*hit_text/total_text:.1f}%)")
print(f"  unreached:  {unhit_text:>5} bytes ({unhit_text/1024.0:.1f} KB)")

# 5. Top unreached functions (biggest first).
print(f"\nTop " f"{TOP}" f" LARGEST unreached functions (cut candidates):")
unreached = [(functions[i][1], functions[i][2])
             for i in range(len(functions)) if not hit[i]]
unreached.sort(reverse=True)
for sz, name in unreached[:TOP]:
    print(f"  {sz:>6}  {name}")

# 6. Aggregate by name prefix to see which subsystems dominate the
# unreached set.
print(f"\nUnreached by prefix bucket:")
buckets = {}
for sz, name in unreached:
    # Strip leading underscore + take the cluster prefix
    bare = name.lstrip("_@")
    # Take the leading uppercase-letter run as the cluster
    m = re.match(r"([A-Z][a-z]+|[A-Z]+)", bare)
    prefix = m.group(1) if m else "(other)"
    buckets[prefix] = buckets.get(prefix, 0) + sz
for prefix, sz in sorted(buckets.items(), key=lambda x: -x[1])[:20]:
    print(f"  {sz:>6}  {prefix}*")
