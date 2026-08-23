# The Xbox memory map: retail vs nxkrnl

**Status:** living reference. Goal: make nxkrnl's runtime memory map
compatible with the retail Xbox kernel's. This doc records what we know
about *both* maps in matching detail. Where a fact is unverified it is
marked **UNKNOWN** — do not guess; investigate in a follow-up.

Much of the retail data here was measured with two probe XBEs that run on
both kernels (black-box, never bios.bin disassembly):
- `tests/xbe/krnlsect` — walks the running kernel's PE header at the
  fixed base `0x80010000` (section names, sizes, DISCARDABLE flags).
- `tests/xbe/meminv` — `MmQueryStatistics` + a deduped walk of the
  self-mapped page tables (per-VA-region frame census + max-contig).

## Sources (clean-room)

- **cxbx-reloaded** `src/common/AddressRanges.h`, `core/kernel/init/CxbxKrnl.h`
  — region constants (GPL-2). Vendored at `vendor/cxbx-reloaded`.
- **xemu monitor + probe XBEs** — black-box observation of the running
  official kernel (`info mem/pci/registers/mtrr/tlb`, and the two probes
  above). Explicitly permitted; never disassembly of `bios.bin`.

---

# 1. The retail kernel

## 1.1 Image — base and sections (measured, `krnlsect`, 64 MB)

Kernel image base **VA `0x80010000`** (PA `0x10000`), KSEG0 alias of the
low physical pages. `SizeOfImage = 636,160` (156 pages).

| Section | Bytes | Pages | Resident? | Notes |
|---|---|---|---|---|
| `.text` | 153,136 | 38 | resident | all kernel code |
| `.data` | 21,964 | 6 | resident | incl. read-only data; no separate `.rdata` |
| `STICKY` | 6,544 | 2 | resident | **UNKNOWN contents** — likely state that survives quick-reboot; investigate |
| `IDEXPRDT` | 264 | 1 | resident | pre-allocated IDE DMA PRD table (bus-master descriptors) |
| `INIT` | 453,082 | 111 | **discarded** | boot-only; freed after init |
| **resident total** | **192,512** | **47** | | + the 1 header page |
| discarded total | 454,656 | 111 | | |

Retail has **no `.bss` section** — zero-init/uninitialized data lives in
`.data`. Its **boot/idle and #DF/NMI stacks are static in `.data`** (in the
image; measured, §1.6 / §2.7); only *runtime-created* thread stacks are
allocated dynamically (system-VA `0xD0000000`+). Retail front-loads **111
pages into discardable `INIT`** (4.6× what nxkrnl discards).

## 1.2 Runtime page census (measured, `meminv`, clean XBE, 64 MB)

`MmQueryStatistics`: `total=16384 avail=16217 pool=5 stack=19 cache=16
image=29(title)`. So at a clean baseline the **entire retail kernel
runtime footprint is ~167 pages (≈484 KB)** incl. the 46-page probe XBE,
i.e. **kernel-side ≈121 pages**.

Deduped frame census by VA region:
- **KSEG0 RAM identity**: mapped with **4 KB pages, sparsely** (only
  pages actually touched — `map4k kseg≈79`); **no PSE large pages** for
  KSEG0. (Retail does *not* PSE-map KSEG0.)
- **WC aperture** `0xF0000000`: 32 × 4 MB PSE pages.
- **MMIO** `0xF8000000+`: 31 × 4 MB PSE pages.
- **`0x84000000`–`0xB0000000`** (where nxkrnl's ARM3 System-PTE/paged
  pool live): **empty on retail** — 0 frames.
- **`0xB0000000`** (nxkrnl shadow PFN db): **empty on retail**.
- **`0xD0000000`+** (system memory): ~40 frames.
- **kernel image + contig** (`kern` bucket, 4 K-mapped): ~63 frames.
- **page tables**: ~18 frames.
- **Max single contiguous allocation: ≥60 MB** (measured).

## 1.3 Virtual address map (from the running kernel)

