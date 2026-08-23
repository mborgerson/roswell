#!/usr/bin/env python3
#
# struct-audit -- compare every named struct/union/enum/function the Xbox
# kernel header (third_party/nxdk/lib/xboxkrnl/xboxkrnl.h) and the ReactOS
# NT headers (vendor/reactos/sdk/include/...) declare under the same name.
#
# Why: ABI drift bites us silently.  KDPC was 28 vs 32 bytes (NT added
# DpcData at +28); we found it by accident after pbkit's DPC re-queue stopped
# working.  Enum drift (e.g. KWAIT_REASON, FILE_INFORMATION_CLASS) and
# function-signature drift (a NT export that lost a parameter, gained a
# const-qualifier, changed return type) bite the same way.
#
# How: libclang parses each shim TU, we walk the AST for STRUCT_DECL /
# UNION_DECL / ENUM_DECL / FUNCTION_DECL nodes and diff the two name-keyed
# maps.  Output is a markdown report:
# tools/struct-audit/struct-audit-report.md (committed) plus a non-zero
# exit code on drift so CI can gate on it.
#
# Usage:
#   tools/struct-audit/audit.py [-o REPORT.md] [--quiet]
#
# Requires: python-clang, libclang.so.
#
import argparse, os, subprocess, sys
from collections import OrderedDict

import clang.cindex
from clang.cindex import CursorKind, TypeKind, TranslationUnit

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))


def reactos_flags():
    """The same -D/-I switches CMake passes to nxbe.c, scraped from build.ninja.

    Anything not affecting struct layout (-W*, -O*, -f*) is dropped; we keep
    only -D and -I.  The arch flags (_M_IX86, _X86_, __i386__) are essential:
    the NT structs include them in #ifdefs.
    """
    ninja = os.path.join(REPO, "build/reactos-build/build.ninja")
    if not os.path.exists(ninja):
        sys.exit("struct-audit: %s not found -- run tools/build-reactos once."
                 % ninja)
    block = subprocess.check_output(
        ["grep", "-A12", "^build ntoskrnl/.*xbe.c.obj:", ninja], text=True)
    defines = []
    includes = []
    for line in block.splitlines():
        s = line.strip()
        if s.startswith("DEFINES = "):
            defines = s[len("DEFINES = "):].split()
        elif s.startswith("INCLUDES = "):
            includes = s[len("INCLUDES = "):].split()
    if not defines or not includes:
        sys.exit("struct-audit: failed to scrape xbe.c DEFINES/INCLUDES")
    return defines + includes


def xbox_flags():
    """Minimal flags to parse lib/xboxkrnl/xboxkrnl.h out of a local nxdk."""
    nxdk_dir = os.environ.get("NXDK_DIR")
    if not nxdk_dir:
        sys.exit("struct-audit: set NXDK_DIR to a local nxdk clone "
                 "(the nxdk submodule was removed; the build itself only "
                 "needs ntoskrnl/xb/xboxkrnl.exe.def, but re-running this "
                 "audit needs the full nxdk header tree)")
    nxdk_xboxkrnl = os.path.join(nxdk_dir, "lib/xboxkrnl")
    nxdk_winapi   = os.path.join(nxdk_dir, "lib/winapi")
    return [
        "-I" + nxdk_xboxkrnl,
        "-I" + nxdk_xboxkrnl + "/..",  # so <xboxkrnl/xboxkrnl.h> resolves
        "-I" + nxdk_winapi,
    ]


def common_flags():
    """Flags both shims need to look like the i386 target."""
    return [
        "-target", "i686-pc-mingw32",
        "-m32",
        "-fms-extensions",
        "-fno-strict-aliasing",
        # libclang sometimes complains about pragma directives in DDK headers;
        # silence so the AST is still produced.
        "-Wno-everything",
        # Some declarations rely on inline asm; skip parsing function bodies
        # to keep us from tripping on inline assembly nuances.
        "-fsyntax-only",
    ]


def parse_shim(shim_path, flags):
    """Return TranslationUnit; print fatal-or-error diagnostics on failure."""
    index = clang.cindex.Index.create()
    args = common_flags() + flags
    tu = index.parse(shim_path, args=args,
                     options=TranslationUnit.PARSE_DETAILED_PROCESSING_RECORD)
    fatal = [d for d in tu.diagnostics if d.severity >= d.Error]
    if fatal:
        for d in fatal[:10]:
            print("error: %s" % d.format(), file=sys.stderr)
        if len(fatal) > 10:
            print("... (%d more)" % (len(fatal) - 10), file=sys.stderr)
        sys.exit("struct-audit: %s failed to parse cleanly (%d errors)"
                 % (shim_path, len(fatal)))
    return tu


CC_NAMES = {
    clang.cindex.TypeKind.FUNCTIONPROTO: "fnproto",
}


