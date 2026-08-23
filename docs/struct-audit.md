# Xbox vs ReactOS struct layout audit

**Status:** snapshot 2026-05-22.

The Xbox kernel and ReactOS share an ancestry, but every kernel-visible struct
has had ten years of independent drift on the NT side (Win2k → Win2k3 SP1 → up).
Whenever a struct's `sizeof` or field-offset disagrees between the Xbox header
([`third_party/nxdk/lib/xboxkrnl/xboxkrnl.h`](../third_party/nxdk/lib/xboxkrnl/xboxkrnl.h))
and the ReactOS NDK
([`sdk/include/xdk/ketypes.h`](../../sdk/include/xdk/ketypes.h),
[`sdk/include/ndk/ketypes.h`](../../sdk/include/ndk/ketypes.h)),
either the kernel writes off the end of a title-allocated buffer (corruption) or
the title reads a field at a different offset (garbage). The KDPC `DpcData`
field at offset 28 was exactly this — invisible until pbkit's second
re-queue of a DPC failed silently and the vblank chain stopped.

This doc is the systematic version of that audit. For each kernel-visible
struct: the Xbox layout, the NT layout, byte-by-byte where they diverge, and
the disposition — *compatible*, *needs shadow*, or *already shadowed*.

The mechanical complement is [`tools/struct-audit/audit.py`](../tools/struct-audit/audit.py):
libclang parses both header trees, enumerates every struct/union/enum/function
under the same name, and emits a diff at
[`tools/struct-audit/struct-audit-report.md`](../tools/struct-audit/struct-audit-report.md).
This curated doc has the narrative; the report has the exhaustive truth.
Run with `--fail-on-drift` to use as a CI gate.

The single biggest pain mitigator is empirical: **titles do not dereference
kernel-struct fields directly**. They allocate the buffer, pass the pointer
to a kernel API, and treat the contents as opaque. (Grep nxdk's pbkit / hal /
nvnetdrv: not one `Dpc->`, `Timer->`, `Event->`. The only direct field touch
is via `RtlInterlocked*` on `LIST_ENTRY`s the kernel set up.) So drift only
bites when **the kernel writes into a title-allocated buffer**, not when the
title reads kernel memory back.

## Summary table

| Struct | Xbox size | NT size | Disposition | Notes |
|---|---|---|---|---|
| `LIST_ENTRY` | 8 | 8 | ✓ identical | |
| `LARGE_INTEGER` | 8 | 8 | ✓ identical | |
| `DISPATCHER_HEADER` | 16 | 16 | ✓ size-compatible | byte 1-3 reinterpreted (see below) |
| `KEVENT` | 16 | 16 | ✓ compatible | |
| `KSEMAPHORE` | 20 | 20 | ✓ compatible | |
| `KMUTANT` | 32 (28+pad) | 32 (30+pad) | ✓ compatible | NT adds `ApcDisable` in tail padding |
| `KTIMER` | 40 | 40 | ✓ compatible | `Dpc` is a pointer — see KDPC |
| `KDPC` | **28** | 28 | ✓ **native** | kernel KDPC is the retail layout on SARCH=xbox (2026-06-10) |
| `KAPC` | 40 | 48 | ⚠ needs shadow if used | NT adds `SpareLong0`+`ApcStateIndex`/`Mode`/`Inserted` tail |
| `KAPC_STATE` | 28 | 28 | ✓ compatible | |
| `KINTERRUPT` | 112 (24+22×4) | ≥60+dispatch | 🛡 **shadowed via init adapter** | titles treat opaquely; `XeKeInitializeInterrupt` glues |
| `KWAIT_BLOCK` | 20 | 20 | ✓ compatible | NT (NTDDI < WIN7) layout matches |
| `KDEVICE_QUEUE` | 12 | 16 | ⚠ only used by IoCreate (kernel-allocated) | |
| `KDEVICE_QUEUE_ENTRY` | 16 | 16 | ✓ compatible | |
| `KTHREAD` | ~108 | wildly larger | 🛡 **inline shadow** | `XeXboxShadow[0x80]` + `XeXboxFs4` appended to ReactOS `KTHREAD`; `XeKeGetCurrentThread` returns `&Thread->XeXboxShadow` (offset 0x28 = TlsData).  Promoted from side-table 2026-05-24. |
| `KPROCESS` | 28 | wildly larger | n/a | titles never allocate or dereference |
| `KQUEUE` | 36 | n/a | n/a | no Xbox `Queue` ordinals yet |
| `KFLOATING_SAVE` | 32 | 32 | ✓ compatible | both are x86 FXSAVE-area-aligned |
| `KSYSTEM_TIME` | 12 | 12 | ✓ compatible | exported by ordinal (USER_SHARED_DATA-style) |
| `KINTERRUPT_MODE` enum | 4 | 4 | ✓ identical | |
| `HARDWARE_PTE` | 4 | 4 | ✓ identical | both standard x86 4 KB PTE |
| `SLIST_HEADER` | 8 | 8 | 🛡 **fastcall thunked** | ord 56/57/58.  Real Xbox is 8-byte union (cxbx-reloaded confirms); nxdk's header writes it as a struct so `sizeof` reports 16, but the kernel only ever writes the first 8 bytes -- safe.  Xbox uses a lockless 1/2-arg FASTCALL ABI; NT'"'"'s `Ex*` SLIST variants thread a separate spinlock pointer.  `XeInterlockedPopEntrySList` / `XeInterlockedPushEntrySList` are CMPXCHG8B thunks; ord 56 forwards to `ExInterlockedFlushSList` directly. |
| `OBJECT_ATTRIBUTES` | 16 | 16 | ⚠ ANSI vs UNICODE name | `Xbe*` Nt creators translate (`XeNtCreateEvent` etc.) |
| `OBJECT_TYPE` | 28 | n/a | ⚠ no static export | ord 240/245/249/256/259 unimplementable; see ordinals map |
| `OBJECT_HEADER` | 24 | 24 | ✓ compatible | titles never see this |
| `RTL_CRITICAL_SECTION` | 28 | n/a | 🛡 **owned by nxbe** | nxbe's minimal recursive lock; ord 277/291/294/306 |
| `ERWLOCK` | 32 | n/a | ⚠ not yet implemented | Xbox-only API; no test title uses it |
| `IRP`, `FILE_OBJECT`, `DEVICE_OBJECT`, `DRIVER_OBJECT` | (large) | (larger) | n/a | I/O not on the triangle path; the file-open ordinals delegate by HANDLE |
| `ETHREAD` | 72 | wildly larger | n/a | not exposed to titles |