| VA range | Size | Purpose |
|---|---|---|
| `0x00000000`–`0x0000FFFF` | 64 KB | NULL guard |
| `0x00010000`–`0x03FFFFFF` | ~64 MB | **Title** (XBE at `0x00010000`), low RAM window |
| `0x80000000`–`0x83FFFFFF` | 64 MB | **KSEG0 / contiguous** — RAM, cached; **kernel at `0x80010000`**; `MmAllocateContiguousMemory` returns here |
| `0xC0000000`–`0xC03FFFFF` | 4 MB | page tables (self-map); PD at `0xC0300000` |
| `0xD0000000`–`0xEFFFFFFF` | 512 MB | system memory (`MmAllocateSystemMemory`), demand-mapped |
| `0xF0000000`–`0xF7FFFFFF` | 128 MB | write-combined (GPU) — PSE+PWT → PAT1 = WC |
| `0xF8000000`–`0xFFBFFFFF` | ~124 MB | uncached device MMIO |
| `0xFD000000`–`0xFDFFFFFF` | 16 MB | NV2A GPU MMIO (PRAMIN at `0xFD700000`) |
| `0xFE800000`–`0xFE87FFFF` | 512 KB | APU (audio) |
| `0xFEF00000`–`0xFEF003FF` | 1 KB | nForce NIC |
| `0xFF000000`–`0xFFFFFFFF` | 16 MB | flash ROM (4 MB ×4 mirror) |
| `0xFFDF0000`–`0xFFDFFFFF` | 64 KB | KI_USER_SHARED_DATA + KPCR/KPRCB (KIP0PCRADDRESS `0xFFDFF000`) |

## 1.4 Paging (from `info registers`)

- `CR3 = 0x0000F000` — page directory at **PA `0xF000`**, one fixed CR3
  for the system's life (no per-process address spaces).
- `CR4`: **PSE set, PAE clear** — classic non-PAE 2-level i386 paging,
  4 KB + 4 MB pages, single shared address space.

## 1.5 Caching — MTRR / PAT (from `info mtrr`)

| MTRR | Range | Type |
|---|---|---|
| Var0 | `0x00000000`–`0x03FFFFFF` (64 MB) | WB (RAM) |
| Var1 | `0xFFF80000`–`0xFFFFFFFF` (512 KB) | WP (flash) |
| default | everything else | UC |

`IA32_PAT = 0x0007010600070106` — power-on default with PA1 and PA5 set
to WC. WC is a PAT mechanism (PWT bit selects PA1), not an MTRR one.

## 1.6 Pools, descriptors, GDT/IDT, stacks (partly UNKNOWN)

- **Nonpaged / paged pool**: at the clean baseline retail commits only
  ~5 pages of pool. Exact pool VA bases/sizes for retail are **UNKNOWN**
  (not probed); measured behavior is they're tiny at idle.
- **System memory** (`MmAllocateSystemMemory`): VA `0xD0000000`+,
  demand-mapped; ~40 frames live at baseline.
- **PFN database**: 16 entries at PA `0x03FF0000` (64 KB window) — retail
  does **not** keep a per-page PFN database like NT/ReactOS.
- **GDT / TSS** (measured, `stackpos` probe): GDT at VA `0x8003B298`, TSS at
  VA `0x8003B160` (TR=`0x18`) — low KSEG0, PA `~0x3B000`. The main TSS's
  `Esp0` is **0**: retail runs everything in ring 0, so there is no ring-3→0
  transition stack. #DF/NMI use task gates with their own TSS, sharing one
  static `.data` stack (see Stacks below).
