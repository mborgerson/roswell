# Kernel footprint: the image budget and the runtime-page gap

Three things, only the first of which is done. All measured against the
retail kernel as a black-box oracle (probe XBEs, never `bios.bin`
disassembly):

- **ROM / flash size — met.** The xz-compressed kernel fits its flash
  slot with headroom (~28% free), so image *size* on disk is solved.
- **Resident-section ceiling — reached, but that is not the bar.** The
  kernel loads at PA `0x10000` and titles hardcode a contiguous pool at
  PA `0x61000`, so everything resident after `MiFreeInitializationCode`
  must fit under **`0x51000` = 331,776 bytes**. `tools/kernel-size.py`
  reports us sitting *at* `0x51000` — right against the ceiling, no
  slack. (`MiFreeInitializationCode` donates freed INIT/INITDATA/`.rsrc`
  pages to the title heap when they land in title-visible RAM via
  `NxkMmTryDonateInitPages`, so this counts resident sections, not the
  whole image — the same trick retail plays, its ~350 KB image also
  crosses `0x61000`.)
- **Runtime page footprint — NOT met, and it is the real target.** At
  runtime we still occupy far more pages than retail: resident kernel
  **81 pages vs retail's 47** (+34), plus page tables, pool, and `.bss`.
  So the kernel can't yet slot in below the title pool at the retail
  base — which is exactly why both builds run at the **high devkit base
  (`0x84000000`)** and give the title all of low RAM. Closing this gap
  (demand-committed usage, not reservation size) is what unblocks the
  retail IMAGEBASE move.

## The target is 47 pages, and Halo needs 22 back

`tests/xbe/krnlsect` reads the running kernel's PE section header at the
fixed base `0x80010000` (black-box) on both kernels — the authoritative
image size:

| Section | Retail | Ours |
|---|---|---|
| `.text` | 38 pg (153 KB) | 62 pg (253 KB) |
| `.data` | 6 pg | 1 pg |
| `.rdata` | (folded into `.data`) | 6 pg |
| `.bss` | ~0 | 10 pg (40 KB) |
| `PAGE` | — | 1 pg |
| `STICKY` / `IDEXPRDT` | 3 pg | — |
| **resident** | **47 pg (188 KB)** | **81 pg (incl. header)** |
| INIT (discarded) | 111 pg (453 KB) | 24 pg |

Retail front-loads **4.6× more code into discardable INIT** than we do,
which is most of the `.text` gap: much of our resident code is boot-only
and movable. It also keeps ~zero static `.bss` — boot/idle/#DF stacks,
interrupt shadows, and tables that we place in `.bss` retail allocates
dynamically at boot (see `memory-map.md` §2.7).

Full retail parity isn't required to boot titles. Binary search on the
DBG-retail-map phantom reservation (`NXK_PHANTOM_FREE_PAGES` in
`boot/nxldr/loaderblock.c`, Halo CE intro + menu, verified visually and
by log markers) pins the need at **exactly 22 more free pages (88 KB)**:
too few → Bink decode thread can't spawn (green screen); a little more →
thread runs but a decode buffer alloc returns NULL → null-deref bugcheck;
≥22 → video plays. Target: **resident 81 → 59 pages**, well inside the
−34 gap.

## What is and isn't a lever

**Levers (value order):**
1. **Resident image (−34 vs retail).** The prize. `.bss`→dynamic,
   INIT-migration of boot-only `.text`, and a purpose-built thin storage
   stack (nxata/nxdisk, ~50 KB pool) — the export surface is shared
   substrate (`tools/ordinal-cost.py`: ~294 ordinals carry only ~27 KB
   of exclusive cones, all title-critical), so a bespoke storage stack is
   the remaining big `.text` lever, not stub-reversion.
2. **Page tables (~12 frames).** Reconcile our PSE-KSEG0 + sysva-aperture
   PTs against retail's sparser set.
3. **High-zone NT carve (−12 MB max-contig).** Relocate/shrink so the
   title arena is one ~60 MB run like retail. Layout change.
4. **Pool object-shrinking (~7 frames).** Incremental; device/thread
   objects, object-manager tables.

