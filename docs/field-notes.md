# Field notes

Non-obvious constraints and empirically-discovered hardware / ABI / title
facts behind the kernel — the load-bearing "why" that isn't visible in the
code itself. These are the lessons a maintainer couldn't re-derive by
reading the source: hardware quirks, retail-kernel behavior measured as a
black-box oracle, and title expectations found the hard way.

Deeper treatments live elsewhere — [`footguns.md`](footguns.md) for "looks
safe but isn't" changes, [`memory-map.md`](memory-map.md) for layout,
[`struct-audit.md`](struct-audit.md) for the struct ABI — and are
cross-referenced rather than repeated here.

## Boot / loader

- **MCPX-less boot.** With no bootrom overlay, xemu drops the CPU reset vector
  straight into flash; the kernel owns the reset path with a real→pmode stub +
  an asm Xcode-bytecode interpreter (no stack, no SSE). Flash is mirrored across
  0xff000000–0xffffffff and the reset vector / Xcode-overflow entry live in the
  LAST mirror, so link the in-flash image at 0xfffc0000, not 0xff000000. The
  `mov $entry;jmp` poke must track the entry symbol's real VMA or it triple-faults.
- **Freestanding loader traps.** Pad each loaded section to 32 B (a flat
  `rep movsl` else lands .rodata below its VMA); build with `-mgeneral-regs-only`
  (a stray `movdqa` with CR4.OSFXSR off → #UD → boot loop). Enable **CR4.PSE
  before CR0.PG** or every 4 MB PSE PDE the HAL/Mm installs is walked as a PT
  pointer.
- **GDT/lgdt barrier.** The kernel copies the loader GDT into resident `KiGdt`
  and `lgdt`s it; a compiler memory barrier between the copy and the `lgdt` is
  mandatory or the dead-looking copy is dropped and lgdt loads garbage → silent
  triple fault. The loader itself needs no GDT (it inherits flat pmode selectors).
- **Kernel synthesizes its own loader block.** The loader must NOT build the
  LOADER_PARAMETER_BLOCK / descriptors — the kernel builds them from its own PE
  header in the KiSystemStartup prologue; a hand-mirrored NTDDI_LONGHORN block
  wrote a phantom FirmwareInformation field the (pre-LONGHORN) kernel never had.
  Handoff is `call` not `jmp` (KiSystemStartup is __stdcall, arg at [esp+4]); the
  PCR is 16 pages at 0xFFDF0000.
- **The PE loader refuses to rebase**: the optional-header ImageBase must equal
  the link-time kernel image base (no relocations are applied).
- **Load-bearing PCI bring-up.** IDE config reg **0x50 = 0x02** (nForce
  channel-enable) or pciidex sees the channels disabled → no PDOs → atapi never
  attaches → xdvdfs can't mount → `c000003a`. Two 2BL writes were also missing
  (LPC 0x8C = 0x40000000 enable IDE+NIC; host bridge 0x80 = 0x100 Whoami), found
  by diffing an xemu PCI config-space trace. PnP needs the full ConfigurationRoot
  MultiFunctionAdapter tree (NoBuses = 2) or the PCI driver never starts.
- **INRAM boot** (single-ROM flash, no DVD) must skip IopMarkBootPartition (else
  it stamps the boot device as \Device\CdRom0 → bugcheck 0x7B); `InitWinPEModeType`
  is upstream a 1-byte BOOLEAN but gets `|= 0x80000000` — widen it to ULONG or the
  INRAM bit truncates to 0.
- **Devkit high-base landmines.** The GDT/IDT/TSS must be KSEG0-mapped into the
  kernel page directory (the first KeFlushEntireTb evicts the stale TLB entry →
  IDT unreadable → triple fault); reserve cromwell's framebuffer (PA 0x3C00000–
  0x04000000) as firmware-permanent since bootvid zeroes it through the 0xF0000000
  aperture and would wipe an IDT placed there. A PCR at PA 0x29000 tripped an
  MmInit assert; 0x30000 works.
