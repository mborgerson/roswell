#!/usr/bin/env python3
#
# lto-init-relink.py -- two-stage link that moves boot-only functions into the
# discardable INIT section automatically, with no source-side CODE_SEG tags.
#
# Wired in as ntoskrnl's RULE_LAUNCH_LINK, so cmake/ninja invokes it with the
# full kernel link command as its arguments.  It:
#
#   1. runs the link (pass 1)  -> binary + incremental-LTO ltrans cache
#   2. asks find-init-candidates.py --closure for the full transitive set of
#      resident functions reachable only from INIT and safe to move
#      (returns-normally, address-never-taken, not-exported, all-callers-INIT).
#      Moving a function makes its callees init-only too, so the closure is
#      computed once over the (move-invariant) call graph rather than by
#      relinking after each round.
#   3. renames each one's per-function `.text$<fn>` section to `INIT$<fn>` --
#      the lds already routes `*(INIT$*)` into the INIT output section, so no
#      lds change and no section-order fight.  Two kinds of input carry the
#      definitions:
#        * LTO ltrans objects (build/.../lto-cache) -- renamed in place; the
#          cache is gcc-internal, not a ninja node, so this causes no rebuild.
#        * non-LTO static libs (rtl/crt/...) -- renamed into PRIVATE COPIES in
#          lto-init-libobjs/ that are prepended to the link so they shadow the
#          archive's members.  The originals are never touched, so ninja still
#          sees a clean no-op build (modifying tracked inputs would loop).
#   4. relinks once with the whole set moved -- that final relink is the link
#      output ninja consumes
#
# Requires -ffunction-sections on the LINK so per-function sections survive LTO
# codegen (and at compile time for the libs).  The build's init-ref-check is the
# post-link safety gate: a wrongly-moved function (resident caller) fails it.
# Any link that isn't the kernel passes straight through unchanged.
#
# Env (set by the cmake RULE_LAUNCH_LINK):
#   LTO_INIT_FINDTOOL  path to tools/find-init-candidates.py
#   DEF                xboxkrnl.def (export filter for find-init-candidates.py)
#   NM / OBJDUMP / OBJCOPY / AR   i686-w64-mingw32 binutils

import os
import re
import shutil
import subprocess
import sys

LINK = sys.argv[1:]

def run(cmd):
    return subprocess.run(cmd).returncode

# --- pass 1 ---
rc = run(LINK)
if rc != 0:
    sys.exit(rc)

# Only post-process the kernel link.
out = cache = None
for i, a in enumerate(LINK):
    if a == "-o" and i + 1 < len(LINK):
        out = LINK[i + 1]
    elif a.startswith("-o") and len(a) > 2:
        out = a[2:]
    elif a.startswith("-flto-incremental="):
        cache = a.split("=", 1)[1]
if not out or os.path.basename(out) != "xboxkrnl.exe":
    sys.exit(0)

findtool = os.environ.get("LTO_INIT_FINDTOOL")
objcopy = os.environ.get("OBJCOPY", "i686-w64-mingw32-objcopy")
objdump = os.environ.get("OBJDUMP", "i686-w64-mingw32-objdump")
ar = os.environ.get("AR", "i686-w64-mingw32-ar")
if not findtool or not cache or not os.path.isdir(cache):
    print("lto-init-relink.py: not configured (findtool/cache) -- skipping INIT placement",
          file=sys.stderr)
    sys.exit(0)

libdir = os.path.join(os.path.dirname(cache.rstrip("/")), "lto-init-libobjs")
os.makedirs(libdir, exist_ok=True)
sub = {}   # original lib member path -> private renamed copy (shadows the .a)

sec_line = re.compile(r"\(sec\s+(\d+)\).*?\s(\S+)\s*$")

def ltrans_objs():
    return sorted(os.path.join(cache, f) for f in os.listdir(cache)
                  if f.endswith(".ltrans.o"))

def archive_members(a):
    """Object files a thin static archive references (objcopy can't rewrite a
    thin archive itself, but its members are ordinary on-disk COFF objects).
    Member names are stored relative to the build dir; the link runs from
    there, so try the name as-is and a couple of fallbacks."""
    names = subprocess.run([ar, "t", a], capture_output=True, text=True).stdout.split()
    root = os.path.dirname(os.getcwd())
    adir = os.path.dirname(os.path.abspath(a))
    members = []
    for m in names:
        for cand in (m, os.path.join(root, m), os.path.join(adir, os.path.basename(m))):
            if os.path.isfile(cand):
                members.append(cand)
                break
    return members

def lib_members():
    members = []
    for a in LINK:
        if a.endswith(".a") and os.path.isfile(a):
            members += archive_members(a)
    return members

