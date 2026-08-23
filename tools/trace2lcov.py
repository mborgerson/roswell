#!/usr/bin/env python3
"""
trace2lcov.py: convert xemu `-d in_asm,nochain` trace logs into an lcov
tracefile (.info) using the kernel's DWARF line tables.

The in_asm log records every instruction of every translated block, and
translation happens at first execution, so an instruction address in the
log means "this instruction ran at least once".  Every DWARF line-table
row starts at an instruction boundary, so a source line is hit iff any
of its row addresses appears in the trace.

Requires a kernel built with -DCOVERAGE=ON (Release builds otherwise
carry no line info).  Line tables are read from the unstripped image.

Usage:
    trace2lcov.py [-o coverage.info] [--exe xboxkrnl.unstripped.exe]
                  trace.log [trace.log...]

Multiple logs union.  Paths in the output are repo-relative; files
outside the repo (libgcc, mingw CRT headers) are dropped.

Env: OBJDUMP overrides the objdump binary (default i686-w64-mingw32-objdump).
"""

import argparse
import os
import re
import subprocess
import sys
from collections import defaultdict

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def parse_trace(paths):
    """Union of instruction addresses across all trace logs."""
    addr_re = re.compile(r"^0x([0-9a-fA-F]+):")
    addrs = set()
    for path in paths:
        n0 = len(addrs)
        with open(path, errors="replace") as f:
            for line in f:
                m = addr_re.match(line)
                if m:
                    addrs.add(int(m.group(1), 16))
        print(f"  {path}: {len(addrs) - n0} new instruction addrs",
              file=sys.stderr)
    return addrs


def pe_image_range(path):
    """[base, base+SizeOfImage) from the PE optional header."""
    d = open(path, "rb").read(0x400)
    e = int.from_bytes(d[0x3C:0x40], "little")
    base = int.from_bytes(d[e + 24 + 28:e + 24 + 32], "little")
    size = int.from_bytes(d[e + 24 + 56:e + 24 + 60], "little")
    return base, base + size


def parse_decodedline(exe, objdump):
    """Yield (source-path, line, addr) rows from the DWARF line tables.

    objdump --dwarf=decodedline prints per-CU blocks:

        CU: /abs/path/to/foo.c:
        File name    Line number    Starting address    View    Stmt
        foo.c                 12            0x84001000            x
        ./hdr/bar.h            3            0x84001010            x
        foo.c                  -            0x84001020

    The file-name column is relative to the CU's directory when it isn't
    the CU's own file; '-' in the line column marks end-of-sequence.
    """
    out = subprocess.run(
        [objdump, "--dwarf=decodedline", exe],
        check=True, capture_output=True, text=True).stdout

    cu_path = None
    cu_dir = ""
    cu_base = None  # file-name column value that means "the CU file itself"
    row_re = re.compile(r"^(.*?)\s+(\d+|-)\s+0x([0-9a-fA-F]+)")
    for line in out.splitlines():
        line = line.rstrip()
        if not line or line.startswith(("Contents of", "File name", " ")):
            continue
        if line.startswith("CU:"):
            cu_path = line[3:].strip().rstrip(":")
            cu_dir = os.path.dirname(cu_path)
            cu_base = os.path.basename(cu_path)
            continue
        # Some binutils versions print short CU paths as a bare "path:"
        # header line instead of "CU: path:".
        if line.endswith(":") and "0x" not in line:
            cu_path = line.rstrip(":")
            cu_dir = os.path.dirname(cu_path)
            cu_base = os.path.basename(cu_path)
            continue
        m = row_re.match(line)
        if not m or cu_path is None:
            continue
        fname, lineno, addr = m.group(1).strip(), m.group(2), m.group(3)
        if lineno == "-":  # end of sequence
            continue
        if fname == cu_base:
            src = cu_path
        elif os.path.isabs(fname):
            src = fname
        else:
            src = os.path.normpath(os.path.join(cu_dir, fname))
        yield src, int(lineno), int(addr, 16)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("traces", nargs="+", help="xemu -d in_asm trace log(s)")
    ap.add_argument("-o", "--output", default="coverage.info")
    ap.add_argument("--exe",
                    default=os.path.join(REPO, "build-cov", "ntoskrnl",
                                         "xboxkrnl.unstripped.exe"))
    ap.add_argument("--src-root", default=REPO,
                    help="files outside this tree are dropped; paths in "
                         "the output are relative to it")
    args = ap.parse_args()
    objdump = os.environ.get("OBJDUMP", "i686-w64-mingw32-objdump")

    print("reading trace logs...", file=sys.stderr)
    traced = parse_trace(args.traces)
    lo, hi = pe_image_range(args.exe)
    traced_img = {a for a in traced if lo <= a < hi}
    print(f"  {len(traced_img)} in kernel image [{lo:#x},{hi:#x})",
          file=sys.stderr)

    print(f"reading line tables from {args.exe}...", file=sys.stderr)
    src_root = os.path.realpath(args.src_root)

    def to_relpath(src, _cache={}):
        """Map a DWARF path to repo-relative, or None to drop it.

        The build compiles with -ffile-prefix-map=<repo>= so DWARF paths
        arrive repo-relative, but with a leading slash when the compiler
        saw an absolute path (/sdk/lib/...).  Genuinely absolute paths
        (mingw CRT headers) stay absolute and fall outside the repo.
        """
        if src not in _cache:
            rel = src.lstrip("/") if not os.path.exists(src) else None
            if rel is None:
                real = os.path.realpath(src)
                rel = (os.path.relpath(real, src_root)
                       if real.startswith(src_root + os.sep) else None)
            if rel is not None:
                rel = os.path.normpath(rel)
                if not os.path.exists(os.path.join(src_root, rel)):
                    rel = None
            _cache[src] = rel
        return _cache[src]

    # (file, line) -> hit?  A line is hit if any of its addresses ran.
    lines = defaultdict(bool)
    nrows = 0
    for src, lineno, addr in parse_decodedline(args.exe, objdump):
        if not (lo <= addr < hi):
            continue  # tombstoned rows from --gc-sections/LTO-dropped code
        rel = to_relpath(src)
        if rel is None:
            continue
        nrows += 1
        lines[(rel, lineno)] |= addr in traced_img
    if not nrows:
        sys.exit("trace2lcov: no line-table rows found -- was the kernel "
                 "built with -DCOVERAGE=ON?")

    per_file = defaultdict(dict)
    for (rel, lineno), hit in lines.items():
        per_file[rel][lineno] = hit

    with open(args.output, "w") as out:
        out.write("TN:\n")
        for rel in sorted(per_file):
            fl = per_file[rel]
            out.write(f"SF:{rel}\n")
            for lineno in sorted(fl):
                out.write(f"DA:{lineno},{1 if fl[lineno] else 0}\n")
            out.write(f"LF:{len(fl)}\n")
            out.write(f"LH:{sum(fl.values())}\n")
            out.write("end_of_record\n")

    total = sum(len(f) for f in per_file.values())
    hit = sum(sum(f.values()) for f in per_file.values())
    print(f"{args.output}: {len(per_file)} files, "
          f"{hit}/{total} lines hit ({100.0 * hit / total:.1f}%)",
          file=sys.stderr)


if __name__ == "__main__":
    main()
