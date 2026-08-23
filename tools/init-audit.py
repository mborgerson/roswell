#!/usr/bin/env python3
"""
init-audit.py: static INIT-migration / dead-code audit of xboxkrnl.exe.

Builds a conservative call graph over the linked kernel and classifies
every function:

  runtime   reachable from a runtime root (export directory entry, or
            address-taken anywhere outside INIT/INITDATA -- dispatch
            tables, DPC/ISR/thread-entry pointers, IDT writes)
  init-only reachable only from the init domain (image entry point +
            functions already in INIT).  Safe CODE_SEG("INIT")
            candidates: the pages vanish after MiFreeInitializationCode.
  dead      reachable from neither -- candidates for SARCH gating (the
            linker keeps them, so something references them; check why).

Conservatism: any `call *r/m` can hit any address-taken function, so
EVERY address-taken function is a runtime root -- even when the only
reference is in INIT code, because init code plants pointers into
long-lived structures (object-type procedures, IRP dispatch tables,
DPC/ISR records) that are invoked long after INIT is freed.  False
address-takens (random data matching a function VA) only hide
candidates -- they never produce an unsafe one.

Usage:
    init-audit.py [unstripped-exe] [--cov LOG...]
    init-audit.py --check [unstripped-exe] [--allow FILE]

With --cov, each candidate is annotated against the dynamic trace split
(see cov-analyze.py): `hot-post` marks functions some trace executed after
its title boundary -- a static/dynamic disagreement worth a second look.

--check is the post-link migration gate (wired into the kernel build):
it fails (exit 1) if any resident section still references a
discardable one (INIT / INITDATA / .rsrc) -- a direct branch, a code
operand holding a discardable address, an aligned data pointer, or an
export RVA.  Such a reference crashes after MiFreeInitializationCode
unmaps the pages, and LTO can create one silently by placing an
inlined clone differently from its origin.  Resident functions that
legitimately reference INIT (the init orchestrators -- everything they
touch runs before the discard) are allowlisted in tools/init-ref.allow
with per-entry justifications.

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
cov_logs = []
check_mode = False
allow_file = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                          "init-ref.allow")
if "--check" in args:
    check_mode = True
    args.remove("--check")
if "--allow" in args:
    i = args.index("--allow")
    allow_file = args[i + 1]
    del args[i:i + 2]
if "--cov" in args:
    i = args.index("--cov")
    cov_logs = args[i + 1:]
    args = args[:i]
exe = args[0] if args else DEFAULT_EXE
nm = os.environ.get("NM", "i686-w64-mingw32-nm")
objdump = os.environ.get("OBJDUMP", "i686-w64-mingw32-objdump")

# --- 1. PE geometry: sections, entry point, export directory ----------------
data = open(exe, "rb").read()
e_lfanew = struct.unpack_from("<I", data, 0x3C)[0]
nsec, = struct.unpack_from("<H", data, e_lfanew + 6)
optsz, = struct.unpack_from("<H", data, e_lfanew + 20)
image_base, = struct.unpack_from("<I", data, e_lfanew + 24 + 28)
entry_rva, = struct.unpack_from("<I", data, e_lfanew + 24 + 16)
export_rva, export_sz = struct.unpack_from("<II", data, e_lfanew + 24 + 96)
secoff = e_lfanew + 24 + optsz

sections = []  # (name, va, vsize, rawoff, rawsz, chars)
for i in range(nsec):
    off = secoff + i * 40
    name = data[off:off + 8].rstrip(b"\0").decode("latin-1")
    vsize, va_rva, rawsz, rawoff = struct.unpack_from("<IIII", data, off + 8)
    chars, = struct.unpack_from("<I", data, off + 36)
    sections.append((name, image_base + va_rva, vsize, rawoff, rawsz, chars))

def section_of(va):
    for s in sections:
        if s[1] <= va < s[1] + max(s[2], s[4]):
            return s
    return None

def va_to_off(va):
    s = section_of(va)
    if not s:
        return None
    delta = va - s[1]
    if delta >= s[4]:
        return None  # in zero-fill tail
    return s[3] + delta

INIT_SECTIONS = {"INIT", "INITDATA"}

# --- 2. Function table from nm ----------------------------------------------
out = subprocess.check_output(
    [nm, "-S", "--size-sort", "--defined-only", exe],
    stderr=subprocess.DEVNULL).decode("latin-1").splitlines()
functions = {}  # start va -> (size, name)
for line in out:
    parts = line.split()
    if len(parts) < 4:
        continue
    addr, size, kind, name = int(parts[0], 16), int(parts[1], 16), parts[2], parts[3]
    if kind not in ("t", "T"):
        continue
    if name in ("PAGE", "INIT", "PAGECONS", "PAGEDATA", ".text") or \
       name.startswith((".text", "PAGE$", "INIT$")) and "$" not in name:
        continue
    if name.startswith(".text$"):
        name = name[len(".text$"):]
    elif name.startswith("INIT$"):
        name = name[len("INIT$"):]
    elif name.startswith("PAGE"):
        if "$" in name:
            name = name.split("$", 1)[1]
    # Prefer the section-alias entries (one per function); plain symbol
    # entries at the same address just refine the name.
    if addr in functions and functions[addr][0] >= size:
        continue
    functions[addr] = (size, name)
# Gap-fill: code regions nm has no sized symbol for (asm stubs, some
# INIT bodies) still call and reference things; orphaning them breaks
# the closure.  Cover every executable-section gap with a pseudo entry
# so edges and address-takens from there are attributed to the right
# domain.
known = sorted(functions)
for name, va, vsize, rawoff, rawsz, chars in sections:
    if not chars & 0x20000000:
        continue
    sec_funcs = [a for a in known if va <= a < va + vsize]
    cursor = va
    for a in sec_funcs + [va + vsize]:
        if a - cursor >= 4:
            functions[cursor] = (a - cursor, f"anon@{cursor:x}({name})")
        if a < va + vsize:
            cursor = max(cursor, a + functions[a][0])
starts = sorted(functions)
print(f"{len(functions)} functions (incl. gap fills)", file=sys.stderr)

def func_at(va):
    idx = bisect_right(starts, va) - 1
    if idx < 0:
        return None
    a = starts[idx]
    if va < a + functions[a][0]:
        return a
    return None

# --- 2b. --check: resident->discardable reference gate -----------------------
# Discardable = unmapped + freed after MiFreeInitializationCode.  .rsrc is
# included by name: its DISCARDABLE flag is only stamped by native-pefixup
# on the *stripped* image, after this unstripped copy is made.
DISCARD_NAMES = {"INIT", "INITDATA", ".rsrc"}

def is_discardable(s):
    return s[0] in DISCARD_NAMES or bool(s[5] & 0x02000000)

def norm_name(n):
    n = n.lstrip("_@")
    n = n.split("@")[0]
    for tag in (".lto_priv", ".constprop", ".isra", ".part", ".cold"):
        i = n.find(tag)
        if i > 0:
            n = n[:i]
    return n

if check_mode:
    discard_ranges = [(s[1], s[1] + max(s[2], s[4]), s[0])
                      for s in sections if is_discardable(s)]

    def in_discard(va):
        for lo, hi, name in discard_ranges:
            if lo <= va < hi:
                return name
        return None

    # Full symbol list (all kinds) for attributing data hits.
    all_syms = []  # (va, name)
    out = subprocess.check_output(
        [nm, "--defined-only", exe],
        stderr=subprocess.DEVNULL).decode("latin-1").splitlines()
    for line in out:
        parts = line.split()
        if len(parts) != 3:
            continue
        all_syms.append((int(parts[0], 16), parts[2]))
    all_syms.sort()
    sym_addrs = [a for a, _ in all_syms]
    # Entry addresses of discardable symbols -- the only values an operand
    # literal may legitimately hold to count as a real address-take.
    discard_entries = {a for a, _ in all_syms if in_discard(a)}

    def sym_at(va):
        idx = bisect_right(sym_addrs, va) - 1
        if idx < 0:
            return f"?@{va:08x}"
        a, n = all_syms[idx]
        return f"{n}+0x{va - a:x}" if va != a else n

    allow = set()
    if os.path.exists(allow_file):
        for line in open(allow_file):
            line = line.split("#", 1)[0].strip()
            if line:
                allow.add(line)

    violations = []  # (kind, source-desc, source-norm-name, target-desc)

    # 1. Export RVAs must never land in a discardable section (a title can
    #    call any ordinal at any time).  Never allowlistable.
    fatal_exports = []
    if export_rva:
        eoff = va_to_off(image_base + export_rva)
        nfuncs, = struct.unpack_from("<I", data, eoff + 20)
        aof_rva, = struct.unpack_from("<I", data, eoff + 28)
        aoff = va_to_off(image_base + aof_rva)
        for i in range(nfuncs):
            rva, = struct.unpack_from("<I", data, aoff + i * 4)
            if rva and in_discard(image_base + rva):
                fatal_exports.append(
                    f"ordinal {i + 1} -> {sym_at(image_base + rva)}")

    # 2. Code references: scan every insn in resident exec sections for
    #    branch targets / operand literals inside a discardable range.
    dis = subprocess.run(
        [objdump, "-d", "--no-show-raw-insn", exe],
        capture_output=True).stdout.decode("latin-1", errors="replace")
    insn_re = re.compile(r"^\s*([0-9a-f]+):\t(\w[\w ]*?)\s+(.*)$")
    target_re = re.compile(r"^([0-9a-f]+)\b")
    lit_re = re.compile(r"0x([0-9a-f]{7,8})\b")
    for line in dis.splitlines():
        m = insn_re.match(line)
        if not m:
            continue
        insn_va = int(m.group(1), 16)
        src_sec = section_of(insn_va)
        if src_sec is None or is_discardable(src_sec):
            continue
        mnem, ops = m.group(2).strip(), m.group(3)
        targets = []
        if mnem.startswith(("call", "jmp", "j", "loop")):
            tm = target_re.match(ops)
            if tm:
                targets.append(int(tm.group(1), 16))
        # Operand literals count only when they hit a discardable symbol's
        # entry.  A literal landing in a function interior is a coincidental
        # constant -- e.g. an NTSTATUS/HRESULT (0x8007xxxx) overlapping the
        # kernel's own low VA range at the retail image base, not a pointer.
        targets += [v for v in (int(l, 16) for l in lit_re.findall(ops))
                    if v in discard_entries]
        for tgt in targets:
            dsec = in_discard(tgt)
            if not dsec:
                continue
            fn = func_at(insn_va)
            src = functions[fn][1] if fn is not None else f"?@{insn_va:08x}"
            violations.append(("code", f"{src} @{insn_va:08x}",
                               norm_name(src), f"{sym_at(tgt)} ({dsec})"))

    # 3. Data references: aligned dwords in resident data sections whose
    #    value lands in a discardable range (function pointers / data
    #    pointers planted in long-lived structures).  Debug sections are
    #    metadata, not loaded state -- skip.
    for s in sections:
        name, va, vsize, rawoff, rawsz, chars = s
        if chars & 0x20000000 or is_discardable(s):
            continue
        if name.startswith((".debug", ".edata", ".idata")):
            continue
        blob = data[rawoff:rawoff + min(vsize, rawsz)]
        for off in range(0, len(blob) - 3, 4):
            v = struct.unpack_from("<I", blob, off)[0]
            dsec = in_discard(v)
            if not dsec:
                continue
            # Switch jump tables live in .rdata but their entries point
            # at basic blocks *inside* the owning function -- when that
            # function is in INIT, the table's only reader dies with the
            # section.  A planted function pointer targets a function
            # START; skip interior-of-INIT-function targets.
            tf = func_at(v)
            if tf is not None and v != tf:
                continue
            src = sym_at(va + off)
            violations.append(("data", f"{src} ({name})",
                               norm_name(src.split("+")[0]),
                               f"{sym_at(v)} ({dsec})"))

    flagged = [v for v in violations if v[2] not in allow]
    allowed = [v for v in violations if v[2] in allow]

    if allowed:
        print(f"init-ref-check: {len(allowed)} allowlisted "
              f"resident->discardable refs", file=sys.stderr)
    if fatal_exports:
        print("init-ref-check: FATAL -- export RVAs in discardable "
              "sections (titles can call these any time):")
        for e in fatal_exports:
            print(f"  {e}")
    if flagged:
        print(f"init-ref-check: {len(flagged)} resident->discardable "
              f"reference(s) outside {os.path.basename(allow_file)}:")
        for kind, src, _, tgt in flagged:
            print(f"  [{kind}] {src} -> {tgt}")
        print("Each is a use-after-free once MiFreeInitializationCode "
              "runs.\nFix the migration, or -- only if the source "
              "provably runs before the\ndiscard -- add its symbol to "
              f"{allow_file} with a justification.")
    if fatal_exports or flagged:
        sys.exit(1)
    print(f"init-ref-check: OK ({len(allowed)} allowlisted refs, "
          f"exports clean)", file=sys.stderr)
    sys.exit(0)

# --- 3. Direct-call edges from objdump --------------------------------------
# objdump syncs at symbol boundaries, so per-function disassembly is
# reliable here (unlike raw byte scans).  One pass over the whole image.
edges = {a: set() for a in starts}
call_re = re.compile(
    r"^\s*([0-9a-f]+):\s+(?:[0-9a-f]{2} )+\s*\t(call|jmp|je|jne|jb|jbe|ja|"
    r"jae|jg|jge|jl|jle|js|jns|jo|jc|jnc|jz|jnz|jcxz|loop\w*)\s+(?:\*?)"
    r"0x?([0-9a-f]+)\b")
dis = subprocess.run(
    [objdump, "-d", "--no-show-raw-insn", exe],
    capture_output=True).stdout.decode("latin-1", errors="replace")
cur = None
insn_re = re.compile(r"^\s*([0-9a-f]+):\t(\w[\w ]*?)\s+(.*)$")
target_re = re.compile(r"^([0-9a-f]+)\b")
imm_re = re.compile(r"\$0x([0-9a-f]+)")
address_taken = set()   # every address-taken function start
for line in dis.splitlines():
    if line.endswith(">:") and " <" in line:
        try:
            cur = int(line.split(" ", 1)[0], 16)
        except ValueError:
            cur = None
        continue
    m = insn_re.match(line)
    if not m or cur is None:
        continue
    insn_va, mnem, ops = int(m.group(1), 16), m.group(2).strip(), m.group(3)
    fn = func_at(insn_va)
    if fn is None:
        continue
    if mnem.startswith(("call", "jmp", "j", "loop")):
        tm = target_re.match(ops)
        if tm:
            tgt = int(tm.group(1), 16)
            tf = func_at(tgt)
            if tf is not None and tf != fn:
                edges[fn].add(tf)
    # immediates that look like function addresses = address-taken
    for im in imm_re.finditer(ops):
        v = int(im.group(1), 16)
        tf = func_at(v)
        if tf is not None and v == tf:  # only exact function starts
            address_taken.add(tf)

# --- 4. Address-taken scan over data sections -------------------------------
fset = set(starts)
for name, va, vsize, rawoff, rawsz, chars in sections:
    if chars & 0x20000000:  # IMAGE_SCN_MEM_EXECUTE: code handled above
        continue
    blob = data[rawoff:rawoff + min(vsize, rawsz)]
    for off in range(0, len(blob) - 3):
        v = struct.unpack_from("<I", blob, off)[0]
        if v in fset:
            address_taken.add(v)

# --- 5. Runtime / init root sets ---------------------------------------------
runtime_roots = set(address_taken)
# Export directory: every exported ordinal is title-callable forever.
if export_rva:
    eoff = va_to_off(image_base + export_rva)
    nfuncs, = struct.unpack_from("<I", data, eoff + 20)
    aof_rva, = struct.unpack_from("<I", data, eoff + 28)
    aoff = va_to_off(image_base + aof_rva)
    for i in range(nfuncs):
        rva, = struct.unpack_from("<I", data, aoff + i * 4)
        if not rva:
            continue
        f = func_at(image_base + rva)
        if f:
            runtime_roots.add(f)

entry_fn = func_at(image_base + entry_rva)
init_domain_roots = {entry_fn} if entry_fn else set()
init_domain_roots |= {a for a in starts
                      if (section_of(a) or ("",))[0] in INIT_SECTIONS}

def closure(roots, stop=None):
    seen = set()
    q = deque(r for r in roots if r is not None)
    while q:
        f = q.popleft()
        if f in seen:
            continue
        seen.add(f)
        for t in edges.get(f, ()):
            if t not in seen:
                q.append(t)
    return seen

runtime_reach = closure(runtime_roots)
init_reach = closure(init_domain_roots)

# --- 6. Optional dynamic annotation ------------------------------------------
post_hit = set()
if cov_logs:
    addr_re = re.compile(r"^0x([0-9a-fA-F]+):")
    for log in cov_logs:
        saw_title = False
        with open(log) as f:
            for line in f:
                m = addr_re.match(line)
                if not m:
                    continue
                a = int(m.group(1), 16)
                if 0x10000 <= a < 0x100000:
                    saw_title = True
                    continue
                if a < 0x80000000 or not saw_title:
                    continue
                fn = func_at(a)
                if fn:
                    post_hit.add(fn)

# --- 7. Report ----------------------------------------------------------------
# Init-reachable is necessary but not sufficient: the function must
# also RETURN before Phase1InitializationDiscard frees INIT.  Functions
# entered during init that never exit stay resident forever.
RUNS_FOREVER = {"KiIdleLoop"}

candidates = []   # init-only, not yet in INIT
dead = []
for a in starts:
    size, name = functions[a]
    sec = (section_of(a) or ("?",))[0]
    if a in runtime_reach:
        continue
    if a in init_reach:
        if sec not in INIT_SECTIONS and \
           name.lstrip("_@").split("@")[0] not in RUNS_FOREVER:
            candidates.append((size, name, a))
    else:
        dead.append((size, name, a, sec))

candidates.sort(reverse=True)
dead.sort(reverse=True)

tot = sum(s for s, _, _ in candidates)
print(f"\nINIT-migration candidates (statically init-only, outside INIT;"
      f"\nstill verify each returns before Phase 1 ends and is not"
      f" DBG-live): {len(candidates)} fns, {tot:,} bytes"
      f" ({tot/1024.0:.1f} KB)")
for size, name, a in candidates:
    tag = "  [hot-post!]" if a in post_hit else ""
    print(f"  {size:>6}  {name}{tag}")

tot = sum(s for s, _, _, _ in dead)
print(f"\nUnreachable from any root (gating candidates -- verify why "
      f"linked): {len(dead)} fns, {tot:,} bytes ({tot/1024.0:.1f} KB)")
for size, name, a, sec in dead[:80]:
    tag = "  [hot-post!]" if a in post_hit else ""
    print(f"  {size:>6}  {name} ({sec}){tag}")