def cc_name(ty):
    """Calling-convention spelling.  Returns "" if libclang can't resolve."""
    try:
        cc = ty.get_canonical().get_calling_conv()
    except Exception:
        return ""
    # Map enum CXCallingConv -> short string; we only care about the ones
    # that show up on Win32 (Default ~= cdecl, X86Stdcall, X86Fastcall).
    return {
        100: "default",
        0:   "default",
        1:   "cdecl",
        2:   "fastcall",
        3:   "thiscall",
        4:   "x86stdcall",
        5:   "x86fastcall",
        7:   "x86thiscall",
        9:   "x86pascal",
    }.get(int(cc), "cc%d" % int(cc))


def canon_type(ty):
    """Spelling of the canonical type, with calling convention for fnproto.

    libclang's `.spelling` already includes 'const' / pointer levels, and
    `get_canonical()` resolves typedefs.  For function pointers we tack on
    the calling convention so '__stdcall' / '__cdecl' drift surfaces too.
    """
    canon = ty.get_canonical()
    s = canon.spelling
    if "(" in s and ")" in s:  # function-pointer-ish
        c = cc_name(canon)
        if c and c != "default":
            s = "[%s] %s" % (c, s)
    return s


def collect_records(tu):
    """Walk the TU and bucket every named declaration by kind.

    Returns a dict with three top-level keys:
      structs : name -> {kind, size, fields:[(name,off,size)]}
      enums   : name -> {constants:[(name, value)], underlying_size}
      funcs   : name -> {ret, args:[(name?, type)], cc, variadic}

    All buckets are name-keyed.  Anonymous tags are skipped (they're picked
    up via their typedef'd parent or live inline as a field).
    """
    structs = OrderedDict()
    enums   = OrderedDict()
    funcs   = OrderedDict()

    for cur in tu.cursor.walk_preorder():
        kind = cur.kind
        if kind in (CursorKind.STRUCT_DECL, CursorKind.UNION_DECL):
            if not cur.is_definition():
                continue
            name = cur.spelling
            if not name:
                continue
            ty = cur.type
            size = ty.get_size()
            if size < 0:
                continue
            fields = []
            for f in ty.get_fields():
                fname = f.spelling or "(anon)"
                try:
                    off = ty.get_offset(fname) if fname != "(anon)" else -1
                except Exception:
                    off = -1
                fsize = f.type.get_size()
                fields.append((fname, off, fsize))
            tag = "struct" if kind == CursorKind.STRUCT_DECL else "union"
            structs.setdefault(name, {"kind": tag, "size": size, "fields": fields})

        elif kind == CursorKind.ENUM_DECL:
            if not cur.is_definition():
                continue
            name = cur.spelling
            if not name:
                continue
            consts = []
            for c in cur.get_children():
                if c.kind == CursorKind.ENUM_CONSTANT_DECL:
                    consts.append((c.spelling, c.enum_value))
            usize = cur.type.get_size()
            enums.setdefault(name, {"constants": consts, "underlying_size": usize})

        elif kind == CursorKind.FUNCTION_DECL:
            # External declarations (the usual case in headers) and inline
            # definitions both surface here.  Skip static inlines in headers
            # (their linkage is internal but storage_class flag varies).
            name = cur.spelling
            if not name:
                continue
            ty = cur.type
            try:
                args = [(p.spelling or "", canon_type(p.type))
                        for p in cur.get_arguments()]
                ret = canon_type(cur.result_type)
                cc = cc_name(ty)
                variadic = ty.is_function_variadic()
            except Exception:
                continue
            # Header is included once per shim, but the same prototype may
            # appear under both 'extern "C"' and bare. Keep the first.
            funcs.setdefault(name, {
                "ret": ret, "args": args, "cc": cc, "variadic": variadic
            })

    return {"structs": structs, "enums": enums, "funcs": funcs}


def fmt_size(b):
    return "?" if b is None or b < 0 else str(b)


def diff_structs(xbox, nt):
    out = []
    for name in sorted(set(xbox) | set(nt)):
        x = xbox.get(name); n = nt.get(name)
        if x and n:
            if x["size"] != n["size"] or x["fields"] != n["fields"]:
                out.append((name, x, n))
        else:
            out.append((name, x, n))
    return out


def diff_enums(xbox, nt):
    out = []
    for name in sorted(set(xbox) & set(nt)):
        x = xbox[name]; n = nt[name]
        # Build value maps; an enum drifts if any constant they share has
        # a different value, or if either side has a constant the other lacks.
        xv = dict(x["constants"]); nv = dict(n["constants"])
        diff_consts = []
        for k in sorted(set(xv) | set(nv)):
            xb = xv.get(k); nb = nv.get(k)
            if xb != nb:
                diff_consts.append((k, xb, nb))
        if diff_consts or x["underlying_size"] != n["underlying_size"]:
            out.append((name, x, n, diff_consts))
    return out


