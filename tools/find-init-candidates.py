#!/usr/bin/env python3
#
# find-init-candidates.py -- resident .text functions that *could* be tagged
# CODE_SEG("INIT") but aren't: only ever reached from INIT, so they're
# boot-only yet sit in the resident image.
#
# usage: tools/find-init-candidates.py [PATH/TO/xboxkrnl.unstripped.exe]
#
# This is the "misplaced boot-only function" finder (the resource-walker /
# ExCreateHandleTable pattern).  The hard part is SAFETY -- a naive
# "all callers are INIT" filter also flags things that would corrupt at
# runtime, so every candidate must pass ALL of:
#
#   1. resident .text (not already INIT/PAGE), and not exported (ABI root)
#   2. RETURNS NORMALLY -- contains a `ret`.  Excludes KiIdleLoop and the
#      noreturn bugcheck/idle functions that are "entered" from INIT but
#      never come back (moving them = exec in freed memory).
#   3. ADDRESS NEVER TAKEN -- its address appears as neither an immediate
#      operand in .text nor a word in any data/INIT section.  Excludes
#      dispatch/callback routines registered into vtables or (via an INIT
#      `mov $fn`) into runtime-pool DriverObjects -- those are called by
#      pointer at runtime and init-ref-check CANNOT catch the move.
#   4. has >=1 direct caller, and EVERY call/jmp reference is from INIT.
#
# Output is for human review, largest first.  Moving one is then:
#   #ifdef SARCH_XBOX \n CODE_SEG("INIT") \n #endif  before the definition,
#   and the build's init-ref-check verifies no resident reference remains.

import os
import re
import subprocess
import sys

_argv = [a for a in sys.argv[1:] if not a.startswith("--")]
NAMES_ONLY = "--names" in sys.argv   # print just candidate symbol names, one per line
CLOSURE = "--closure" in sys.argv    # transitive set (a move makes callees init-only too)
EXE = _argv[0] if _argv else "build/ntoskrnl/xboxkrnl.unstripped.exe"
NM = os.environ.get("NM", "i686-w64-mingw32-nm")
OBJDUMP = os.environ.get("OBJDUMP", "i686-w64-mingw32-objdump")
DEF = os.environ.get("DEF", "build/ntoskrnl/xboxkrnl.def")
# Optional: restrict candidates to functions the linker can physically relocate
# (those with a per-function .text$ section).  The lto-init-relink.py launcher
# passes this so the --closure set never selects a function on the assumption
# that an UNmovable caller will reach INIT.  Absent -> no restriction.
MOVABLE = None
_movfile = os.environ.get("MOVABLE_FILE")
if _movfile:
    try:
        MOVABLE = set(open(_movfile).read().split())
    except OSError:
        pass

# Force-resident denylist: functions that must NEVER be discarded, even though
# they look statically init-only.  These are runtime-installed entry points --
# IRP dispatch routines, AddDevice, etc. -- whose pointers are written into
# long-lived resident DRIVER_OBJECT/DEVICE_OBJECTs at boot and can be invoked
# after MiFreeInitializationCode.  Static analysis can't see the runtime store,
# so the safety call is explicit here.  Denying a function also keeps its
# callees resident: the closure only moves a callee when every caller is INIT
# or chosen, and a denied caller is never chosen.  Entries are base names
# (no leading _/@, no @N / .constprop / .lto_priv suffix).  Source:
# tools/init-force-resident.list, overridable via DENY_FILE.
DENY = set()
_denyfile = os.environ.get("DENY_FILE") or os.path.join(
    os.path.dirname(os.path.abspath(__file__)), "init-force-resident.list")
try:
    for _ln in open(_denyfile):
        _ln = _ln.split('#', 1)[0].strip()
        if _ln:
            DENY.add(_ln)
except OSError:
    pass

def run(*a):
    return subprocess.run(a, capture_output=True, text=True).stdout

# --- sections ---
secs = []
for line in run(OBJDUMP, "-h", EXE).splitlines():
    p = line.split()
    if len(p) >= 7 and (p[1].startswith('.') or p[1] in ('INIT', 'PAGE')):
        try:
            secs.append((p[1], int(p[3], 16), int(p[2], 16), int(p[5], 16)))
        except ValueError:
            pass

def sec_of(va):
    for n, b, s, _ in secs:
        if b <= va < b + s:
            return n
    return '?'

data = open(EXE, "rb").read()