- **Stacks** (measured, `stackpos` probe). Retail splits stack placement by
  thread kind:
  - **Idle/boot stack** — *static, in the image* (`.data`): VA
    `0x80036840`–`0x80039840` (12 KB / 3 pg), PA `~0x39740`.
  - **#DF + NMI stack** — *static, in `.data`*, **shared**: both task-gate
    TSSes (#DF-TSS `0x8003B1C8`, NMI-TSS `0x8003B230`) load ESP `0x8003A840`
    (PA `~0x3A740`), ~4 KB just above the idle stack (handler EIPs
    `0x8001B680` / `0x8001B3FC`).
  - **Runtime (created) thread stacks** — *dynamic*: system-memory region
    (`MmAllocateSystemMemory`, VA `0xD0000000`+) over **low** scattered PA
    (`0x78000`–`0x123000`). A title thread's was VA `0xD0031000`–`0xD0041000`
    (64 KB).

  So: boot/idle/#DF/NMI **static, in-image** (`.data`); created threads
  **dynamic**, system-VA, low-PA. (An earlier reading caught only the
  title-thread stack and wrongly generalized "all retail stacks dynamic".)
  - **No guard pages** on the static stacks: the idle `StackLimit`
    (`0x80036840`) isn't page-aligned and `.text` is directly below it; the
    #DF/NMI stack abuts the idle stack top (one page up, no gap). Retail packs
    them contiguously in `.data` and accepts a silent overflow into adjacent
    image. Guard pages exist only on the dynamic `MmCreateKernelStack` stacks.
  - **nxkrnl today** (same probe): `MmCreateKernelStack` → VA `0xC1042000`
    (nxmm pool window) backed by **high** PA (`0x7F93000`); TSS `Esp0` set to
    `0x84FACDF0` (high KSEG0). The boot/#DF carve (§2.7) takes high-zone
    KSEG0. To mimic retail: route kernel stacks through system-VA / low PA and
    consider dropping the ring-0 `Esp0` stack (retail keeps it 0). Open: the
    early boot/idle/#DF stacks run before the `0xD0000000` aperture is mapped,
    so they need a low-PA region available at carve time, not system-VA.
- **`STICKY` / `IDEXPRDT` sections**: `IDEXPRDT` is the IDE PRD table;
  `STICKY`'s exact contents are **UNKNOWN**.

## 1.7 Physical layout near top of RAM (cxbx constants, 64 MB)

| PA range | Size | Owner |
|---|---|---|
| `0x03FE0000`–`0x03FEFFFF` | 64 KB | NV2A instance memory (RAMHT/RAMFC/contexts) |
| `0x03FF0000`–`0x03FFFFFF` | 64 KB | retail PFN-database window |
| contig limit | ~`0x03FDFFFF` | top of title-claimable RAM |

---

# 2. nxkrnl (current)

## 2.1 Image — base and sections (measured, release build)

Both builds currently base at **VA `0x84000000`** (PA `0x04000000`, the
64 MiB mark; 128 MB box) — release was moved high so titles fit
until the footprint gap closes (§2.3). The retail-base **`0x80010000`**
(PA `0x10000`) is the 64 MB target. Section sizes below are base-independent.

| Section | Bytes | Pages | Resident? |
|---|---|---|---|
| `.text` | 253,024 | 62 | resident |
| `.data` | 3,416 | 1 | resident |
| `.rdata` | 23,652 | 6 | resident |
| `.bss` | 38,904 | 10 | resident |
| `PAGE` | 2,496 | 1 | resident |
| `INIT` | 97,920 | 24 | discarded |
| `.rsrc` | 2,760 | 1 | discarded |
| **resident total** | | **81** | (incl. header) |

`tools/kernel-size.py` reports resident == `0x51000` (331,776 B), exactly at
the title-pool-floor budget. (A `.bss`→high-zone stack carve that would have
dropped this to 77 was tried and **reverted** — it broke release/64; §2.7.)

## 2.2 Runtime page census (measured, `meminv`, clean XBE, 64 MB)

`total=16384 avail=16062 pool=36 stack=40 cache=16`. Kernel-side runtime
**≈276 frames (≈1.1 MB)** — ~2.4× retail's 121. Max contig **48 MB**
(retail ≥60). The gap vs retail is recorded in `docs/footprint.md`.

Key differences from retail in how memory is mapped:
- **KSEG0**: nxkrnl PSE-maps the full 64 MB with **16 × 4 MB pages**
  (`NxkMmEnsureXboxWindows`); retail 4 K-maps it sparsely.
- **`0x84000000`–`0xB0000000`**: ReactOS ARM3 **System-PTE space**
  (capped to 8192 PTEs in `mm/ARM3/i386/init.c`) + paged pool; ~41
  resident frames. Retail: empty.
- **`0xB0000000`**: ReactOS **shadow PFN database** (one-page-backed by
  nxmm via `NXK_MM_PHYS`; ~544 KB of VA, ~1 frame).
- **`0xC1000000`–`0xC1FFFFFF`**: nxmm **pool window** (16 MB VA;
  `ExAllocatePool` backend, `NXK_MM_POOLPAGES`).
- **`0xD0000000`–`0xD3FFFFFF`**: system-VA aperture, 16 PT pages
  (`NXK_SYSMEM_PT_COUNT`), `MmAllocateSystemMemory`.
- **page-supply array (`NxpLinks`)**: 16 pages (4 B × 16384), boot-
  allocated, not in the image. Replaces the ARM3 PFN free lists.

MTRR/PAT/WC aperture/MMIO match retail (verified earlier).

## 2.3 Physical layout — retail-base 64 MB **(target, not what ships today)**

> **Current release *and* DBG run the devkit high-base layout, not this one.**
> Both builds use `IMAGEBASE 0x84000000` (unconditional),
> so the kernel image sits at **VA `0x84000000` = PA `0x04000000`** — the
> 64 MiB mark — on a **128 MB** box (the `boot_layout_devkit` column in §2.4).
> The retail-base map below (kernel at PA `0x10000`, the whole kernel inside
> 64 MB) is the layout the footprint work is driving toward; the loader still
> implements it (`loaderblock.c`, `image_base == NXKRNL_RETAIL_IMAGE_BASE`)
> but no build selects it yet. Until the resident footprint closes the gap,
> release boots high so titles fit (see `footprint.md`).

| PA range | Size | Owner |
|---|---|---|
| `0x000000`–`0x000FFF` | 4 KB | IVT / BDA — reserved but **vestigial** (kernel runs off the IDT, not the real-mode IVT) |
| `0x001000`–`0x00DFFF` | 52 KB | low pin region — **title-allocatable** (retail-probed) |
| `0x00E000`–`0x00EFFF` | 4 KB | persist slot — reserved but **vestigial** (`NxMmPersistContiguousMemory` is a stub) |
| `0x00F000`–`0x00FFFF` | 4 KB | **CR3** (page directory; the only live low page; matches retail) |
| `0x010000`–`0x060FFF` | 324 KB | **kernel image** (resident; 81 pages) — *target only; today the image is at PA `0x04000000`* |
| `0x061000`–`0x0FFFFF` | ~620 KB | slot slack — free; preferred for sub-16 MB DMA structs |
| `0x100000`–`0x1FFFFF` | 1 MB | loader code + stack (LoaderFirmwareTemporary, reclaimed) |
| `0x200000`–`0x33FFFFF` | ~50 MB | **title arena (main)** — free, claimed into title heap |
| `0x3400000`–`0x3BBFFFF` | ~7.75 MB | **NT bring-up carve** (high zone; `MxFreeDescriptor` re-points here) |
| `0x3BC0000`–`0x3BE9FFF` | ~168 KB | **loader cluster** — PTs + GDT/IDT/TSS + PCR backing + LB/Aux |
| `0x3BEA000`–`0x3BFFFFF` | ~88 KB (22 pg) | **genuinely free** — `LoaderFree` (loaderblock.c), claimed into the supply by `MiBuildPfnDatabaseFromLoaderBlock` (`NxkPageSupplyReturn`); already in the title's free count, *not* reclaimable. Alignment slack below the FB; fragments max-contig, doesn't add free pages. |
| `0x3C00000`–`0x3FDFFFF` | ~3.9 MB | **title arena (top)** — after FB reclaim; keeps retail's top-down landing PAs |
| `0x3FE0000`–`0x3FEFFFF` | 64 KB | NV2A instance reserve |
| `0x3FF0000`–`0x3FFFFFF` | 64 KB | PFN-db window (kept reserved for `MmClaimGpuInstanceMemory` parity) |

## 2.4 Bootloader (nxldr) flow

`boot/nxldr` runs from flash, brings the CPU to protected mode + paging,
loads `xboxkrnl.exe`, builds the LOADER_PARAMETER_BLOCK, and `call`s
`KiSystemStartup` on the **loader stack** (`_stack_top = 0x001FFFF0`).

Two layouts in `loader.c`, selected by the kernel's PE image base in
`pe_load`. **Both builds currently select `boot_layout_devkit`** (release +
DBG are both based at `0x84000000`); `boot_layout_retail` is
the 64 MB target, implemented but unselected (§2.3):

| | `boot_layout_devkit` (current: both builds, 128 MB) | `boot_layout_retail` (target, unused, 64 MB) |
|---|---|---|
| image base | `0x84000000` | `0x80010000` |
| PT_LOW (4 pg, identity 0–16 MB) | `0x07F00000` | `0x03BC0000` |
| PT_KSEG0 | `0x07F04000`, **17 PTs** → VA `0x80000000`–`0x84400000` | `0x03BC4000`, **16 PTs** → VA `0x80000000`–`0x84000000` |
| PT_PCR | `0x07F15000` | `0x03BD4000` |
| GDT / IDT / TSS (1 pg each) | `0x26000` / `0x27000` / `0x28000` | `0x03BD5000` / `0x03BD6000` / `0x03BD7000` |
| PCR region (16 pg, VA `0xFFDF0000`) | `0x30000` | `0x03BD8000` |
| LoaderBlock / Aux | `0x40000` / `0x41000` | `0x03BE8000` / `0x03BE9000` |
| pages_spanned | `0x4400` | `0x4000` |

The cluster's **GDT/IDT/TSS pages are reclaimed** once the kernel installs
its own resident tables early in `KiSystemStartup` (§2.5).

Paging set up in `paging.c`: identity-maps PA 0–16 MB (PT_LOW), maps
KSEG0 (PT_KSEG0, all 4 KB PTEs — these are the 16/17 PT pages),
self-maps the PD at `PD[0x300]` (→ VA `0xC0000000`–`0xC03FFFFF`), and the
PCR region at VA `0xFFDF0000`. `CR3` = PD at PA `0xF000`.

Memory descriptors built in `loaderblock.c` (release: an 11-entry `m[]`
array; see §2.3 for the resulting types). The **NT bring-up carve**
(`0x3400000`+) is emitted as `LoaderFree`; `xb/mm/init.c`
(`NxkMmReserveXboxWindows`) exempts `[NXK_HIGH_ZONE_BASE, LIMIT)` from the
title claim and re-points `MxFreeDescriptor` there.

**DBG-retail-map only:** a *phantom reservation* (`NXK_PHANTOM_FREE_PAGES`
knob in `loaderblock.c`) reserves `0x10000`–`0x61000` to mimic the release
kernel's low footprint so the title sees release-equivalent free memory
even though the DBG kernel is physically high. Binary search on this knob
established that **Halo needs 22 more free pages** to play its intro.

## 2.5 GDT / IDT / TSS / PCR

The kernel **owns** its descriptor tables, resident in the image, like
retail — it does not run on the loader's copies (which it only inherits to
bootstrap, then reclaims):

