# struct-audit

Compare every named struct/union, enum, and function the Xbox kernel header
and the ReactOS NT headers (`sdk/include/...`) declare under the same name.

Re-running the audit needs a local clone of nxdk (the kernel header lives
at `lib/xboxkrnl/xboxkrnl.h`); set `NXDK_DIR` to point at it before
invoking `audit.py`.  We don't vendor nxdk in-tree because the build
itself only needs the much smaller `xboxkrnl.exe.def` (committed at
`ntoskrnl/xb/xboxkrnl.exe.def`).

## Why

ABI drift between Xbox and NT bites silently. The `KDPC` story is the
canonical example: NT added `DpcData` at offset 28 some time after the Xbox
branch forked, so `KeInsertQueueDpc` scribbles 4 bytes past a title's
28-byte buffer **and** reads stale memory back on the next call — pbkit's
DPC re-queue starts dropping silently and the vblank chain hangs. We
caught it by accident after a long instrumentation session.

This tool catches every such drift mechanically:

- **Structs / unions** — `sizeof` mismatch and field-by-field offset/size diff.
- **Enums** — constants that disagree (different value, missing on one side,
  range extended).
- **Function signatures** — return type, calling convention, argument count,
  argument types, variadic-ness.

It does **not** know whether a given drift is exploitable. That's the
human's job; see [`docs/struct-audit.md`](../../docs/struct-audit.md) for
the curated narrative.

## What it produces

`tools/struct-audit/struct-audit-report.md` (checked in). The first three
sections are the actionable diff (shared names that disagree); the
collapsed `<details>` sections at the bottom list orphan names on each
side (mostly NT-internal symbols that have no Xbox analog).

## How it works

`audit.py` invokes libclang twice:

1. `shim-xbox.c` — `#include <xboxkrnl/xboxkrnl.h>` with the nxdk include
   path. Yields the Xbox ABI.
2. `shim-nt.c` — `#include <ntifs.h>` with the *exact* DEFINES and
   INCLUDES that CMake passes to `drivers/nxbe/nxbe.c` (scraped from
   `build/reactos-build/build.ninja`). Yields the NT ABI as nxbe sees it.

For each TU it walks every `STRUCT_DECL` / `UNION_DECL` / `ENUM_DECL` /
`FUNCTION_DECL`, records the layout, and diffs the two maps.

## Running

```sh
tools/struct-audit/audit.py
```

(No arguments needed; writes the report to `struct-audit-report.md`
next to the script.)

Add `--fail-on-drift` to use as a CI gate — any new disagreement exits 1.

## Dependencies

- `clang.cindex` python bindings (Arch: `python-clang`).
- `libclang.so` (Arch: `clang`).
- A completed ReactOS build (`tools/build-reactos`) so `build.ninja`
  exists for the DEFINES/INCLUDES scrape.

## Caveats

- Anonymous tags are not name-keyed, so they appear under their typedef'd
  parent if any, and are otherwise skipped.
- Function-signature drift cascades through struct drift (a function
  whose parameter is a drifted struct will appear in *both* sections);
  fix the struct first.
- The audit can't catch macros, calling-convention attributes encoded
  via `#define`, or layout differences that only manifest at higher
  optimization levels. Those are still the human's job.