Legend: ✓ identical or NT-superset-safe — kernel writes fit inside the
title's allocation. ⚠ mismatched but not currently exercised. 🛡 shadow
adapter exists in [`ntoskrnl/xbe.c`](../../ntoskrnl/xbe.c).

## Detail: each non-trivial struct

### `DISPATCHER_HEADER` — byte 1-3 reinterpretation

| Offset | Xbox name | NT pre-Win7 name (x86) |
|---|---|---|
| 0 | `Type` | `Type` |
| 1 | `Absolute` | `Absolute` ∪ `Abandoned` ∪ `NpxIrql` ∪ `Signalling` (union) |
| 2 | `Size` | `Size` ∪ `Hand` ∪ `ThreadControlFlags` |
| 3 | `Inserted` | `Inserted` ∪ `DebugActive` ∪ `DpcActive` |
| 4 | `SignalState` (LONG) | `SignalState` (LONG) |
| 8 | `WaitListHead` (LIST_ENTRY) | `WaitListHead` (LIST_ENTRY) |

Same 16 bytes, and the *primary* names match. NT uses byte 1 as `NpxIrql` only
on KTHREAD headers, byte 2 as `Hand` only on KTIMER headers, etc. — so when a
title allocates a `KEVENT` and NT writes its `DISPATCHER_HEADER`, the bytes
NT writes are the ones the title would have written. **No bug.**

### `KDPC` — unified into the kernel (the one that bit us)

```
Xbox (28 B):                            NT (32 B):
+0  CSHORT  Type                        +0  UCHAR  Type
+2  BOOLEAN Inserted                    +1  UCHAR  Importance
+3  UCHAR   Padding                     +2  USHORT Number   (volatile)
+4  LIST_ENTRY DpcListEntry             +4  LIST_ENTRY DpcListEntry
+12 PVOID   DeferredRoutine             +12 PVOID  DeferredRoutine
+16 PVOID   DeferredContext             +16 PVOID  DeferredContext
+20 PVOID   SystemArgument1             +20 PVOID  SystemArgument1
+24 PVOID   SystemArgument2             +24 PVOID  SystemArgument2
                                        +28 PVOID  DpcData      <-- the killer
```

The title allocates 28 bytes; NT's `KeInsertQueueDpc` wrote `DpcData` at
+28 to gate re-entry, scribbling past the title's allocation and then
reading stale bytes ("already queued") on re-queue → silent drop.  This
first shipped as a 32-slot shadow table in xbe.c.