- **GDT** — `KiGdt[16]` (`.data`, `cpu.c`). Very early in `KiSystemStartup`
  the kernel copies the loader's GDT into `KiGdt` and `lgdt`s to it; the
  layout is identical, so the live CS/DS/SS/FS keep valid cached
  descriptors. Subsequent GDT writes — the TSS/#DF/NMI descriptors here,
  per-thread TEB later — land in `KiGdt`. (A compiler barrier commits the
  copy before the `lgdt`; the CPU's GDT reads are invisible to the
  optimizer.)
- **IDT** — `KiIdt` (`.data`, `trap.s`), trimmed to **64 entries / 512 B**
  on Xbox (vectors 0–0x3F, via `SARCH_XBOX` in `trap.s` + the
  `KeInitExceptions` fill loop) vs NT's 256-entry / 2 KB table. The kernel
  `lidt`s `KiIdt` directly; `#DF`/`NMI` keep task gates (`KiIdt[8]`/`[2]`)
  into `KiDoubleFaultTSS` / `KiNMITSS`.
- **Main TSS** — `KiInitialTss` (`.data`), **minimal, no I/O bitmap**, like
  retail; `ltr` points the GDT TSS descriptor at it.
- **PCR** — still loader-provided: 16 pages at VA `0xFFDF0000`, zeroed and
  initialized by `KiInitializePcr`. KPCR layout matches NT (KIP0PCRADDRESS
  `0xFFDFF000`).