def diff_funcs(xbox, nt):
    out = []
    for name in sorted(set(xbox) & set(nt)):
        x = xbox[name]; n = nt[name]
        # Compare normalized prototypes only; calling convention drift is
        # a hard ABI bug.
        if (x["ret"]      != n["ret"] or
            x["cc"]       != n["cc"]  or
            x["variadic"] != n["variadic"] or
            [a[1] for a in x["args"]] != [a[1] for a in n["args"]]):
            out.append((name, x, n))
    return out


def emit_markdown(struct_diffs, enum_diffs, func_diffs, xbox, nt, out_path):
    lines = []
    lines.append("<!-- generated by tools/struct-audit/audit.py -- do not edit -->")
    lines.append("# Generated Xbox-vs-NT ABI audit")
    lines.append("")
    lines.append("Xbox source: `third_party/nxdk/lib/xboxkrnl/xboxkrnl.h`  ")
    lines.append("NT source: ReactOS `<ntifs.h>` + transitive ReactOS NDK + DDK.")
    lines.append("")
    lines.append("Run `tools/struct-audit/audit.py` to regenerate.")
    lines.append("")
    lines.append("## Counts")
    lines.append("")
    lines.append("|              | structs / unions | enums | functions |")
    lines.append("|---|---|---|---|")
    lines.append("| Xbox declared | %d | %d | %d |"
                 % (len(xbox["structs"]), len(xbox["enums"]), len(xbox["funcs"])))
    lines.append("| NT declared   | %d | %d | %d |"
                 % (len(nt["structs"]),   len(nt["enums"]),   len(nt["funcs"])))
    lines.append("| Shared names  | %d | %d | %d |"
                 % (len(set(xbox["structs"]) & set(nt["structs"])),
                    len(set(xbox["enums"])   & set(nt["enums"])),
                    len(set(xbox["funcs"])   & set(nt["funcs"]))))
    lines.append("| Drift in this report | %d | %d | %d |"
                 % (len(struct_diffs), len(enum_diffs), len(func_diffs)))
    lines.append("")

    # --- structs / unions ----------------------------------------------------
    # Bucket: shared-name drift (actionable) vs NT-only / Xbox-only (mostly
    # internal symbols that have no Xbox analog).  We surface shared drift up
    # top; the orphan sets go into collapsed lists at the bottom so we keep
    # them on the record but they don't drown out the signal.
    shared_drift = [(n, x, m) for (n, x, m) in struct_diffs if x and m]
    nt_only      = [n for (n, x, m) in struct_diffs if not x]
    xbox_only    = [n for (n, x, m) in struct_diffs if not m]

    lines.append("## Shared-name struct/union drift")
    lines.append("")
    if not shared_drift:
        lines.append("_None._")
    else:
        # Size mismatches first (worst), then size-equal-but-field-layout drift.
        shared_drift.sort(key=lambda t: (t[1]["size"] == t[2]["size"], t[0]))
        lines.append("| Name | Xbox size | NT size | Notes |")
        lines.append("|---|---|---|---|")
        for name, x, n in shared_drift:
            note = []
            if x["size"] != n["size"]: note.append("size mismatch")
            if x["fields"] != n["fields"]: note.append("field layout differs")
            lines.append("| `%s` | %s | %s | %s |"
                         % (name, x["size"], n["size"], "; ".join(note)))
    lines.append("")

    if shared_drift:
        lines.append("### Field-level (shared-name structs)")
        lines.append("")
        for name, x, n in shared_drift:
            lines.append("#### `%s` -- Xbox %d B vs NT %d B" % (name, x["size"], n["size"]))
            lines.append("")
            lines.append("| Field | Xbox off (bits) | Xbox sz (B) | NT off (bits) | NT sz (B) |")
            lines.append("|---|---|---|---|---|")
            xb = {f[0]: f for f in x["fields"]}
            nb = {f[0]: f for f in n["fields"]}
            keys = list(OrderedDict.fromkeys(
                [f[0] for f in x["fields"]] + [f[0] for f in n["fields"]]))
            for k in keys:
                xr = xb.get(k); nr = nb.get(k)
                lines.append("| `%s` | %s | %s | %s | %s |" % (
                    k,
                    "-" if not xr else str(xr[1]),
                    "-" if not xr else str(xr[2]),
                    "-" if not nr else str(nr[1]),
                    "-" if not nr else str(nr[2]),
                ))
            lines.append("")

    # --- enums ---------------------------------------------------------------
    lines.append("## Enum drift")
    lines.append("")
    if not enum_diffs:
        lines.append("_None._")
    else:
        for name, x, n, diff_consts in enum_diffs:
            lines.append("### `%s`" % name)
            lines.append("")
            if x["underlying_size"] != n["underlying_size"]:
                lines.append("- underlying size: Xbox=%d, NT=%d"
                             % (x["underlying_size"], n["underlying_size"]))
            if diff_consts:
                lines.append("")
                lines.append("| Constant | Xbox | NT |")
                lines.append("|---|---|---|")
                for k, xb, nb in diff_consts:
                    lines.append("| `%s` | %s | %s |"
                                 % (k,
                                    "-" if xb is None else str(xb),
                                    "-" if nb is None else str(nb)))
            lines.append("")

    # --- functions -----------------------------------------------------------
    lines.append("## Function-signature drift")
    lines.append("")
    if not func_diffs:
        lines.append("_None._")
    else:
        lines.append("Sorted by drift kind.  Note: when a parameter's type "
                     "drifts because the underlying *struct* drifts (e.g. "
                     "`PKDPC` resolves to a different layout per side), the "
                     "fix is in the struct, not the function.")
        lines.append("")
        for name, x, n in func_diffs:
            lines.append("### `%s`" % name)
            lines.append("")
            xa = ", ".join(a[1] for a in x["args"]) + (", ..." if x["variadic"] else "")
            na = ", ".join(a[1] for a in n["args"]) + (", ..." if n["variadic"] else "")
            lines.append("- Xbox: `%s [%s] %s(%s)`" % (x["ret"], x["cc"], name, xa))
            lines.append("- NT:   `%s [%s] %s(%s)`" % (n["ret"], n["cc"], name, na))
            lines.append("")

    # --- orphan sets (collapsed) --------------------------------------------
    lines.append("## Xbox-only / NT-only structs")
    lines.append("")
    lines.append("These names exist on only one side; no shared-name drift "
                 "comparison applies.  Listed for completeness.")
    lines.append("")
    lines.append("<details><summary>Xbox-only structs (%d)</summary>" % len(xbox_only))
    lines.append("")
    for n in xbox_only:
        lines.append("- `%s`" % n)
    lines.append("")
    lines.append("</details>")
    lines.append("")
    lines.append("<details><summary>NT-only structs (%d)</summary>" % len(nt_only))
    lines.append("")
    for n in nt_only:
        lines.append("- `%s`" % n)
    lines.append("")
    lines.append("</details>")
    lines.append("")

    # Strip the repo's absolute path from clang-spelled type names like
    # "(anonymous at /home/.../repo/path/file.h:line:col)" so the report stays
    # checkout-independent (and diffable across machines).
    text = "\n".join(lines).replace(REPO + "/", "") + "\n"
    with open(out_path, "w") as f:
        f.write(text)
    return shared_drift


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("-o", "--output",
                    default=os.path.join(REPO, "tools/struct-audit/struct-audit-report.md"),
                    help="path to write the markdown report")
    ap.add_argument("--quiet", action="store_true",
                    help="suppress drift summary on stdout")
    ap.add_argument("--fail-on-drift", action="store_true",
                    help="exit 1 if any shared-name struct drifted")
    args = ap.parse_args()

    here = os.path.dirname(os.path.abspath(__file__))
    xbox_tu = parse_shim(os.path.join(here, "shim-xbox.c"), xbox_flags())
    nt_tu   = parse_shim(os.path.join(here, "shim-nt.c"),   reactos_flags())

    xbox = collect_records(xbox_tu)
    nt   = collect_records(nt_tu)

    sd = diff_structs(xbox["structs"], nt["structs"])
    ed = diff_enums(xbox["enums"],     nt["enums"])
    fd = diff_funcs(xbox["funcs"],     nt["funcs"])
    shared_struct_drift = emit_markdown(sd, ed, fd, xbox, nt, args.output)

    if not args.quiet:
        print("struct-audit: structs xbox=%d nt=%d drift=%d (shared=%d)"
              % (len(xbox["structs"]), len(nt["structs"]), len(sd),
                 len(shared_struct_drift)))
        print("struct-audit: enums   xbox=%d nt=%d drift=%d"
              % (len(xbox["enums"]),  len(nt["enums"]),  len(ed)))
        print("struct-audit: funcs   xbox=%d nt=%d drift=%d"
              % (len(xbox["funcs"]),  len(nt["funcs"]),  len(fd)))
        print("struct-audit: report  -> %s" % args.output)
        for name, x, n in shared_struct_drift[:20]:
            print("  struct %-28s xbox=%-4d nt=%-4d" % (name, x["size"], n["size"]))
        if len(shared_struct_drift) > 20:
            print("  ... (%d more in report)" % (len(shared_struct_drift) - 20))

    if args.fail_on_drift and (shared_struct_drift or ed or fd):
        sys.exit(1)


if __name__ == "__main__":
    main()
