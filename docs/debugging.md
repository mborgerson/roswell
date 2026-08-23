# Debugging recipes

Practical workflows for the bugs you'll actually hit.

## Where the bug is

Most bugs fall into one of three buckets.  Picking the right one early
saves hours.

| Symptom | Likely location | First move |
|---|---|---|
| Boot stops before the XBE starts (no title window, no entry-point trace) | Kernel side: init, paging, drivers | DBG kernel, serial log; look for the last successful initcheck |
| Boot reaches the title but the title bugchecks or hangs | Title <-> kernel ABI surface | DBG kernel + angr on the title's XBE |
| Title runs but behaves wrong (FPS drop, missing UI, audio dropouts, ...) | Behavior / perf regression in a kernel surface the suite doesn't cover | Bisect, then run the affected title at HEAD and the suspected commit |

## DBG kernel

`build-dbg/` is configured as `-DDBG=1 -DKDBG=FALSE`.  Pass
`--serial file:/tmp/log` to `tools/run-xemu` to capture `DbgPrint` /
`DPRINT1` output over the LPC UART.  Format is
`(/foo/bar.c:NNN) <message>`.

For interactive WinDbg: build `build-dbg/`, boot with
`--serial tcp::5555,server,nowait`, then on the host
`windbg -k com:port=5555,baud=115200`.  The kd64 protocol is preserved
in DBG builds.

## Bisecting a regression

`git bisect` works.  The repo's flash artifacts mean a clean cmake
build at each midpoint is required (the build/ tree's content can
desync from the source state when checking out a different commit):

```sh
git bisect start
git bisect bad <broken-ref>
git bisect good <known-good-ref>

# at each midpoint:
rm -rf build && cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DENABLE_CCACHE=ON -DCMAKE_TOOLCHAIN_FILE=toolchain-gcc.cmake .
cmake --build build
# boot the title, observe symptom
git bisect good   # or: git bisect bad
```

ccache makes each step ~10x faster after the first.  Use `git bisect
skip` for commits that don't build or that hang for unrelated reasons
-- don't burn a bisect step trying to fix them.

## Title-side bugs: ordinal strace first

When the symptom is "title runs for a while, then bugchecks / hangs",
the fastest narrowing is **strace on the kernel-ordinal interface**:
every Xbox ordinal call from the title gets logged with the title's
return address (the call site in the title's `.text`) before being
forwarded to the real implementation.

Enable:

```c
// ntoskrnl/xb/strace.h
#define XB_STRACE 1   // was 0
```

Rebuild the kernel (DBG=1 to keep DbgPrint live), boot the title with
`--serial file:/tmp/log`, reproduce the bug, then look at the tail of
the log.  Output lines look like:

```
strace[ord=0x009 caller=0x000124f8]: NtClose(0x60000010)
strace[ord=0x0a3 caller=0x00033210]: RtlAnsiStringToUnicodeString(...)
```

The `caller=` field is the title's `.text` address right after the
ordinal call -- feed it to angr (below) to recover the function the
title was in.

**Gotcha:** DATA ordinals (`HalDiskCachePartitionCount` etc.) must not
be wrapped -- titles read those as values, not function pointers.
`ntoskrnl/xb/gen-exports.py` handles the skip list; if you add a new
DATA ordinal to `xb/ordinals.map`, mark it `DATA`.  DbgPrint itself is
ordinal 8 (i.e. it reaches the kernel through a trampoline), so the
implementation uses a per-CPU re-entrancy flag to avoid infinite
recursion; this is invisible to callers.

Cost: roughly 24 bytes of trampoline per ordinal, allocated from NP
pool at boot, plus one DbgPrint per ordinal call.  Don't ship with it
on; it's a development knob.

## Title-side bugs: use angr second

When a title bugchecks, AVs, or hangs after the entry point, the bug
is usually in the title's interaction with the kernel API surface --
title-side code reading a kernel-supplied value and crashing on it.
The fastest way to find which call is involved is to **statically RE
the title's own XBE** and let angr trace the CFG to the symbol that
faulted.

```sh
# In a venv that has angr installed (`pip install angr` or the angr-dev
# editable install -- the latter if you want to hack on angr itself).
. /path/to/angr-venv/bin/activate
python3
```

```python
import angr, cle
proj = angr.Project('path/to/default.xbe', auto_load_libs=False)
# CLE has a native XBE backend (Backend('xbe')), so the import sees the
# title image at its declared base.
cfg = proj.analyses.CFGFast()
# Now you can ask "what function calls kernel ordinal N?", "where is
# the address that EIP=0x1234abcd points to?", etc.
```

The clean-room rule applies: angr-driven analysis of the *title* is
fine -- that's the title author's own binary, we already have
permission to run it.  The forbidden case is disassembling `bios.bin`.

**Don't reach for byte-pattern grep against the .text first.**  x86 is
variable-length, so any naive offset arithmetic desyncs through the
first non-aligned instruction and misses indirect writes entirely.
angr's CFG resolves those for free.

## Kernel-side bugs: serial + `in_asm` trace

For "where in the kernel does this hang":

```sh
tools/run-xemu --serial file:/tmp/serial.log --trace /tmp/trace.log \
    --dvd path/to/title.iso
```

`--trace` enables qemu's `in_asm,nochain` so every translated block is
logged once.  The output is multi-MB and slow; kill xemu when the
workload of interest finishes.

Feed the trace to `tools/cov-analyze.py` to get a "what executed" map.
Diff two traces (one good, one bad) to find the divergence.  See
`tools/cov-analyze.py --help`.

Run that trace **once you've narrowed the time window** (between which
DbgPrint and which) -- not over a full boot, the log gets unwieldy.

## Verifying your kernel change took

Driver compile errors in the kernel image are non-fatal at the link
step; the link succeeds against the prior driver build artifacts and
`build/flash.bin` reflects an old kernel.  After every kernel change:

```sh
strings build/flash.bin | grep <distinctive-marker-from-your-change>
```

If the marker isn't in the flash, your rebuild didn't take -- look
back through the `cmake --build` output for compile warnings, fix
those first.  See [`footguns.md`](footguns.md#flashbin-staleness) for
the full story.

## Running titles

`tools/run-xemu` boots `build/flash.bin` by default:

```sh
NXKRNL_XEMU=/path/to/xemu/build/qemu-system-i386 \
NXKRNL_HDD=/path/to/xbox_hdd.qcow2 \
    tools/run-xemu --dvd path/to/title.iso --serial file:/tmp/log
```

`xbox_hdd.qcow2` is the pre-formatted FATX disk from xemu-dashboard
(see [`building.md`](building.md)); `xemu` should be the locally-built
`qemu-system-i386` -- the prebuilt `dist/xemu` asserts on title nvnet
writes and is not usable for kernel development.

For test boots that need to terminate cleanly, wrap with `timeout`:

```sh
timeout -k 5 60 tools/run-xemu --dvd ... --serial file:/tmp/log
```

Exit code 124 = the timeout fired; that's the normal exit path for any
boot test that doesn't issue an SMC shutdown.  See `tools/run-xemu --help`
for the full flag set.