- **Chainload / reboot.** A QuickReboot with no explicit LDT_TITLE must persist
  LDT_LAUNCH_DASHBOARD or the next boot re-launches D:\default.xbe → infinite loop.
  HalReturnToFirmware drives an SMC **warm reset that preserves RAM** (verified by
  reading a magic value back from a physical address), so the persist slot must
  sit in the kernel's low-PA reserve, not the title heap. Title search order:
  D:\default.xbe → C:\xboxdash.xbe (Partition4 = C:).
- **Release splash.** Paint text at FbConsPutChar (UiVtbl is only partially init'd)
  and KEEP the upstream inbv bootanim.rc resources — without /SOS the kernel loads
  IDB_PROGRESS_BAR and BitBltAligned dereferences the bitmap with no NULL check →
  guaranteed 0x7E if the resource is missing.
- See [`memory-map.md`](memory-map.md) for MTRR/PAT programming and the loader
  cluster layout; the folded-driver / IopLoadDriver-short-circuit model is in the
  memory notes.

## Memory (Mm)

- **The KSEG0 contract titles depend on**: contiguous-memory VA == PA | 0x80000000,
  and nxdk recovers a GPU physical address by masking the pointer with **0x03FFFFFF**
  (not MmGetPhysicalAddress), then dereferences the low-VA alias directly (uncached),
  even poking a pushbuffer jump at VA 0x80000000. Mm must therefore identity-map the
  low-VA aliases of the contiguous sub-window, not only the KSEG0 aliases.
- **PSE-window hazards.** The KSEG0 PSE paint must cap at MmSystemPteSpaceStart or
  it orphans the PT pages backing system-PTE allocations → the dispatcher reads
  zeros from wait-path buffers → KeDelay-after-MmAllocate hangs (see the kedelay-hang
  memory). The MMIO window (0xF0000000+) collides with nonpaged-pool expansion VAs
  (MmNonPagedPoolEnd 0xFFBE0000 walks down into it) → cap it at NXK_MMIO_BASE, else
  pool PTE writes land on NV2A registers. `MmIsAddressValid` on the statically-painted
  PSE windows must short-circuit — the self-map reads a PSE PDE as a "PTE" so its
  Valid bit is coincidental.
- **The relocating contiguous allocator.** Titles pin exact windows that cover their
  own XBE image and rely on the page moving with content intact, so the allocator
  needs a frame→owning-PTE reverse map and does copy + retarget + TLB-flush at
  HIGH_LEVEL in stack-write-free asm (so even the caller's own stack pages are
  movable). It must NOT move MDL-locked frames (a device has captured the PA).
  Single-page spills **ascend** from the lowest free PA — descending spill shredded
  the framebuffer zone and a 2.4 MB surface alloc failed → the title bailed. A
  step-down bug (`(Base + N − N) & ~(align−1) == Base` never advances) spun forever
  under the PFN lock at DISPATCH — both Halos froze on a black screen with NO
  bugcheck.
- **Pool is frames, unmapped on free.** Retail unmaps whole-page pool on free
  (MmIsAddressValid returns FALSE after ExFreePool, verified against bios.bin), so
  a use-after-free faults like retail; the window allocator must unmap freed runs
  and return the frames, and NxMmIsAddressValid needs a PT-walk fallback for live
  pool. Buckets are power-of-two 32..2048 with a 2040-byte small-block ceiling
  (small blocks stay mapped, big ones unmap). nxpool is routed by VA, so
  MmAllocateContiguousMemory frees must ask NxPoolPagesOwns() or they bugcheck on
  the PFN path.
- **Pool VA window ≥ 32 MB.** The DVD intro-video path makes a single 3250-page
  (12.7 MB) pool allocation; a 16 MB window green-screened both Halos before their
  first frame. The window VA is free — only touched pages cost RAM — so sizing it
  to a "budget" was backwards (it caused a Bink-intro regression).
- **The synthetic UserApcPending leak.** The flag set to wake alertable poll-waits
  (with STATUS_USER_APC) leaks into the *non-alertable* user-wait branch too;
  distinguish real from synthetic by whether ApcState.ApcListHead[UserMode] is
  empty, else non-alertable waits get a spurious USER_APC and titles stall (Halo's
  ui.map streaming froze at ~2.5 MB).
- **Large-page MmProbeAndLockPages.** Xbox maps low memory + KSEG0 via 4 MB PSE
  pages, so driver buffers are often "physically resident"; stock ReactOS ASSERTs
  `!MI_IS_PHYSICAL_ADDRESS` then assumes 4 KB PTEs → bugcheck 0xA in CdCreateUserMdl
  on Halo's ui.map. Build the MDL from the large-page PDE PFNs (no probe/lock).
- **RAM accounting on the 128 MB map.** MmHighestPhysicalPage from the descriptor
  scan excluded hardware-reserved types → it returned reclaimed NV2A-split pages
  PAST the end of the supply array (corrupting adjacent pool metadata in release)
  and lost the top 3.875 MB; raise it to the 64 MB boundary and size the supply to
  match. Separately, MmQueryStatistics undercounted RAM (it dropped the mid-RAM
  LoaderFirmwarePermanent framebuffer block → AvailablePages > TotalPhysicalPages)
  → stamp the total from the top of the loader descriptor list.
- **Loader-page reclaim.** MmFreeLoaderBlock reading MMPFN refcount/Flink under the
  singleton-PFN model is reading graffiti → a wild write stomped title memory (green
  screen, but normal serial — serial gates are blind to scanout, so flag-bisect with
  eyes on the screen). Under NXK_MM_PHYS just keep the loader pages / tag them
  LoaderOsloaderHeap and reclaim via the supply.
- **No-pagefile invariants.** Disable the balance manager + pageout (MiBalancerThread
  → MmTrimUserMemory can't make forward progress with no pagefile); titles run ring-0
  with no PEB/TEB (ThreadContext = NULL) so MmCreatePeb/MmCreateTeb are unreachable.
  NV2A padding at PA 0x3FF0000 (retail's PFN-database home) aliases PRAMIN in xemu
  and is NOT usable as plain RAM.
- **File cache.** NxcBlocks is title-visible .bss capped at 32 blocks / 512 KB
  (the Halos boot on the 4-block default and never call FscSetCacheSize; over-cap is
  dead RAM, over-request just evicts). The 16 KB block experiment must stay reverted
  at 64 KB — NxcPinCommon truncates cross-block requests and vfat spans straddle a
  16 KB block, so callers walk off the buffer (dir-creation AV). The single cache
  mutex is never held across IO, which keeps FSD recursion legal (a data-miss paging
  read re-enters the cache for FAT metadata).
- See the contig carve-out, nxvm eager-commit, and "resident sections < 324 KB"
  donation notes in the memory index and [`footprint.md`](footprint.md).

## IO / storage / FS

- **DMA-not-PIO.** AtaGetRegistryKey fails AND returns without applying the caller's
  default → TransferModeUserAllowedMask = 0 strips every DMA bit → PIO only → ~10x
  IDE IRQ inflation; seed UAllowed = MAXULONG in pdo.c. DMA also needs BAR4 + IRQs
  14/15 in the static resource list, identity-translation of the legacy ATA ports
  (below the PCI IO window), and Isa-retry of the IRQs — HalTranslateBusAddress /
  HalGetInterruptVector otherwise fail and null the whole list. And the final
  SCATTER_GATHER_LIST must be carved from the AdapterControl context buffer, NOT
  ExAllocatePoolWithTag'd at callback time: under 64 MB pool pressure that alloc
  fails, the callback returns without calling AdapterListControl, and one failure
  wedges all storage DMA.
- **ATAPI PIO fallback for misaligned buffers.** BM-IDE PRDs require word-aligned
  regions and PciIdePreparePrdTable asserts hard; real titles pass byte-aligned
  NtReadFile buffers (retail handles this transparently) → check the DataBuffer LSB
  and skip S/G → PIO. Newer nxdk dashboards triple-faulted at pata_io.c before main();
  the HDD xboxdash only worked by even-alignment luck.
- **CHECK_VERIFY.** Retail does NOT implement IOCTL_STORAGE_CHECK_VERIFY (it returns
  STATUS_INVALID_DEVICE_REQUEST in every shape); keep the SCSI-emitting arm gated or
  it dereferences a compile-time-NULL srb (write to 0xA) on any title-issued
  CHECK_VERIFY.
- **rawfs can't be dropped.** Titles open \Device\Harddisk0\partition0 raw to read
  the partition table; without the built-in RAW file system the IoMountVolume cascade
  fails STATUS_UNRECOGNIZED_VOLUME and the title can't start.
- **The Xbox HDD has no MBR/GPT** — its five FATX partitions (C/E + the X/Y/Z caches)
  sit at fixed byte offsets baked into firmware; IoReadPartitionTableEx probes those
  offsets for the FATX magic and synthesizes a DRIVE_LAYOUT so partmgr enumerates
  them (offsets cross-checked against cromwell's fatx and fatxfs).
- **FATX directory quirks.** The "directory mask ignored" bug was in
  NtQueryDirectoryFile, not vfatfs: the search-pattern capture lived entirely inside
  the `PreviousMode != KernelMode` probe block (compiled out for ring-0 titles), so
  FileName was always NULL → match-all; it needs a probe-free kernel-mode capture.
  FATX has no on-disk ./.. and retail returns none, so gate the synthesized dot
  entries and their +2 index bias off on Xbox (see the fontcache-fatx-bias memory).
  The proto also has no ReturnSingleEntry and uses ANSI names (FATX stores ASCII).
- **vfat lifecycle.** GrabFCBFromTable must skip stale FCBs (FCB_DELETE_PENDING with
  OpenHandleCount = 0): cache-manager BCB refs drain asynchronously so IRP_MJ_CLOSE
  fires after NtClose returns, and without the skip the next CREATE for the same path
  returns STATUS_DELETE_PENDING. Dismount must refuse (flush + ACCESS_DENIED) while
  other handles are open (else it's a UAF over freed FCBs) — BUT Halo 2 self-formats
  its cache partition and dismounts it WITHOUT an exclusive lock, so drop the
  VCB_VOLUME_LOCKED prerequisite while keeping VCB_IS_SYS_OR_HAS_PAGE guarding C:.
  (The title also needs 64 KB clusters — a 16 KB test HDD still fails.) The vfat
  dirty-bit / FSInfo machinery is dead on FATX (no 0xaa55 boot-sector signature —
  FATX has 0xFF at offset 510).
- **Async-IO completion.** NtUserIoApcDispatcher must actually deliver the async-IO
  completion callback (convert NTSTATUS/Information → dwErrorCode/dwBytesTransferred,
  invoke the title routine); left inert, Halo's HDD cache never learned its writes
  finished, stored INVALID_HANDLE in its slot table, and wedged before rendering.
- **NxbeNtReleaseMutant** must pre-check KMUTANT.OwnerThread in C — KeReleaseMutant
  raises STATUS_MUTANT_NOT_OWNED via the unstructured kernel-exception model and the
  SEH-less shim bugchecks 0x7E; cdfs masked it (instant cached reads), so nxxdvdfs's
  blocking reads opened the race window.
- **classpnp NP-pool.** DbgPacketLogs is ~50 KB of nonpaged pool per class FDO
  (disk + DVD ≈ 26 pages) → shrink it to 2 entries on Xbox (needed to reach the menu
  on the 64 MB map). ClassBadItems is a table of PC-OEM CD/DVD firmware-quirk
  workarounds (none apply to Xbox's Philips/Samsung/Thomson drives) → empty it to a
  single terminator.
- **DriverEntry is INIT-taggable** (its only pointer sits in discardable INITDATA
  tables and it runs once at boot) — unlike runtime IRP dispatch handlers, whose
  pointer sits in a resident DRIVER_OBJECT.
- **Fixed-topology invariants** (these justify the PnP cuts and are load-bearing if a
  path is ever reintroduced): Xbox storage never receives STOP/REMOVE/SURPRISE/QUERY
  PnP; MCPX IDE is dual-channel non-simplex (the serializing controller/simplex
  objects are dead); atapi hardcodes SRB_TYPE_SCSI_REQUEST_BLOCK (no extended SRB
  reaches classpnp/disk); the single permanently-referenced system process never
  loses its last thread and no second process is ever created (address-space teardown
  is unreachable).
- See the FSD extra-Vpb-ref, IoGetDmaAdapter, and static-device-tree / BusRelations
  notes in [`footguns.md`](footguns.md) and the memory index.

## HAL / hardware

- **8259 ELCR for PCI INTx.** Left at the ISA edge default, xemu treats PCI lines as
  edge-triggered; pbkit's vblank ISR toggling the line works *most* of the time but
  wedges on a momentary 0-read or a fast trap burst after N frames. Retail ELCR:
  master 0x68 / slave 0x18 (only IRQ3/5/6 + IRQ11/12 level; IRQ1 OHCI stays edge).
- **HalEndSystemInterrupt2 must ALWAYS restore the PIC mask** on IRQL-lower
  (KiI8259MaskTable[OldIrql] | IDR), not only when a soft-IRR is pending — else the
  IMR stays stuck at the elevated value HalpDismissIrq set and subsequent OHCI IRQ1
  raises sit masked in the IRR → USB transfers time out (-203).
- **PHY init runs at DISPATCH_LEVEL** (from the title's nvnet driver) → busy-wait via
  KeStallExecutionProcessor, NOT KeDelayExecutionThread (the IRQL ≤ APC_LEVEL assert
  produced 244k debug prints per session in Halo 2). The MCPX NIC uses xemu/xqemu's
  MII bit layout (INUSE = bit 15), NOT cromwell forcedeth's desktop-nForce layout
  (INUSE = 0x10000). The HAL bring-up subset leaves KPCR_STALL_SCALE_FACTOR = 0, so
  early PHY polling must use plain iteration counts, not KeStall.
- **The AV encoder is real SMBus/I2C.** AvSendTVEncoderOption composes the SMC AV-pack
  register + the EEPROM video-standard byte into the adapter|region result and drives
  the Conexant DACs via encoder regs 0xA8/0xAA/0xAC (encoder probed at 0x45/0x6A,
  SMBus at 0xC000). GET_SETTINGS must also pack the refresh-rate + HD-format capability
  flags into the high word (0x00400000 = 60 Hz NTSC + HD scan bits) or D3D's
  mode-validation rejects every 640x480 mode → E_FAIL → Halo 2 QuickReboots. AV_FLAGS
  bit positions come from cxbx-reloaded (WIDESCREEN = 16 … 60Hz = 22 … NORMAL = 0).
- **AvSetDisplayMode** writes the VGA CR[0x13/0x19/0x25] line-offset registers +
  CR[0x39] = 0xFF (interlace off) — xemu's display shader scales Y by these, so stale
  text-mode values make every fragment sample the bottom framebuffer row (a gradient)
  or double the height.
- **DPC-callable event APIs.** NtPulseEvent is called at DISPATCH by pbkit (it pulses
  its VBlank event), but NT's has PAGED_CODE + an Ob handle walk and bugchecks; cache
  the handle→KEVENT at PASSIVE-time create and go straight to KePulseEvent. PAGED_CODE()
  is neutralized to an empty *block* (not empty) so it stays valid at semicolon-less
  use sites.
- **SMBus / SMC specifics.** HalRead/WriteSMBusValue: the Xbox API uses an 8-bit write
  address (shift right 1 for the 7-bit slave); the dashboard probes TV encoders
  (0xd4/0xe0/0x8a) and the SMC at 0x20. HalReadSMCTrayState reads SMC reg 0x03's high
  nibble. MmClaimGpuInstanceMemory mirrors retail's top-64-KB-of-RAM instance region
  (0x83FF0000 / 0x10000) since pbkit reaches it via PRAMIN.
- **Timekeeping.** KeQueryPerformanceCounter must read the ACPI PM timer at PMBASE+0x08
  (24-bit, wrap-extended), NOT scale the PIT. The Xbox PM timer runs at 3,375,000 Hz
  (an xemu XBOX override; PCs use 3,579,545), but KeQueryPerformanceFrequency returns
  **3,374,488 Hz** to match retail's published constant (the 512 Hz drift is a one-time
  silicon calibration). The system clock ticks at **1 ms / 1 kHz** (not NT's ~15 ms) —
  nxdk's GetTickCount() returns KeTickCount raw and assumes the Xbox rate, so a wrong
  tick rate makes a title's "one second elapsed" math off by ~15x.

## Ordinals / ABI

- **CurrentThread at fs:[0x28].** The Xbox KPCR keeps CurrentThread inline at PCR+0x28
  (NT keeps it in PrcbData); titles run `mov eax, fs:[0x28]; mov edx, [eax+0x28]` to
  reach their KTHREAD shadow, so it must be written on every context switch.
- **Per-thread fs:[0x04] TLS.** The Xbox TLS pointer lives in Pcr→NtTib.StackBase,
  which is per-CPU not per-thread, so the cached value must be re-pushed in
  KiSwapContextExit (lock-free, at DISPATCH). Single-thread titles sneak through;
  multithreaded ones fault. fs:[0x04] points at the *end* of the TLS block, and the
  block's usable area (block+4) must be 16-byte aligned (nxdk's WinapiThreadStartup
  asserts it, but ExAllocatePoolWithTag only guarantees 8 → over-allocate and offset).
- **KPRCB+0x24C/0x250 must be zeroed on EVERY trap entry AND exit** (gated on the
  title-thread flag). Halo 2 reads +0x250 expecting NULL (retail's per-thread debug/FP
  pointer), but on NT's layout that offset lands inside KPROCESSOR_STATE's FXSAVE
  image, which interrupt/DPC activity keeps repopulating — so zeroing once at the
  trampoline or context-switch is insufficient.
- **Retail pseudo root handles.** (HANDLE)-3 = \?? (the DOS-devices namespace),
  (HANDLE)-4 = \Xbox (the Win32 named-object namespace — XAPI uses -4 for every named
  event/mutant/semaphore). NT's ObReferenceObjectByHandle rejects both with
  STATUS_INVALID_HANDLE, so map -4 → \Xbox and -3 → \??\ (constants from cxbx-reloaded's
  ob.h).
- **Title int3s are inter-function padding, not assertions.** A title int3 (e.g. Halo 2
  at EIP 0x121F6) is padding reached because an abort-path kernel call returned where
  retail diverts control. Dispatch it only when the title has its own SEH frame
  (recorded fs:[0] baseline in a KTHREAD tail field) — routing a padding-int3 into the
  system-thread catch-all wrapper kills the thread (it froze Halo CE workers). Note: on
  a kernel→kernel INT the CPU doesn't push SS:ESP, so TrapFrame→HardwareEsp is stale and
  dereferencing it recurses the bugcheck.
- **KeGetPreviousMode constant-folds to KernelMode** under SARCH_XBOX (ring-0 titles,
  no user entry), which collapses the ProbeForRead/Write + SEH arms across all 52 Nt*
  implementations (a correct side effect on Xbox: privilege/access-mode checks always
  take the maximally-privileged path).
- **The measured bad-pointer contract.** A bad-pointer fault is NEVER converted into a
  returned STATUS_ACCESS_VIOLATION — retail either succeeds without touching the buffer,
  lets the AV propagate into the title's SEH (at PASSIVE), or bugchecks 0xA/0x1E (read
  path / DISPATCH-level deref). Ring-0 titles get the fault either way, so the SEH-return
  arms were guarding a contract retail doesn't offer.
- **Object-type identity.** NtCreateMutant must use the real ExMutantObjectType (not
  ExEventObjectType with a KMUTANT body, else NtClose runs EVENT-type cleanup over it);
  ordinals 16/22/30 must forward the real Ex{Event,Mutant,Semaphore}ObjectType data
  exports (titles compare handle type-pointers); ExTimerObjectType stays stubbed (no
  ReactOS equivalent). Named-create falls back to anonymous for the unrooted names
  titles use ("mutex_N") that the NT object manager rejects.
- **KeSynchronizeExecution (ord 153)** must route through the shadow KINTERRUPT whose
  ActualLock is a real spinlock — the title's Xbox-shape KINTERRUPT leaves NT-offset
  0x1C (ActualLock) NULL and NT's implementation dereferences it.
- **stdcall @N link limits.** PE/COFF can't alias stdcall-decorated symbols at link
  time (ld --defsym, `#pragma redefine_extname`, and GAS `.set` all choke on the '@N'
  decoration) — that's why the Zw→Nt thunks existed; the fix is renaming at the call
  site, after which the SSDT drops entirely (ring-0 titles never take int 0x2e).
  SLIST_HEADER is layout-identical NT/Xbox but the Pop/Push ABI differs (Xbox is
  FASTCALL(ListHead) vs NT's separate-spinlock threading) → use an inline CMPXCHG8B loop
  (relinking librtl conflicts with ntoskrnl's own copy); the Interlocked ordinals 52–54
  are __fastcall, not stdcall.
- **Retail ABI constants.** XboxKrnlVersion@324 = {1, 0, 5530, 0} (titles gate on
  Major = 1); OBJECT_TYPE exports need a 28-byte Xbox shim with a printable .PoolTag at
  offset 0x18.
- The KDPC/KTIMER/KINTERRUPT/KTHREAD/OBJECT_ATTRIBUTES shadows, the DATA-ordinal strace
  rule, wait/APC alertability, the DbgPrint@8 no-op, KeBugCheckDebugBreak, and per-thread
  kernel-stack sizing are covered in [`struct-audit.md`](struct-audit.md),
  [`footguns.md`](footguns.md), and the memory index.

## Build / footprint

- **Compression is XZ + the x86 BCJ filter.** A 256 KB flash can't hold an LZ4 payload
  (no entropy coder; ~290 KB vs ~210 KB for XZ). zstd was rejected earlier for a 41 KB
  decoder (vs LZ4's ~1 KB); XZ+BCJ superseded LZ4 as the final choice.
- **LTO gotchas.** Don't naively -flto the kernel: gen-import-table's `nm -u` thunk
  discovery sees nothing in slim-LTO archives (`__imp_` references only materialize at
  ltrans) → it needs fat LTO objects + an ALWAYS_EMIT set, and the CRT / untyped-alias
  TUs must build `-fno-lto` (libcall materialization and type-merge conflicts). And
  ld's .def-driven export rooting stops working under the LTO plugin → every export
  without an internal caller is dead-stripped and its EAT slot holds RVA 0x7c000000 →
  NULL thunk → the first ordinal call jumps to 0 → double fault. Fix with a `-fno-lto`
  address-taking anchor (xb-export-roots.c) + a post-link .edata guard (fix-edata.py).
- **-Os, not -O2** on Xbox Release: the kernel is an IO-bound appliance (the heavy
  lifting is on the GPU/APU), so the code-size win beats the scalar-throughput penalty;
  non-Xbox arches stay -O2. Use `-ffunction-sections` only, NOT `-fdata-sections` — on
  GCC pei-i386 the latter emits each zero-init global into `.data$<name>` (CONTENTS=yes)
  instead of `.bss$<name>`, loading the whole BSS (~80 KB) from file and wiping the gain.
- **The custom linker script** (xbox-kernel.lds): PE output sections are page-aligned,
  so each tiny section costs a resident page → fold .xdata into .data and drop .idata
  (zero imports). The export directory is self-built post-link (redirecting *(.edata)
  into .rdata makes bfd over-reserve ~6 KB of NONAME name-table space); pefixup --kernel
  compacts it but must leave VirtualSize and DataDirectory[EXPORT].Size alone so the PE
  loader's contiguous-page invariant holds (runtime lookup uses NumberOfFunctions/Names,
  not Size).
- **Import-table minimization.** The in-kernel `__imp_` table must be filtered (via
  `nm -u` per library) to only the symbols the folded drivers actually consume —
  emitting a thunk for every ntoskrnl.def export (~1559) anchors whole subgraphs
  (oplocks, MCB, security, LPC) that --gc-sections would otherwise drop (−40 KB). Each
  thunk needs its own `.data$__imp_<sym>` section or --gc-sections can't drop any one in
  isolation. Drivers direct-bind via the tree-wide `_NTOSKRNL_/_NTHAL_/_NTSYSTEM_` macros.
- **INIT auto-placement and its UAF class.** `-ffunction-sections` on the kernel link
  makes per-function `.text$<fn>` survive LTO; a relink pass renames the init-only ones
  into `INIT$<fn>`. The soundness rule: only move a function if EVERY caller is
  INIT-or-movable-and-chosen — never relocate on the assumption that an unmovable
  resident caller will "reach INIT" (that's a resident→INIT reference = a UAF after
  MiFreeInitializationCode). `init-ref-check` gates it, because LTO can create one
  silently: constprop clones inherit the parent's compile-time section and the relink
  pass can't move them. init-audit must also only count an operand literal as a reference
  if it equals a discardable symbol's *entry* address — NTSTATUS/HRESULT constants
  (0x8007xxxx) otherwise collide with INIT-section VAs at base 0x80010000. And a manual
  `CODE_SEG("INIT")` on a driver's IRP-dispatch routine is a latent UAF (its address
  lives in a long-lived DRIVER_OBJECT and PnP can dispatch it post-boot on DVD
  media-change), so force runtime-installed entry points resident via a denylist. Note
  GCC honors a header declaration's CODE_SEG over the definition's — move the .h
  declarations too.
- **The INITDATA_RW linker section.** INITDATA folds into the read-only/exec INIT
  section (a section-type conflict), so writable boot-only globals need a separate input
  section (used for the eeprom shadow, MainPalette, HalpFakePci*).
- **--gc-sections at .rdata granularity.** Without -fdata-sections, the classpnp
  jump-table is kept alive transitively: ClasspEnableIdlePower's wide-string literals
  anchor the unnamed .rdata → which keeps the jump table → which keeps
  ClasspPowerDownCompletion. A load-bearing coupling to remember before "cleaning up" a
  table.
- **NLS.** Filesystem paths flow through ANSI_STRING, so the kernel never converts via
  codepage tables → drop CP-1252/CP-437 (−132 KB) with ASCII fast-paths, BUT keep the
  4.8 KB Unicode case table (FATX/ISO case-insensitive open, the object namespace, and
  wildcards all depend on it).
- **Symbol / flash mechanics.** `--strip-all` (not --strip-debug) is needed to drop the
  ~950 KB COFF symbol table the linker appends (keep xboxkrnl.unstripped.exe for
  nm-based tools). FileAlignment → 0x200 is safe because the loader's pe.c reads
  PointerToRawData verbatim (SectionAlignment stays 0x1000). The pci.ids VendorTable is
  1.5 MB of INITDATA for pretty PCI-dump names only (it falls back to "Unknown") → stub
  it, but wire the generated-header dependency via OBJECT_DEPENDS or a clean build races
  on the missing pci_classes.h. Flash is capped at 8 MB by xemu's xbox_flash_init (16 MB
  → fatal ROM overlap), and a RelWithDebInfo kernel (7.7 MB) exceeds it. `-Werror` was
  dropped (upstream only enables it for Debug and doesn't keep the code clean for it;
  `static` on a HAL function referenced from a non-static `inline` is C99-illegal).
- **kd release stubs.** NxkKdpStub must mirror KdpStub for the 4 kernel breakpoint
  commands (LOAD/UNLOAD_SYMBOLS, COMMAND_STRING, PRINT — all encoded as int3) or
  returning FALSE re-executes the int3 → infinite recursion → silent release hang. Don't
  gate the kd64 protocol on SARCH_XBOX inside kdapi.c/kdio.c — the release/DBG split
  lives in ntos.cmake.
- **Stacks.** The double-fault stack only ever runs the bugcheck path (plus a pre-Phase-1
  DpcStack placeholder), so it shrinks to a guard page + one usable page; the
  page-aligned boot/DF stacks must live in a non-LTO TU with .bss sorted by alignment, or
  LTO scatters ~7 KB of holes through the section.

## Testing

- **api-regression harness shape.** It emits TAP over the isa-debugcon port (0xE9), NOT
  DbgPrint, so the same XBE can run against retail bios.bin as an oracle; the io/file
  tests use the raw `\Device\Harddisk0\Partition1` path (e:\ isn't mounted when booting
  straight off a DVD); it halts via HalHaltRoutine to emit one clean TAP block.
- **Oracle-validated contract corrections.** Running against bios.bin caught several
  false assumptions: both kernels DO deliver the sync-write completion APC at wait entry
  (the "user-APC not implemented" conclusion was wrong); Sleep(0) legitimately returns
  STATUS_NO_YIELD_PERFORMED; retail rejects a 12-star directory mask with
  STATUS_INVALID_PARAMETER; create_with_explicit_stack needs a non-NULL SystemRoutine
  (retail enters threads through XapiThreadStartup); and retail does NOT zero-fill the gap
  when EOF is extended (stale cluster bytes are observable).