def text_dollar_syms(path):
    """{function symbol -> its `.text$<fn>` section name} for `path`.  Maps
    section<->symbol via the symbol table (exact name, no normalization),
    handling archive members whose indices reset per member.  A function in a
    shared `.text` (hand-written asm) has no `.text$` section, so it's absent
    here -- i.e. not movable, and excluded from the closure below."""
    dump = subprocess.run([objdump, "-t", path], capture_output=True, text=True).stdout
    out_map = {}
    sec_name, fn_sec = {}, {}

    def flush():
        for sym, idx in fn_sec.items():
            if idx in sec_name:
                out_map[sym] = sec_name[idx]

    for line in dump.splitlines():
        if "file format" in line:
            flush()
            sec_name, fn_sec = {}, {}
            continue
        m = sec_line.search(line)
        if not m:
            continue
        idx, name = int(m.group(1)), m.group(2)
        if name.startswith(".text$"):
            sec_name[idx] = name
        elif idx != 0 and not name.startswith("."):
            fn_sec.setdefault(name, idx)
    flush()
    return out_map

def apply(path, sections):
    args = []
    for s in sorted(sections):
        args += ["--rename-section", "%s=INIT$%s" % (s, s[len(".text$"):])]
    if run([objcopy] + args + [path]) != 0:
        print("lto-init-relink.py: objcopy failed on %s" % path, file=sys.stderr)
        sys.exit(1)

def link_cmd():
    # Prepend the private renamed copies so they shadow the archive members.
    return [LINK[0]] + list(sub.values()) + LINK[1:]

# Scan every COFF input ONCE for its per-function sections.  The set of
# symbols that have a `.text$<fn>` section is exactly what we can move, and it
# feeds the closure: a function is only chosen if all its callers are INIT or
# are themselves movable-and-chosen.  This keeps the logical set and what we
# can physically relocate in lockstep -- otherwise the closure could move a
# function whose (resident, unmovable) caller stays put, a use-after-free that
# the per-round relink loop avoided by re-deriving from the actual binary.
def scan_ltrans():
    return {lt: text_dollar_syms(lt) for lt in ltrans_objs()}

# Scan every COFF input for its per-function sections.  The set of symbols that
# have a `.text$<fn>` section is exactly what we can relocate, and it feeds the
# closure: a function is only chosen if all its callers are INIT or are
# themselves movable-and-chosen.  This keeps the logical set and what we can
# physically move in lockstep -- otherwise the closure could move a function
# whose (resident, unmovable) caller stays put, a use-after-free.
lt_syms = scan_ltrans()
lib_syms = {m: text_dollar_syms(m) for m in lib_members()}
movable = set().union(*(list(lt_syms.values()) + list(lib_syms.values()))) \
    if (lt_syms or lib_syms) else set()

movfile = os.path.join(libdir, "movable.txt")
with open(movfile, "w") as f:
    f.write("\n".join(sorted(movable)))

# The full transitive init-only set, computed once over the static (move-
# invariant) call graph restricted to the movable universe.
env = dict(os.environ, MOVABLE_FILE=movfile)
res = subprocess.run([sys.executable, findtool, out, "--names", "--closure"],
                     capture_output=True, text=True, env=env)
want = set(res.stdout.split())

# Moving is two phases because introducing the lib-shadow objects changes the
# link inputs, which makes gcc REGENERATE the incremental-LTO ltrans cache --
# wiping any in-place ltrans renames done beforehand.  So:
#   A. rename the lib members into shadow copies, then relink ONCE -- this
#      settles the new input set and regenerates the ltrans cache.
#   B. rename the ltrans in the now-stable cache, then relink again with the
#      SAME inputs, so the renames survive (a same-input relink reuses the
#      cache as-is).
# Bounded at three links total regardless of cascade depth.
lib_moved = lt_moved = 0
for member, smap in lib_syms.items():
    secs = {sec for sym, sec in smap.items() if sym in want}
    if secs:
        dst = os.path.join(libdir, member.replace("/", "_").replace("\\", "_"))
        shutil.copyfile(member, dst)
        apply(dst, secs)
        sub[member] = dst
        lib_moved += len(secs)
if sub:
    rc = run(link_cmd())                  # phase A relink (regenerates ltrans)
    if rc != 0:
        sys.exit(rc)
    lt_syms = scan_ltrans()               # re-read the regenerated cache

for lt, smap in lt_syms.items():
    secs = {sec for sym, sec in smap.items() if sym in want}
    if secs:
        apply(lt, secs)                   # in-place in the untracked cache
        lt_moved += len(secs)
if lt_moved or (lib_moved and not sub):
    rc = run(link_cmd())                  # phase B relink (same inputs -> kept)
    if rc != 0:
        sys.exit(rc)
print("lto-init-relink.py: moved %d init-only functions to INIT (%d lib, %d lto)"
      % (lib_moved + lt_moved, lib_moved, lt_moved), file=sys.stderr)
sys.exit(0)