Because the kernel owns all three tables, the loader's GDT/IDT/TSS pages are
dead by the time Mm runs and are **reclaimed** (§2.4): devkit
`md_gdtidttss` (PA `0x26000`–`0x29000`) → `LoaderFree`; release splits the
cluster so the 3 descriptor pages are freed while the page tables +
PCR/LoaderBlock stay live.

## 2.6 Pools and page supply

- **`ExAllocatePool`** → nxmm pool window (`0xC1000000`, 16 MB VA),
  demand-backed from the page supply; high-zone (kernel-preferred) first.
- **`MmAllocateContiguousMemory(Ex)`** → `xb/mm/contig.c`, top-down scan
  over the one page supply (`NxkPageSupplyTakeRun`), returns KSEG0 VAs.
- **`MmAllocateSystemMemory`** → `0xD0000000` aperture (`xb/mm/sysva.c`),
  backed by NT NP pool aliased in; falls back to NP pool on exhaustion.
- **Page supply** (`xb/mm/pagesupply.c`, `NXK_MM_PHYS`): one
  `NxpLinks` array (4 B/page) for all RAM; pages below PFN `0x10` are
  pin-only; kernel-preferred class = high zone (retail base) / upper
  64 MB (devkit).
- Residual ARM3 (System-PTE space, paged pool, shadow PFN) still
  initialized but largely unused for allocation. Its apparent footprint is
  small/illusory once measured (§3 item 5); the retail gap is the resident
  image, not this machinery (`footprint.md`).