Resolved 2026-06-10 by making the retail layout the kernel's own KDPC on
SARCH=xbox (`sdk/include/xdk/ketypes.h`): the queued gate is the
`Inserted` boolean and `ke/dpc.c`'s insert/remove/retire protocol uses it
directly (uniprocessor; importance/targeting fields don't exist and
`KeSetImportanceDpc`/`KeSetTargetProcessorDpc` are no-ops).  Ordinals
107/119/137 and the timer family 97/113/149/150 map straight to the
kernel exports; the shadow table, its 32-DPC cap, and the per-dispatch
slot scan are gone.

### `KTIMER` — same layout; KDPC unification removed the transitive hazard

Both are 40 bytes.  The old hazard was `Timer->Dpc` pointing at a
title-allocated 28-byte KDPC that NT's expiration path would overrun;
with the kernel KDPC now 28 bytes there is nothing to substitute and
`KeSetTimer`/`KeSetTimerEx`/`KeCancelTimer` are direct.

### `KAPC` — drifted; not currently exercised

```
Xbox (40 B):                            NT (48 B):
+0  SHORT  Type                         +0  UCHAR  Type
+2  CHAR   ApcMode                      +1  UCHAR  SpareByte0
+3  UCHAR  Inserted                     +2  UCHAR  Size
+4  PKTHREAD Thread                     +3  UCHAR  SpareByte1
                                        +4  ULONG  SpareLong0
                                        +8  PKTHREAD Thread
+8  LIST_ENTRY ApcListEntry             +12 LIST_ENTRY ApcListEntry
+16 PVOID  KernelRoutine                +20 PVOID  KernelRoutine
+20 PVOID  RundownRoutine               +24 PVOID  RundownRoutine
+24 PVOID  NormalRoutine                +28 PVOID  NormalRoutine
+28 PVOID  NormalContext                +32 PVOID  NormalContext
+32 PVOID  SystemArgument1              +36 PVOID  SystemArgument1
+36 PVOID  SystemArgument2              +40 PVOID  SystemArgument2
                                        +44 CCHAR  ApcStateIndex
                                        +45 KPROCESSOR_MODE ApcMode
                                        +46 BOOLEAN Inserted
```

NT writes 48 bytes; Xbox alloc is 40. NT also has `Thread` at +8 vs Xbox at
+4 — every later field shifts by 4 too. **Hard mismatch.** Not on the
triangle path; no `KeInitializeApc` ordinal is mapped. If a title ever uses
`KeInitializeApc`, prefer the KDPC treatment (unify the kernel struct to
the retail layout) over a shadow table.

### `KINTERRUPT` — drifted; shadowed via init adapter

Xbox KINTERRUPT (112 B) is dominated by `DispatchCode[22]` — 88 bytes of
hand-written trampoline that the ISR enters at the IDT vector. NT KINTERRUPT
embeds `DispatchCode[N]` too, but every preceding field has moved.

Mitigation: `XeKeInitializeInterrupt` (ord 109) re-shapes the call into
the NT 11-arg form; the title's 112-byte buffer is big enough to hold NT's
layout. Title code grep confirms `KINTERRUPT` is fully opaque to titles —
they allocate it, pass it to `KeInitializeInterrupt` / `KeConnectInterrupt`,
and the kernel does the rest.

### `OBJECT_ATTRIBUTES` — ANSI vs UNICODE

```
Xbox (16 B):                            NT WDK (24 B, but Xbox flavor used here):
+0  HANDLE RootDirectory                +0  ULONG  Length
+4  PANSI_STRING ObjectName             +4  HANDLE RootDirectory
+8  ULONG  Attributes                   +8  PUNICODE_STRING ObjectName
+12 PVOID  SecurityDescriptor           +12 ULONG  Attributes
                                        +16 PVOID  SecurityDescriptor
                                        +20 PVOID  SecurityQualityOfService
```

Field reordering **and** ANSI vs UNICODE name string. Every `Xbe*` `Nt`
creator (Event/File/Mutant/Directory/SymbolicLink) translates: stack-allocate
an NT `OBJECT_ATTRIBUTES` + a `UNICODE_STRING` over the ANSI buffer (with
RtlAnsiStringToUnicodeString) before calling the NT primitive. See `ntoskrnl/xbe.c`
lines 376-410.

### `KTHREAD` — fully shadowed