**Non-levers (measured, do not pursue):**
- **Retiring the ARM3 sys-PTE space** looks like ~41 frames but is mostly
  a measurement artifact (`tests/xbe/syspte`): ~1024 are MMIO framebuffer
  maps (zero RAM), ~40 are a zeroed KSEG0-resident allocation only
  *attributed* to the sys-PTE window by a `meminv` PSE-counting quirk, ~9
  are the PT skeleton. Real recoverable: single-digit PT pages, and only
  after rerouting `MmMapIoSpace`/MDL consumers. Low value, real effort.
- **Shrinking pool VA reservations** (`MmSizeOfNonPagedPoolInBytes`,
  `NXK_SYSMEM_PT_COUNT`) frees **zero** frames — they are VA reservations,
  not committed pages.
- **Inbv / bootvid** — KeBugCheck renders through them.
- **vfatfs / xdvdfs runtime bodies** — titles do file I/O for their whole
  run; the Cc remnant is vfatfs's live working set.

## Methodology

- `tools/kernel-size.py` — the tracked metric: page-rounded sum of
  non-DISCARDABLE sections plus the header page, with the delta to
  budget. Record the delta in every trim commit.
- `tools/init-audit.py` — conservative **static** closure: address-taken
  anywhere = runtime root (init code plants pointers into long-lived
  structures); exports = runtime roots. `--check` runs post-link (both
  trees, in the POST_BUILD chain) and fails the link if any resident
  section references INIT/INITDATA/`.rsrc`; legitimate pre-discard
  orchestrators live in `tools/init-ref.allow` with per-entry
  justifications.
- `tools/cov-analyze.py --init log1 log2 …` — dynamic **ranker** from
  `run-xemu --trace` runs. It **over-includes**: `in_asm` logs at
  translation time, so hot runtime code translated during boot looks
  boot-only. Static closure is the gate; coverage only ranks. Capture it
  against a `-DLTCG=FALSE` build (`build-cov/`) — LTO clone names and
  cross-TU inlining smear per-function attribution.
- `tools/ordinal-cost.py` — per-ordinal exclusive `.text` cost; record
  deltas when implementing ordinals.
- Probes run on both kernels: `krnlsect` (PE sections), `meminv`
  (`MmQueryStatistics` + deduped page-table census + max-contig). See
  `memory-map.md` for the detail.

Gates per batch:
- **Both trees.** The DBG (non-LTO) build keeps callers Release GC'd away
  (kd debug-log, `RtlAssert`) and exposes resident→INIT edges LTO inlines
  out of existence. A Release-only gate has shipped DBG link breaks.
- **Correctness + a title boot.** The api-regression suite must pass, plus
  a windowed Halo CE boot to menu with OHCI traffic. Init-reachability is
  necessary but not sufficient — the function must also *return* before
  Phase 1 ends (`KiIdleLoop` is init-entered and runs forever) and must
  not be DBG-live (`DbgPrintEx`).
- **Retail A/B for ordinal-narrowing trims.** A trim that narrows an
  *exported* ordinal's argument space needs a retail-behavior comparison,
  not just api-regression + a boot — see the `footguns.md` alertability
  entry for why. One subsystem per commit.

## Remaining work

The coverage-driven `.text` rounds and the `.bss`/`.idata`/section-page
packing are done; the image sits at the ceiling. What's left to reach the
47-page target and Halo's 22 pages:

1. **`.bss`→dynamic + `.bss`→`.data`.** Match retail: allocate the boot/
   interrupt buffers at boot, land the static stacks in `.data`
   (`memory-map.md` §2.7 — the high-zone carve was tried and reverted).
2. **INIT-migration of boot-only `.text`.** Retail proves far more is
   movable; `init-audit.py --check` crash-proofs each migration.
3. **Thin storage stack (nxata/nxdisk, ~50 KB pool).** The strategic
   `.text` lever once coverage is exhausted.
4. **Split layout** (data sections above the title region) closes
   whatever cutting can't — but a true retail-base boot is gated on more
   than the budget (`boot/nxldr` hardcodes the image base/PA and the
   loader-block descriptor list lives inside a retail-base image).

Meter every image cut with `kernel-size.py`; meter every runtime-page cut
with `meminv` on both kernels; gate with api-regression (64 and 128 MB) +
a windowed Halo CE boot.