# --- functions + adjacency sizes ---
funcs = []
for l in run(NM, "-n", EXE).splitlines():
    p = l.split()
    if len(p) == 3 and p[1].lower() == 't':
        funcs.append((int(p[0], 16), p[2]))
funcs.sort()
faddr = {n: a for a, n in funcs}
size = {}
for i, (a, n) in enumerate(funcs):
    size[n] = (funcs[i + 1][0] - a) if i + 1 < len(funcs) else 0

# --- exports (ABI roots) ---
exp = set()
try:
    deftext = open(DEF).read()
    for m in re.finditer(r'(\w+)(?:=[\w@]+)?\s+@\s*\d+', deftext):
        exp.add(m.group(1))
except OSError:
    print("warning: no .def at %s -- export filter disabled" % DEF, file=sys.stderr)

# --- disassembly: callers, ret-presence, address-as-immediate ---
callers = {}        # target name -> set(caller name)
has_ret = set()     # functions containing a `ret`
imm_addrs = set()   # values appearing as $0x.. immediate operands
hdr = re.compile(r'^[0-9a-f]+ <(.+)>:')
ref = re.compile(r'\b(?:call|jmp)\s+[0-9a-f]+ <([^>+]+)>')
imm = re.compile(r'\$0x([0-9a-f]+)')
cur = None
for line in run(OBJDUMP, "-d", EXE).splitlines():
    m = hdr.match(line)
    if m:
        cur = m.group(1)
        continue
    if cur is None:
        continue
    if re.search(r'\bret\b', line):
        has_ret.add(cur)
    r = ref.search(line)
    if r:
        callers.setdefault(r.group(1), set()).add(cur)
    for im in imm.finditer(line):
        try:
            imm_addrs.add(int(im.group(1), 16))
        except ValueError:
            pass

def addr_in_data(a):
    needle = a.to_bytes(4, "little")
    for name, b, s, fo in secs:
        if name in ('.data', '.rdata', '.bss', 'INIT', 'PAGE') and data[fo:fo + s].count(needle):
            return True
    return False

def is_init(va):
    return sec_of(va) == 'INIT'

# --- filters 1-3 (independent of which functions get moved) ---
eligible = {}    # name -> set of caller names
for a, name in funcs:
    if sec_of(a) != '.text':         # filter 1: resident .text only
        continue
    if MOVABLE is not None and name not in MOVABLE:   # physically relocatable
        continue
    base = re.sub(r'(@\d+|\.(constprop|part|isra|lto_priv|cold).*)$', '', name).lstrip('_@')
    if base in DENY:                 # force-resident: never discard (see DENY above)
        continue
    if base in exp or name.lstrip('_@') in exp:   # ...and not exported (ABI root)
        continue
    if name not in has_ret:          # filter 2: returns normally
        continue
    if a in imm_addrs or addr_in_data(a):   # filter 3: address never taken
        continue
    cs = callers.get(name, set())
    if not cs:                       # no callers -> GC'd anyway / unreachable
        continue
    eligible[name] = cs

# --- filter 4: every caller is INIT.  With --closure, treat already-chosen
# candidates as INIT too and iterate to a fixpoint, since moving a function
# makes its callees init-only.  The call graph and the address-taken set are
# invariant under the moves (a call/address ref is preserved when its source
# is relocated to INIT, and filter 3 already scans INIT), so this static
# closure yields exactly what re-running after each relink would.
def callers_init(cs, chosen):
    return all(is_init(faddr.get(c, 0)) or c in chosen for c in cs)

if CLOSURE:
    chosen = set()
    changed = True
    while changed:
        changed = False
        for name, cs in eligible.items():
            if name not in chosen and callers_init(cs, chosen):
                chosen.add(name)
                changed = True
else:
    chosen = {name for name, cs in eligible.items() if callers_init(cs, set())}

cands = sorted(((size[name], name, sorted(eligible[name])[:4]) for name in chosen),
               reverse=True)
if NAMES_ONLY:
    for sz, name, cl in cands:
        print(name)
    sys.exit(0)
print('# resident .text functions that could be CODE_SEG("INIT") but are not')
print('# (returns-normally + address-never-taken + not-exported + all-callers-INIT)')
total = 0
for sz, name, cl in cands:
    total += sz
    print('%5d  %-46s <- %s' % (sz, name, ', '.join(cl)))
print('# %d candidates, %d B total' % (len(cands), total))
