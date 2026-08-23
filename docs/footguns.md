# Footguns

Things that look safe to change but aren't.  Every entry here cost a
session to discover; the goal is to make the next person not pay the
same cost twice.

## "Stubs" that aren't actually stubs

`ntoskrnl/xb/pnpstubs.c` and the other `xb/*-stubs.c` files mix two
different kinds of code:

1. **Real stubs.**  Empty bodies, return `STATUS_NOT_IMPLEMENTED` or
   the documented "no-op" value.  Deletable when the only callers are
   gated out.
2. **Trampolines disguised as stubs.**  The body does real work --
   delegating to another subsystem, providing a default the caller
   actually depends on.  Look like stubs because the body is short.

**Before deleting anything from a `*-stubs.c` file, read the function's
own comment.**  If it warns about a downside ("returning NULL strands X
in Y mode"), the stub is load-bearing.  If the caller's "handle the
absent case" path is a *fallback to a slow mode* rather than an
*alternate fast path*, the stub is load-bearing.

Three regressions caught by bisect, each cost a session:

| Function | What deleting it did |
|---|---|
| `IoGetDmaAdapter` | Delegated to `HalGetDmaAdapter`; NULL strands pciidex in PIO mode.  Halo 2 lost ~30% framerate as soon as the title started streaming map data off the DVD. |
| `IoInvalidateDeviceRelations` PCI BusRelations | Looked redundant with the static device tree; gating it broke ATAPI DVD enumeration.  Coverage-driven `in_asm` traces miss one-shot init paths. |
| `sdk/lib/rtl/error.c` NTSTATUS->Win32 table | Tables looked dead because the suite passed without them; Halo 2 "damaged content" came from a missing `STATUS_OBJECT_NAME_NOT_FOUND` -> `ERROR_FILE_NOT_FOUND` mapping. |

## Extending Mm: replace ARM3, don't shadow it in place

The Xbox Mm *is* a from-scratch rewrite (`ntoskrnl/xb/mm/`: `pool.c`,
`vm.c`, `fault.c`, `cache.c`, `contig.c`, `pagesupply.c`, `sysva.c`). It
owns its own VA windows and routes each allocation by *source*,
displacing ARM3's pool / VM / fault / cache; only ARM3's boot bring-up
still runs. So the question isn't *whether* to rewrite Mm — it's *how*.

An earlier attempt replaced ARM3 **in place** — a function-level shadow
that swapped individual routines (PFN → Pool → VAD → MDL → Fault) one at
a time behind the existing ARM3 surface. It was reverted the same day:
ARM3's internals call back into pool/MDL at IRQLs and in contexts the
shadows don't respect, and two stateful allocators sharing one address
space diverge immediately after the first call.

The lesson for any further Mm work: follow nxmm's **disjoint** model —
give the new allocator its own VA window and route to it by allocation
source, leaving ARM3's routines (and its bring-up) untouched — rather
than shadowing ARM3's allocators in place.

## The stub kernel header's trailing section name is load-bearing

While the kernel runs high (`0x84000000`) with a minimal stub PE header
standing in at the retail base `0x80010000` (`XeStubKernelHeader`), the
stub's **last section is named `"TEXT"`, and that name is deliberate.**

Some titles introspect the kernel header. Halo 2 reads it at
`0x80010000`, walks to the last section header, and **if that section is
named `INIT`** builds a ring-0 code-segment descriptor limited to that
section's VA and swaps it into GDT entry 8 (the ring-0 CS) — a
*narrowing* of the flat 4 GB CS that reads as anti-tamper hardening. It
uses the last section purely as a "top of kernel image" landmark; the
content is irrelevant, so retail discarding INIT after boot doesn't
matter.

If the stub's last section were named `INIT`, Halo 2 would compute a CS
limit from the *stub's* VA (~`0x8005_0000`) and install it — and the
next ring-0 instruction in our real kernel at `0x84000000` is above that
limit → **#GP → crash.** The `"TEXT"` name makes the `INIT` compare fail
so the title skips the swap and CS stays flat.

**When the kernel moves to a single low image at `0x80010000`** (titles
then introspect the real header), either keep the trailing section **not**
named `INIT`, or make `INIT` genuinely the last section with all code
below its VA (the retail-faithful arrangement — prefer this). Either way,
add a boot/api-regression check asserting the post-launch CS limit still
covers the kernel, so a future layout change can't silently reintroduce
the `#GP`. (Findings recovered clean-room from Halo 2's own `default.xbe`
with angr; no `bios.bin`.)

A related, inert poke: Halo 2 also hooks `IdexChannelObject` (a kernel
DATA export) to drive its own low-level DVD streaming. That ordinal is
backed by a zeroed DATA scaffold here, not a live channel object our
ATAPI stack drives, so the hook writes into a dead blob and the title
falls back to normal `NtReadFile` → FATX → ATAPI I/O (which works).
Lighting it up is a streaming-perf optimization, not a correctness fix —
scope it only if streaming perf becomes a target.

## flash.bin staleness

The kernel and most drivers link *into* `ntoskrnl/xboxkrnl.exe`, which
gets embedded into `build/flash.bin` by `gen-flash-bin.py`.  A driver
compile error often emits a non-fatal warning at link time and leaves
**a previous flash.bin** sitting in the build tree.  You change kernel
code, boot, see the old behavior, and conclude "my change had no
effect."

**The cure:** after every kernel-side change, before testing, run

```sh
strings build/flash.bin | grep <some-marker-from-your-change>
```

If the marker isn't there, the rebuild didn't take.  Look back through
the `cmake --build` output for compile warnings on the file you
touched.

## api-regression is correctness, not performance

`tests/xbe/api-regression/` (driven by `tools/api-regression-run`)
boots an XBE that exercises a broad set of Mm/Ke/Ex/Ob/Io surfaces and
grades their behavior.  It catches behavior regressions like
"`MmAllocateContiguousMemory` started returning misaligned addresses."

It does **not** catch perf-only regressions.  The `IoGetDmaAdapter`
deletion above passed the full suite -- correctness was fine, DMA just
degraded to PIO and Halo 2 lost 30% framerate.

For any change that touches IO paths, IRQ handling, DPC scheduling, or
allocator hot paths, run the suite *and* boot a title that exercises
the affected path.  See [`debugging.md`](debugging.md) for the title
boot recipe.

## Wait/APC ordinal semantics are title-visible -- the alertability trim

Halo took ~5 s per 32 MB cache-file create (~25 s extra to the loading
screen).  Bisect landed on the `-512 B` trim that folded
`KiCheckAlertability`'s usermode arms to KernelMode-only on the premise
that "all callers pass KernelMode on Xbox."  The premise is wrong:
`KeWaitForSingleObject`/`Multiple`/`KeDelayExecutionThread` are
**exported ordinals** -- titles call them with `WaitMode = UserMode`,
`Alertable = TRUE`, and user-APC delivery during alertable waits is how
overlapped-IO completion routines run on Xbox.  With the arms dropped,
a queued user APC could no longer interrupt the wait; the title's
writer thread trickled at ~one IO per clock tick (reverted in the
commit that cites this entry).

Debugging lessons from chasing it the hard way first:

- The symptom profile of a *lost wakeup*: gdb EIP sampling ~97% idle,
  qemu `info irq` shows the device IRQ near-silent (IOs were
  submission-starved, not slow), per-IO latency quantized to the clock
  period.  An idle-dominated histogram during a "slow" phase means
  latency, not throughput -- look for who failed to wake, not what is
  burning cycles.
- api-regression passed in full throughout: it never issues an alertable
  UserMode wait with a pending user APC.  A wait/APC-semantics test
  case would have caught this -- worth adding.
- Time only release kernels and verify the xemu binary; treat DBG
  serial milestones as ordering, not durations.
- A `-monitor unix:...` shim on run-xemu (`info irq`, `info pic`) and
  gdb EIP sampling against the stub are cheap, decisive probes.

## Useful tooling

- `tools/dead-objs.py` -- find .c files that compile but contribute zero
  symbols to the image.
- `tools/kernel-size.py` -- per-section/per-subsystem byte breakdown of
  `xboxkrnl.exe`.
- `tools/cov-analyze.py` -- consume an `--trace` xemu log into a coverage
  delta.
- `tools/api-regression-run` -- the correctness gate.
- `tools/dump-xbe.py` -- XBE inspector: header, sections, TLS, the
  ordinals it imports.
