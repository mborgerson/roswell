#!/usr/bin/env python3
#
# lto-init-relink.py -- two-stage link that moves boot-only functions into the
# discardable INIT section automatically, with no source-side CODE_SEG tags.
#
# Wired in as ntoskrnl's RULE_LAUNCH_LINK, so cmake/ninja invokes it with the
# full kernel link command as its arguments.  It:
#
#   1. runs the link (pass 1) with -save-temps, which keeps the LTO ltrans
#      objects next to the output instead of deleting them
#   2. asks find-init-candidates.py --closure for the full transitive set of
#      resident functions reachable only from INIT and safe to move
#      (returns-normally, address-never-taken, not-exported, all-callers-INIT).
#      Moving a function makes its callees init-only too, so the closure is
#      computed once over the (move-invariant) call graph rather than by
#      relinking after each round.
#   3. writes a copy of the linker script that lists each chosen function's
#      per-function `.text$<fn>` input section inside the INIT output section,
#      and drops .text's `*(SORT(.text$*))` catch-all so those names are free
#      to match INIT.  Placement is the LINKER's job -- no object is rewritten.
#   4. relinks once against that script -- that link is the output ninja
#      consumes
#
# Requires -ffunction-sections on the LINK so per-function sections survive LTO
# codegen (and at compile time for the libs).  Anything the script does not
# name -- hand-written asm in a shared `.text`, a function that appears only in
# pass 2 -- is an orphan section, and ld folds `.text$*` orphans back into
# .text, so the failure mode is "stayed resident", not "silently discarded".
# The build's init-ref-check is the post-link safety gate: a wrongly-moved
# function (resident caller) fails it.  Any link that isn't the kernel passes
# straight through unchanged.
#
# Env (set by the cmake RULE_LAUNCH_LINK):
#   LTO_INIT_FINDTOOL  path to tools/find-init-candidates.py
#   DEF                xboxkrnl.def (export filter for find-init-candidates.py)
#   NM / OBJDUMP / AR  i686-w64-mingw32 binutils

import glob
import os
import re
import subprocess
import sys

LINK = sys.argv[1:]

# .text's catch-all for per-function sections, and the INIT line the generated
# list is spliced in after.  Both are matched literally in the linker script;
# an edit that renames either one has to be reflected here.
TEXT_CATCHALL = "*(SORT(.text$*))"
INIT_ANCHOR = "*(INIT$*)"

WILDCARD = re.compile(r"[*?\[\]]")


def run(cmd):
    return subprocess.run(cmd).returncode


# Only post-process the kernel link.
out = script = script_arg = None
for a in LINK:
    if a.startswith("-Wl,-T,") or a.startswith("-Wl,--script="):
        script_arg, script = a, a.split(",")[-1].split("=")[-1]
for i, a in enumerate(LINK):
    if a == "-o" and i + 1 < len(LINK):
        out = LINK[i + 1]
    elif a.startswith("-o") and len(a) > 2:
        out = a[2:]
if not out or os.path.basename(out) != "xboxkrnl.exe":
    sys.exit(run(LINK))

findtool = os.environ.get("LTO_INIT_FINDTOOL")
objdump = os.environ.get("OBJDUMP", "i686-w64-mingw32-objdump")
ar = os.environ.get("AR", "i686-w64-mingw32-ar")

# --- pass 1: link, keeping the ltrans objects ------------------------------
# -save-temps leaves `<output base>.ltrans<N>.ltrans.o` in the link directory.
# GCC deletes them otherwise, and only GCC 16's -flto-incremental cache keeps
# them anywhere else -- CI's GCC 13 has no cache, so ask for them explicitly.
rc = run(LINK + ["-save-temps"])
if rc != 0:
    sys.exit(rc)

outdir = os.path.dirname(os.path.abspath(out))
temps = sorted(glob.glob(os.path.join(
    outdir, os.path.splitext(os.path.basename(out))[0] + ".ltrans*.ltrans.o")))
if not temps:
    # GCC 16's incremental LTO keeps them in its cache instead.
    for a in LINK:
        if a.startswith("-flto-incremental="):
            temps = sorted(glob.glob(os.path.join(a.split("=", 1)[1],
                                                  "*.ltrans.o")))
if not temps:
    # Only the handful of functions in non-LTO static libs can move without
    # them, so say so rather than quietly placing a fraction of the set.
    print("lto-init-relink.py: no ltrans objects from -save-temps -- LTO "
          "functions cannot be placed in INIT", file=sys.stderr)

if not findtool or not script or not os.path.isfile(script):
    print("lto-init-relink.py: not configured (findtool/linker script) -- "
          "skipping INIT placement", file=sys.stderr)
    sys.exit(0)


def archive_members(a):
    """Object files a thin static archive references (its members are ordinary
    on-disk COFF objects).  Member names are stored relative to the build dir;
    the link runs from there, so try the name as-is and a couple of
    fallbacks."""
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


sec_line = re.compile(r"\(sec\s+(\d+)\).*?\s(\S+)\s*$")


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


