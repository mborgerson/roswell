#!/usr/bin/env python3
#
# dead-objs.py -- find .c files that compile into xboxkrnl but contribute
# zero live symbols (text + data + bss + rdata) to the linked binary.
#
# usage:
#   tools/dead-objs.py                  # default release+DBG cross-check
#   tools/dead-objs.py --release-only   # skip DBG cross-check (faster, less safe)
#   tools/dead-objs.py --group          # group by owning CMakeLists/.cmake
#
# These files are pure compile-time waste -- their bytes never reach the
# final image (--gc-sections drops them) but they still cost a compiler
# invocation per build.  Gate them with `if(NOT SARCH STREQUAL "xbox")`
# in the relevant CMakeLists to skip the compile.
#
# WARNING: on-disk image size will NOT change after gating.  The linker
# already drops these.  This is purely a build-time / compile-CPU win.
#
# How it decides what's dead:
#
#   1. Parse build/build.ninja to get the obj -> source map.
#   2. Walk the xboxkrnl.exe link rule (including recursively into every
#      .a archive it references) to get the set of .obj files that
#      actually feed the linker.
#   3. For each linker-fed obj, run nm.  An obj is "dead" iff none of its
#      defined symbols (T/D/B/R/C/G) appear in the live symbol set of
#      *either* the release or the DBG-built xboxkrnl.exe.
#   4. The live symbol set is built with consistent lstrip-all-underscores
#      so PE mangling (`__foo` for C `_foo`) doesn't desync extraction
#      vs lookup.
#
# Failure modes the script avoids:
#
#  - Text-only audit misses files with live data symbols (e.g. ke/gate.c
#    defines KeInitializeGate as both a function and a data entry that
#    other objs index).  Fix: include D/B/R/C/G in the defined-symbol set.
#
#  - nm emits section-name pseudosymbols (`.bss`, `.data`, `.rdata`)
#    with T/D/R types -- they match the live set spuriously.  Fix: skip
#    any name starting with `.`.
#
#  - PE `__strnicmp` (two underscores) is the binary form of C `_strnicmp`
#    (one underscore).  Inconsistent stripping causes false negatives.
#    Fix: lstrip all leading `_` and `@` in both the live set and the
#    obj-symbol check.
#
#  - Some files are dead in release but live in DBG (e.g. ps/debug.c's
#    PspDumpThreads is called by kd64 only in DBG).  Fix: require dead
#    in BOTH builds.

import argparse
import os
import re
import subprocess
import sys

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BUILD_REL = os.path.join(REPO_ROOT, 'build')
BUILD_DBG = os.path.join(REPO_ROOT, 'build-dbg')
NM = 'i686-w64-mingw32-nm'
OBJDUMP = 'i686-w64-mingw32-objdump'
AR = 'i686-w64-mingw32-ar'


def fail(msg):
    print(f'dead-objs.py: {msg}', file=sys.stderr)
    sys.exit(1)


def normalise(sym):
    """Strip PE underscore-prefix + @stdcall suffix to a portable C name."""
    return sym.lstrip('_@').split('@')[0]


def live_symbols(exe_path):
    """Set of normalised symbol names defined in `exe_path` (any storage class)."""
    if not os.path.exists(exe_path):
        fail(f'missing {exe_path} -- build first')
    out = subprocess.run([NM, exe_path], capture_output=True, text=True, check=True)
    syms = set()
    for line in out.stdout.splitlines():
        parts = line.split()
        if len(parts) < 3:
            continue
        typ, name = parts[1], parts[2]
        # T/D/B/R/G (+ lowercase static): real defined symbols.  W = weak.
        if typ in 'TtDdBbRrCcGg':
            if name.startswith('.'):
                continue   # section-name pseudosymbol
            syms.add(normalise(name))
    return syms


def parse_ninja(build_dir):
    """Build two mappings from `build_dir/build.ninja`:

        obj_to_src[obj_relpath]  = absolute source path
        a_to_objs[lib.a_relpath] = list of obj_relpaths

    obj paths are relative to `build_dir`.
    """
    path = os.path.join(build_dir, 'build.ninja')
    if not os.path.exists(path):
        fail(f'no build.ninja at {path} -- cmake -S . -B {build_dir} first')
    obj_to_src = {}
    with open(path) as f:
        for line in f:
            m = re.match(r'^build (\S+\.obj): C_COMPILER\S+\s+(\S+)', line)
            if m:
                obj_to_src[m.group(1)] = m.group(2)
    # Now walk .a build rules to expand archives.  These build lines can be
    # long (one line per .a) so we re-scan with a different regex.
    a_to_objs = {}
    with open(path) as f:
        content = f.read()
    for m in re.finditer(r'^build (\S+\.a):[^\n]+', content, re.MULTILINE):
        line = m.group(0).split(' || ')[0].replace(' | ', ' ')
        a_to_objs[m.group(1)] = [w for w in line.split() if w.endswith('.obj')]
    return obj_to_src, a_to_objs