## 2.7 Stacks — static `.bss` (the high-zone carve was reverted)

`P0BootStack` (`KERNEL_STACK_SIZE` = 12 KB / 3 pg) and `KiDoubleFaultStack`
(1 pg) are **static `.bss`** in `xb/kistacks.c`, packed at the front of
`.bss`.

This **matches retail's positioning** — only the section differs (`.bss` vs
`.data`; retail has no `.bss`). The `stackpos` probe on retail (measured,
§1.6) shows retail keeps its boot/idle and fault stacks **static in `.data`**,
in the image:
- **Idle/boot stack** — VA `0x80036840`–`0x80039840` (12 KB / 3 pg,
  `KERNEL_STACK_SIZE`), PA `~0x39740`.
- **#DF + NMI stack** — *shared*: ESP `0x8003A840` (PA `~0x3A740`), ~4 KB
  just above the idle stack; the #DF-TSS (`0x8003B1C8`) and NMI-TSS
  (`0x8003B230`) both load the same ESP.
- These sit alongside the GDT/TSS at the `.data` tail (§1.6): idle (12 KB) +
  #DF/NMI (4 KB) + descriptors (~0.5 KB) ≈ 17 KB of the 6-page (24 KB)
  `.data`.

Only retail's *runtime, dynamically-created* thread stacks go **dynamic** —
system-VA `0xD0000000`+, low scattered PA (a title thread was VA
`0xD0031000`–`0xD0041000`). So the split is **boot/idle/#DF/NMI → static,
in-image; created threads → dynamic, system-VA.** Our `.bss` boot/#DF stacks
are the static half done retail-faithfully; the open lever is just moving
them `.bss` → `.data` (§3) to land in the same section.

**No guard pages** on either side's static stacks — matches retail. We
dropped the #DF stack's guard page; `kistacks.c` notes it only
runs into the bugcheck path, so a guard page isn't worth a resident page.
`P0BootStack` has none either. Retail does the same (§1.6): its static stacks
are packed contiguously in `.data` with no guard. (Guard pages remain on the
dynamic `MmCreateKernelStack` thread stacks, both kernels.)

A `NxkCarveBootStacks` experiment that relocated the boot/#DF stacks out of
the image into the high-zone KSEG0 carve (dropping resident 81 → 77) was
tried and **reverted**: it booted on DBG/128 but **broke the release/64
build** (dead before USB; bisected to the carve commit).  The earlier
"faults at the `KiSwitchToBootStack` ESP switch / KSEG0 not mapped" theory
was **disproven** by instrumentation — the switch reaches the new stack on
both layouts; the real failure was release-base-specific.  If revisited,
mimic retail directly: a resident `.data` stack region, not a dynamic
high-zone carve.

---

# 3. Compatibility gaps (retail ← nxkrnl)

