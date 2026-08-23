# Building and running nxkrnl

Practical recipes for an everyday build/test loop. See
[`design.md`](design.md) for the project goal,
[`debugging.md`](debugging.md) for diagnostic workflows, and
[`footguns.md`](footguns.md) for changes that look safe but aren't.

## Build trees

Two sibling build directories at the repo root are conventional:

| Path                | `CMAKE_BUILD_TYPE` | `DBG` | Use case                                       |
| ------------------- | ------------------ | ----- | ---------------------------------------------- |
| `build/`            | `Release`          | `0`   | clean release boots, retail-style title runs   |
| `build-dbg/`        | `Release` + DBG=1  | `1`   | kernel DbgPrint / DPRINT1 traces over the LPC  |

The release build silences `DbgPrint` / `DPRINT1`; use `build-dbg/` when you
need those `(/foo/bar.c:NNN) ...` lines over the LPC port.

Release (non-DBG) trees build with LTO by default (`LTCG=TRUE`,
-52 KB resident -- see [`footprint.md`](footprint.md)); DBG trees keep
it off for faster rebuilds.  Opt out with `-DLTCG=FALSE`.  An existing
build directory keeps its cached value until you reconfigure with
`-ULTCG`.

### One-time configure (already done in this tree)

If you ever need to (re)create either directory, run from the repo root:

```bash
# Release tree
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_TOOLCHAIN_FILE=toolchain-gcc.cmake .

# Debug-logging tree
cmake -B build-dbg -G Ninja -DCMAKE_BUILD_TYPE=Release \
      -DDBG=1 -DKDBG=FALSE \
      -DCMAKE_TOOLCHAIN_FILE=toolchain-gcc.cmake .
```

### `NXK_DBG_RETAIL_MAP` (off by default)

The DBG kernel is based high (`0x84000000`) and normally runs with the
full 128 MB free, so it does **not** reflect the release 64 MB memory
pressure.  `-DNXK_DBG_RETAIL_MAP=ON` makes the DBG build reserve the
release low footprint (PA `0x10000`-`0x61000`) so the title sees the same
free memory as release -- a pressure harness with full serial/asserts.

Keep it **OFF** for everyday Halo debugging: under the retail map Halo's
intro green-screens because the resident kernel (77 pages) is still ~18
pages over the budget Halo's intro needs (~59) -- the footprint gap, see
[`memory-map.md`](memory-map.md) §3.  Only turn it ON for footprint /
memory-pressure work, and don't expect titles to boot under it yet.

### Rebuild the flash image

```bash
cmake --build build-dbg --target flash
```

`flash` is the default target and depends on `startup` (the `boot/nxldr`
loader) and `ntoskrnl`.  The output is a 1 MiB flash image at
`build-dbg/flash.bin` with the xz-compressed kernel injected into a
hole in the loader image -- no third-party bootloader required.

If you only need the kernel image, use
`cmake --build build-dbg --target ntoskrnl`.  To restitch the flash
without rebuilding, run `boot/nxldr/gen-flash-bin.py` directly.

Sanity-check that your change actually landed in the flash:

```bash
strings build-dbg/flash.bin | grep <some-string-from-your-change>
```

## Running xemu

Use `tools/run-xemu`. It wraps the kernel-irqchip / smbus-storage /
flash / DVD / HDD / serial QEMU invocation and writes a per-kernel toml.

### Environment variables

Set these in your shell rc (or wrap with a one-shot prefix on the
command line). The script `set -u`s every one of them, so a missing
variable fails fast with a clear error.

| Variable          | What                                         | Notes                                     |
| ----------------- | -------------------------------------------- | ----------------------------------------- |
| `NXKRNL_XEMU`     | path to the locally-built `qemu-system-i386` | NOT the prebuilt `dist/xemu` — that asserts on title nvnet writes |
| `NXKRNL_ROM_DIR`  | dir with `mcpx.bin`, `eeprom.bin` (and optional `bios.bin` for `--official`) | |
| `NXKRNL_HDD`      | xemu's reference Xbox HDD (qcow2)            | `-snapshot` is implicit, so disk writes never persist |

### Boot the nxkrnl flash

```bash
cd $REPO   # the ReactOS root, where tools/run-xemu lives

tools/run-xemu \
  --flash build-dbg/flash.bin \
  --dvd path/to/title.iso \
  --serial file:/tmp/title.log
```

Notes:

- `--flash <path>` overrides the default of `build/flash.bin`.
- `--dvd <iso>` attaches an ISO as the optical drive.
- `--serial <target>` attaches the LPC debug UART (e.g.
  `file:/tmp/foo.log`, `stdio`, `tcp:127.0.0.1:5558`). Required for
  capturing kernel debug output.
- `--gdb` exposes a GDB stub on `tcp::1234` and starts halted.
- `--official` switches to the retail Xbox flash (`bios.bin`) for
  comparison runs.

### Bounded-time runs

xemu doesn't exit on its own. Wrap with `timeout` to cap boot duration:

```bash
timeout -k 5 60 tools/run-xemu ... --serial file:/tmp/log
```

Exit code `124` means the timeout fired — the kernel ran to the
deadline without crashing, which is the typical outcome for a
debugging session. Any other non-zero exit means xemu crashed.

## A typical iteration

```bash
# 1. Edit kernel source.
$EDITOR ntoskrnl/xb/xbe.c

# 2. Rebuild the flash image (the default target: pulls in nxldr + ntoskrnl).
cmake --build build-dbg

# 3. Verify marker landed in flash.  Driver compile errors are non-fatal
#    at the link step and leave a stale flash.bin -- always check.  See
#    footguns.md.
strings build-dbg/flash.bin | grep my-new-string

# 4. Boot, capture log.
timeout -k 5 60 tools/run-xemu \
  --flash build-dbg/flash.bin \
  --dvd path/to/title.iso \
  --serial file:/tmp/log

# 5. Inspect (drop the high-volume device noise).
grep -vE 'ohci|pci_irq|pic1|frame_boundary' /tmp/log | less
```

## Comment style for our additions

Brief comments, explaining *why* something is the way it is when that's
not obvious from the code.  No `nxkrnl:` / `xbe:` prefixes, no
title-specific references ("Halo 2 needs this") in vendored files, no
ordinal numbers in narrative text, no cross-references to memory notes
or design-doc sections that are likely to be renamed.

The rationale is reviewability: any vendored file we touched should
have a small, surgical diff that reads like an upstream patch.
Title-specific findings belong in commit messages and `footguns.md`,
not in the source itself.

## Tests

```sh
NXKRNL_XEMU=/path/to/xemu/build/qemu-system-i386 \
NXKRNL_HDD=/path/to/xbox_hdd.qcow2 \
    tools/api-regression-run
```

The suite boots an XBE that exercises a broad set of Mm/Ke/Ex/Ob/Io
surfaces and grades behavior.  It catches behavior regressions; it doesn't catch
perf-only regressions (DMA-degrading-to-PIO, allocator-going-quadratic,
that kind of thing) -- for those, also boot a real title.  See
[`footguns.md`](footguns.md) for examples.

`NXKRNL_BUILD_DIR` points the suite at a different build tree, and
`--trace <log>` captures an execution trace -- together they drive
line-coverage measurement, see [`coverage.md`](coverage.md).