def linker_inputs(build_dir, target='ntoskrnl/xboxkrnl.exe'):
    """Walk the target link rule + .a archives to enumerate all input objs."""
    with open(os.path.join(build_dir, 'build.ninja')) as f:
        content = f.read()
    m = re.search(r'^build ' + re.escape(target) + r':[^\n]+',
                  content, re.MULTILINE)
    if not m:
        fail(f'no build rule for {target} in build.ninja')
    line = m.group(0).split(' || ')[0].replace(' | ', ' ')
    direct_objs = set(w for w in line.split() if w.endswith('.obj'))
    libs = set(w for w in line.split() if w.endswith('.a'))
    _, a_to_objs = parse_ninja(build_dir)
    all_objs = set(direct_objs)
    for lib in libs:
        for o in a_to_objs.get(lib, []):
            all_objs.add(o)
    return all_objs


def obj_defined_syms(obj_path):
    """Set of normalised symbol names DEFINED (not U) by `obj_path`."""
    if not os.path.exists(obj_path):
        return set()
    out = subprocess.run([NM, obj_path], capture_output=True, text=True)
    syms = set()
    for line in out.stdout.splitlines():
        parts = line.split()
        if len(parts) < 3:
            continue
        typ, name = parts[1], parts[2]
        if typ in 'TDBRCG' and not name.startswith('.'):
            syms.add(normalise(name))
    return syms


def obj_total_bytes(obj_path):
    """Sum of .text*/.rdata*/.xdata*/.data*/.bss* section sizes."""
    out = subprocess.run([OBJDUMP, '-h', obj_path], capture_output=True, text=True)
    total = 0
    for line in out.stdout.splitlines():
        parts = line.split()
        if len(parts) < 3:
            continue
        sect = parts[1]
        if any(sect.startswith(p) for p in
               ('.text', '.rdata', '.xdata', '.data', '.bss')):
            try:
                total += int(parts[2], 16)
            except ValueError:
                pass
    return total


def find_owning_cmake(src_path):
    """Best-effort: locate the CMakeLists.txt or .cmake that adds `src_path`."""
    basename = os.path.basename(src_path)
    if not basename:
        return None
    try:
        out = subprocess.check_output(
            ['grep', '-rln', '-w', basename,
             '--include=CMakeLists.txt', '--include=*.cmake',
             '--include=ntos.cmake',
             'ntoskrnl', 'drivers', 'hal', 'sdk'],
            cwd=REPO_ROOT, text=True,
        )
    except subprocess.CalledProcessError:
        return None
    matches = [m for m in out.strip().split('\n') if m]
    for m in matches:
        m_dir = os.path.dirname(m)
        if src_path.startswith(m_dir + '/'):
            return m
    return matches[0] if matches else None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--release-only', action='store_true',
                    help='skip DBG cross-check (faster, less safe)')
    ap.add_argument('--group', action='store_true',
                    help='group by owning CMakeLists/.cmake (slower)')
    ap.add_argument('--target', default='ntoskrnl/xboxkrnl.exe',
                    help='which exe to analyse (default: xboxkrnl)')
    args = ap.parse_args()

    # Live symbol set
    rel_exe = os.path.join(BUILD_REL, 'ntoskrnl', 'xboxkrnl.unstripped.exe')
    dbg_exe = os.path.join(BUILD_DBG, 'ntoskrnl', 'xboxkrnl.unstripped.exe')
    live = live_symbols(rel_exe)
    if not args.release_only:
        live |= live_symbols(dbg_exe)

    # Linker inputs (objs that feed the .exe)
    obj_to_src, _ = parse_ninja(BUILD_REL)
    objs = linker_inputs(BUILD_REL, args.target)

    dead = []
    for obj in objs:
        full_rel = os.path.join(BUILD_REL, obj)
        full_dbg = os.path.join(BUILD_DBG, obj)
        if not os.path.exists(full_rel):
            continue
        defined_rel = obj_defined_syms(full_rel)
        if defined_rel & live:
            continue
        if not args.release_only:
            defined_dbg = obj_defined_syms(full_dbg)
            if defined_dbg & live:
                continue
        size = obj_total_bytes(full_rel)
        if size == 0:
            continue
        src = obj_to_src.get(obj, obj)
        if src.startswith(REPO_ROOT + '/'):
            src = src[len(REPO_ROOT) + 1:]
        dead.append((size, src, obj))

    dead.sort(reverse=True)

    if args.group:
        groups = {}
        for sz, src, _ in dead:
            cmake = find_owning_cmake(src) or '?'
            groups.setdefault(cmake, []).append((sz, src))
        for cmake, files in sorted(groups.items(),
                                   key=lambda kv: -sum(s[0] for s in kv[1])):
            total = sum(s[0] for s in files)
            print(f'=== {total:7d} B in {len(files):3d} files  {cmake} ===')
            for sz, src in sorted(files, reverse=True):
                print(f'   {sz:6d}  {os.path.basename(src)}')
    else:
        for sz, src, _ in dead:
            print(f'{sz:6d}  {src}')

    total = sum(d[0] for d in dead)
    print(f'\n# {len(dead)} dead objs, {total} bytes '
          f'({total / 1024:.1f} KB) of compile-time waste', file=sys.stderr)


if __name__ == '__main__':
    main()