Target: claw back the **22 free frames** Halo's intro needs (the phantom-knob
bisection in `footprint.md`). The dominant lever is the **resident
image** — 81 pages vs retail's 47 (−34, covers 22 with margin): `.bss`→
dynamic, INIT migration of boot-only `.text`, nxata/nxdisk thin storage stack.
The kernel's total resident footprint is **276 frames vs retail 121**; the 22
come out of that total. Itemized in `footprint.md`.

> **The "ARM3 Mm retirement ~41 frames" lever was disproven** (measured
> 2026-06-15, `tests/xbe/syspte`): of the 41 in `0x84M–0xB0M`, 1024 are MMIO
> framebuffer maps (no RAM), ~40 are a zeroed contiguous **KSEG0-resident**
> allocation only *attributed* here by a meminv PSE-counting artifact (not
> freed by retiring the space), and ~9 are the PT skeleton. Real recoverable:
> single-digit PT pages. Don't chase it.

Summary:

| Area | Retail | nxkrnl | Action |
|---|---|---|---|
| resident image | 47 pg | 81 pg | the levers below |
| `.bss` / stacks | ~0 (stacks static in `.data`) | 10 pg (incl. 4 pg static `.bss` stacks) | match retail: resident `.data` stacks, not a high-zone carve (§2.7) |
| `.text` | 38 pg | 62 pg | nxata/nxdisk thin storage stack (~12 pg) |
| KSEG0 mapping | 4 K sparse | 16 × 4 MB PSE | (ours is cheaper in PT pages; not a gap) |
| ARM3 sys-PTE space (`0x84M`) | none | "41" frames | **not a lever** — MMIO + a KSEG0-resident alloc miscounted here; ~9 PT pages real (item 5) |
| max contig | ≥60 MB | 48 MB | the high-zone carve fragments the arena |
| GDT/IDT/TSS | static `.data` PA `~0x3B000` (7-entry GDT, 64-entry IDT) | kernel-owned, resident `.data`; loader pages reclaimed (§2.5) | match section (we trim IDT to 64 too) |
| idle/#DF/NMI stacks | static `.data`, PA `~0x39000`–`0x3A000` (§1.6) | static `.bss`, 4 pg | move `.bss` → `.data` (§2.7) |

## Open investigations for the follow-up

1. ~~Fix the release-64 boot with the high-zone carved stacks~~ —
   **abandoned**. The carve broke release/64 (dead before USB) and was
   reverted; the retail-faithful path is resident `.data` stacks, not a
   dynamic high-zone carve (§2.7).
2. ~~Probe retail's GDT/IDT/TSS and stack physical addresses + sizes~~ —
   **done** (§1.6). `stackpos` (incl. idle thread + #DF/NMI task-gate TSSes)
   measured: GDT 7-entry / IDT 64-entry / TSS at `.data` PA `~0x3B000`, main
   `Esp0`=0; idle/boot stack static `.data` (VA `0x80036840`+, 12 KB); #DF and
   NMI share one static `.data` stack (ESP `0x8003A840`); created threads
   dynamic at `0xD0000000`+. Still open: retail's `STICKY` (item 3).
3. Identify retail's `STICKY` section contents (§1.1).
4. Decide whether to keep PSE-KSEG0 or match retail's 4 K mapping.
5. **ARM3 sys-PTE bring-up — low value, disproven (do last, if at all).**
   `MiInitializeSystemPtes` / `MiBuildPagedPool` still run unguarded at init,
   but the measurement (`tests/xbe/syspte`, 2026-06-15) shows the "~41 frames"
   they appeared to cost is **not reclaimable**: 1024 MMIO framebuffer maps
   (no RAM), ~40 a zeroed contiguous KSEG0-resident allocation only attributed
   here by a meminv PSE-counting quirk, ~9 PT skeleton. Gating recovers only
   single-digit PT pages, and only after rerouting the live consumers
   (`MmMapIoSpace` / MDL → the `0xD0000000` sysva aperture). Paged-pool
   allocation is already dead (nxpool serves it; ~2 boot frames). Pursue the
   image shrink (intro) instead.
6. Move the boot/#DF stacks `.bss` → `.data` to match retail's section
   layout (fidelity; no page win) (§2.7).