# Scan every COFF input ONCE for its per-function sections.  The set of
# symbols that have a `.text$<fn>` section is exactly what we can move, and it
# feeds the closure: a function is only chosen if all its callers are INIT or
# are themselves movable-and-chosen.  This keeps the logical set and what we
# can physically relocate in lockstep -- otherwise the closure could move a
# function whose (resident, unmovable) caller stays put, a use-after-free.
lt_syms = {t: text_dollar_syms(t) for t in temps}
lib_syms = {m: text_dollar_syms(m) for m in lib_members()}
movable = set().union(*(list(lt_syms.values()) + list(lib_syms.values()))) \
    if (lt_syms or lib_syms) else set()

movfile = os.path.join(outdir, "lto-init-movable.txt")
with open(movfile, "w") as f:
    f.write("\n".join(sorted(movable)))

# The full transitive init-only set, computed once over the static (move-
# invariant) call graph restricted to the movable universe.
env = dict(os.environ, MOVABLE_FILE=movfile)
res = subprocess.run([sys.executable, findtool, out, "--names", "--closure"],
                     capture_output=True, text=True, env=env)
want = set(res.stdout.split())

chosen = {}          # section name -> function symbol
lt_sections, lib_sections = set(), set()
for maps, dst in ((lt_syms, lt_sections), (lib_syms, lib_sections)):
    for smap in maps.values():
        for sym, sec in smap.items():
            if sym in want and not WILDCARD.search(sec):
                chosen[sec] = sym
                dst.add(sec)
sections = sorted(chosen)
# ld reads a section name as a wildcard pattern, so one containing a
# metacharacter cannot be named exactly.  Nothing GCC emits for C hits this;
# leave any such function resident rather than matching more than intended.
skipped = {sec for maps in (lt_syms, lib_syms) for smap in maps.values()
           for sym, sec in smap.items() if sym in want and WILDCARD.search(sec)}
if skipped:
    print("lto-init-relink.py: %d section(s) left resident -- name is not "
          "literal: %s" % (len(skipped), ", ".join(sorted(skipped)[:4])),
          file=sys.stderr)

# The saved ltrans objects have served their purpose; pass 2 regenerates them
# (or reuses the incremental-LTO cache) and their names are identical either
# way, since the inputs did not change -- only the linker script did.  Only
# ever remove what -save-temps left next to the output; the cache is GCC's.
for t in glob.glob(os.path.join(
        outdir, os.path.splitext(os.path.basename(out))[0] + ".ltrans*")):
    os.remove(t)

if not sections:
    print("lto-init-relink.py: no init-only functions found -- INIT placement "
          "is a no-op", file=sys.stderr)
    sys.exit(0)

# --- pass 2: relink against a script that places them ----------------------
text = open(script).read()
if TEXT_CATCHALL not in text or INIT_ANCHOR not in text:
    print("lto-init-relink.py: %s has no '%s' / '%s' to rewrite -- skipping "
          "INIT placement" % (script, TEXT_CATCHALL, INIT_ANCHOR),
          file=sys.stderr)
    sys.exit(0)

listing = "\n".join("    *(%s)" % s for s in sections)
text = text.replace(
    TEXT_CATCHALL,
    "/* The per-function catch-all lives in INIT below, as the generated\n"
    "       list of boot-only functions.  ld folds every `.text$*` orphan\n"
    "       this leaves -- everything not on that list -- back in here. */")
text = text.replace(INIT_ANCHOR, INIT_ANCHOR + "\n"
                    "    /* generated by tools/lto-init-relink.py */\n" + listing)

generated = os.path.join(outdir, "xboxkrnl-init.lds")
with open(generated, "w") as f:
    f.write(text)

rc = run([("-Wl,-T," + generated) if a == script_arg else a for a in LINK])
if rc != 0:
    sys.exit(rc)

# What actually landed.  A section name the pass-2 codegen no longer emits
# matches nothing and its function just stays resident, so report where the
# chosen functions ended up rather than how many were asked for.
nm = subprocess.run([os.environ.get("NM", "i686-w64-mingw32-nm"), out],
                    capture_output=True, text=True).stdout
addrs = {}
for line in nm.splitlines():
    parts = line.split()
    if len(parts) == 3:
        addrs.setdefault(parts[2], int(parts[0], 16))
start, end = addrs.get("__init_start__"), addrs.get("__init_end__")
if start is None or end is None:
    sys.exit("lto-init-relink.py: %s has no INIT bounds after the relink" % out)
placed = {sec for sec, sym in chosen.items() if start <= addrs.get(sym, -1) < end}
print("lto-init-relink.py: moved %d init-only functions to INIT (%d lib, %d lto)"
      % (len(placed), len(placed & lib_sections), len(placed & lt_sections)),
      file=sys.stderr)
if len(placed) < len(sections):
    print("lto-init-relink.py: %d of %d stayed resident (no matching section in "
          "the pass-2 link)" % (len(sections) - len(placed), len(sections)),
          file=sys.stderr)
sys.exit(0)