Xbox `KTHREAD` is ~108 bytes; NT's is hundreds. The Xbox kernel exposes
`KeGetCurrentThread() → PKTHREAD` and stores per-thread state (`TlsData` at
a known offset, etc.) inside the struct. ReactOS's `KeGetCurrentThread`
returns a pointer into its own thread layout.

Mitigation: `XeKeGetCurrentThread` (ord 104) returns a 256-byte
Xbox-shaped shadow keyed by the underlying ReactOS thread pointer; the
shadow is populated at `XePsCreateSystemThreadEx` (ord 255) time. Titles
reading `Thread->TlsData` get the right pointer; everything else in the
shadow is zero.

## What the audit changes today

Nothing in the triangle path: every struct currently exercised is either
size-compatible or already shadowed. The shadow-adapter pattern (KDPC,
KINTERRUPT, KTHREAD, OBJECT_ATTRIBUTES, RTL_CRITICAL_SECTION) has been the
right answer each time and is locked in.

What it teaches: when we map a new Xbox ordinal, **check this table first**.
If the struct it operates on is "needs shadow if used", build the shadow at
that time. Order to keep in mind:

- `KAPC` first if/when a title uses `KeInitializeApc` / `NtQueueApcThread`.
- `KTIMER`'s Dpc-routing when we map `KeSetTimer` / `KeSetTimerEx`.
- `ERWLOCK` when an Xbox-only `RtlInitializeReadWriteLock` import shows up.
- `OBJECT_TYPE` static exports (ord 240/245/249/256/259) — needs boot-time
  export-directory fixup, not adaptable here.

## How regressions are caught

The libclang audit (above) is the regression mechanism. Re-run
[`tools/struct-audit/audit.py`](../tools/struct-audit/audit.py) after any
ReactOS uprev; pass `--fail-on-drift` to gate. If `sizeof(KDPC)` becomes 36
or a new SAL annotation flips a function signature, the diff surfaces in
[`struct-audit-report.md`](../tools/struct-audit/struct-audit-report.md)
before a test title can hit the corruption.

What the tool **also caught**, beyond the structs already shadowed:

- **`SLIST_HEADER` 16 vs 8** — nxdk's `xboxdef.h` defines it as a
  `struct { ULONGLONG; struct { ... }; }` (two consecutive members) where
  NT uses a `union`. The size disagrees with the canonical `SLIST_HEADER`
  layout. Titles that use `ExInterlockedPopEntrySList` etc. would corrupt
  their second 8 bytes. Triangle path doesn't use SLIST.
- **`ExAllocatePool` 1-arg vs 2-arg**, **`ExAllocatePoolWithTag` 2-arg vs
  3-arg** — Xbox dropped the `POOL_TYPE` parameter. Not currently mapped;
  needs a `Xbe*` adapter when a title imports them.
- **`ExfInterlockedInsert{Head,Tail}List`, `ExfInterlockedRemoveHeadList`,
  `ExInterlockedAddLargeInteger`** — NT versions add a trailing
  `PKSPIN_LOCK` parameter the Xbox versions don't take. Not currently
  mapped.
- **`HalGetInterruptVector`** — already shadowed.
- **`KWAIT_REASON` enum extended** — NT renumbered `MaximumWaitReason` from
  27 to 37 and added 10+ new reasons. Internal-only; titles pass
  `Executive` (0) so no ABI impact.
- **`FILE_INFORMATION_CLASS` / `FSINFOCLASS` enums extended** — NT added
  several entries; `FileMaximumInformation` / `FileFsMaximumInformation`
  drift. Only matters if a title walks to the boundary value.
- **`KPRCB` tail (`fs:[0x20]` → `+0x24C`/`+0x250`)** — Halo 2 worker
  threads read PRCB+0x250 expecting NULL.  cxbx-reloaded's Xbox `KPRCB`
  is unmapped past +0x34 (`Unknown[0x224]`, total 0x258), so the
  community doesn't know the field either; it sits 8 bytes from the
  struct's end.  On NT's layout that offset lands inside
  `ProcessorState.ContextFrame.ExtendedRegisters` (the boot FXSAVE
  image), which interrupt activity repopulates.  A shadow-PRCB view is
  not separable because title threads share the NT KPCR and `Prcb`
  lives at the same +0x20 in both layouts, so the standing workaround
  zeroes the two slots on every trap entry/exit for title threads
  (`trap_x.h`).  NT-side collateral: those bytes are only consumed by
  `KiSaveProcessorState` for bugcheck snapshots.  Revisit if the field
  is ever identified (candidates: per-thread debug/FPU-setup pointer).
