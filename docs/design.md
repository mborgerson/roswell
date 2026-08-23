# nxkrnl — A Free, Open-Source Kernel for the Original Xbox

**Goal:** Build a clean, GPL-licensed replacement for the original Xbox kernel
(`xboxkrnl.exe`) that can load and run Xbox executables (XBEs), starting with
homebrew/unsigned titles. Bootstrap development and testing in
[xemu](https://xemu.app); real-hardware fidelity is an eventual goal.

`nxkrnl` is a **ReactOS fork** (this repo) carrying the Xbox-specific work
under root layout. The kernel core (`Ke`/`Ex`/`Ob`/`Io`/`Ps`/`Mm`/`Rtl`) and the
Xbox HAL come from ReactOS upstream; we strip ReactOS down to what an XBE
needs and extend with the Xbox-specific pieces (the XBE loader, the ~366
`xboxkrnl` ordinal exports, the Xbox memory model, the missing HAL bits). See
[`memory-map.md`](memory-map.md) for the memory model.

---

## 1. Goal & scope

### 1.1 What we are building

`nxkrnl` is a from-scratch implementation of the original Xbox operating-system
kernel. On a real console the kernel is the small (~350 KB) NT-derived image
the MS BIOS hands control to; its job is to bring up the hardware, then load
and execute `default.xbe`. We are reimplementing that component.

The Xbox kernel is attractive to reimplement because:

- **The API is narrow.** It exports ~366 ordinals, and any single title uses
  only a fraction of them. The exact set is discoverable per-title.
- **The execution model is simple.** One process, a flat-ish memory map, no
  user/kernel privilege split for titles, cooperative-ish scheduling with a
  thin NT-style kernel underneath. There is no Win32 subsystem, no registry
  hive (from the title's perspective), no driver-loading framework to emulate.
- **The Xbox kernel is itself a fork of the Windows 2000 kernel.** That means a
  clean-room NT reimplementation — ReactOS — is a legally clean, structurally
  faithful base for the `Ke`/`Ex`/`Ob`/`Io`/`Ps`/`Mm`/`Rtl` subsystems.

### 1.2 In scope (near-term)

- Boot, hardware bring-up, and kernel core on Xbox 1.0–1.6 hardware as emulated
  by xemu. The kernel currently runs on the **128 MB devkit layout** (the
  shipping default, high IMAGEBASE); fitting the **64 MB retail-base layout**
  is the outstanding footprint goal (§5, [`footprint.md`](footprint.md)).
- Loading and running **XBEs** — homebrew (nxdk-/OpenXDK-built) and retail
  alike (see §5 for the titles running today).
- Enough of the kernel API surface to run real titles: graphics via the NV2A,
  input via USB, audio, FATX file I/O, persistent settings, save data.

**Eventual goals.** xemu is the bootstrap environment that lets us move fast
now — *not the ceiling*. Beyond the near-term scope the project explicitly
aims at:

- **Real-hardware fidelity.** The kernel should eventually boot and run on
  actual Xbox 1.0–1.6 consoles, not only under xemu. xemu is how we iterate
  today; correctness against real silicon is the long-term bar.
- **Broad retail-title compatibility.** A handful of retail titles already
  run (§5); running them *at large* is the long-tail goal — it exercises far
  more of the API surface, plus region/media certificate logic and save-data
  signing. On legality: retail XBEs are RSA-*signed* but *not encrypted*, so
  running one in our own kernel is interoperability — directly analogous to
  Wine running Windows binaries, or to xemu itself running retail Xbox games.
  There is no encryption to break, no key to extract, no signature to forge:
  our kernel merely *parses* the signature field and, being our own software,
  chooses not to *enforce* it — it never *defeats* a measure in someone else's
  work. The clean-room rules of §2 apply unchanged.

### 1.3 Out of (initial) scope

- Flashing real hardware as the *development* workflow. Day-to-day iteration
  is xemu-based; real-hardware bring-up belongs to the eventual-goal track,
  not the near-term loop.
- 100 % API coverage on day one — coverage grows demand-driven (§4.1).
- Scope creep into a general-purpose OS — the charter is *run XBEs*, not run
  Windows applications.

### 1.4 Definition of success

A test XBE built with [nxdk](https://github.com/XboxDev/nxdk) boots under
xemu on **our** kernel — MCPX boot ROM → `nxldr` → `ntoskrnl` → XBE entry
point — and produces observable output (framebuffer image and/or debug log)
matching a run on the official kernel.

This is achieved (see §5). Breadth work continues from here.

---

## 2. Clean-room discipline (read this first)

This project must remain **untainted**. Our credibility and the legality of
the result depend on it. The rules below are not optional.

### 2.1 Permitted sources (clean)

| Source | License | Use |
|---|---|---|
| **ReactOS** (`./`) | GPL-2.0 (files mostly GPL-2.0-or-later; a few v2-only, plus BSD/LGPL/MIT pieces) | Independent clean-room NT reimplementation. Primary derivation base for `Ke/Ex/Ob/Io/Ps/Mm/Rtl/Hal`. |
| **Cromwell** (historical reference) | Cromwell-original code GPL-2.0-or-later; its TV-encoder drivers came from xbox-linux and are GPL-2.0 only | Community legal BIOS replacement. Used as the dev bootloader during early bring-up; replaced by our own `boot/nxldr/`. PCI bring-up + video drivers were ported into `hal/halx86/xbox/` (the encoder drivers pin the combined kernel to GPL-2.0); GPL FATX/IDE/SMBus code remains a clean reference. |
| **nxdk** | MIT | Builds our test XBEs. Its `xboxkrnl.h` is a clean, community-reverse-engineered **API declaration** reference (signatures + ordinals — interface facts, not implementation). |
| **xemu / XQEMU** | GPL-2.0 / MIT | Emulator source documents hardware *behavior* (registers, timing). Clean reference. Also a black-box oracle: booting the official kernel under xemu and reading its live page tables / device BARs via the monitor is permitted observation (§2.2), not disassembly. |
| **cxbx-reloaded** | GPL-2.0 | Clean-room community Xbox emulator (HLE). Documents the Xbox memory map, kernel API contract, and hardware behaviour. |
| **xboxdevwiki.net**, caustik's XBE docs, public homebrew docs | Community docs | Documentation of formats and the API contract. Clean. |

### 2.2 Forbidden sources (would taint the project)

- **The official kernel binary** (`bios.bin`). It may be used **only as a
  black-box behavioral oracle** — observe inputs/outputs of a running system.
  It must **never** be disassembled, decompiled, single-stepped to copy logic,
  or transcribed.
- **Leaked Microsoft source code** of any kind. Do not read it. Do not search
  for it.
- **The Microsoft XDK** — headers, libraries, samples, documentation. Do not
  use any XDK material.
- MS debug symbols / PDBs for the kernel.

### 2.3 Process rules

- Every file derived from ReactOS or cromwell **keeps its original copyright
  header** and adds ours.
- The API *contract* (ordinal numbers, function signatures, struct layouts)
  is an interface fact, like any documented API, and is fine to use from
  clean docs (nxdk, xboxdevwiki). The *implementation behind it* must be
  original or ReactOS-derived. This distinction is the core of the discipline.
- A/B testing against the official kernel compares **observable behavior**
  only. The moment a question is "how does the official kernel implement X
  internally," the answer is: don't look — derive it from NT semantics
  (ReactOS) and the public docs.

---

## 3. How the Xbox boots and runs an XBE

Understanding the target. (Verify all hardware specifics against xemu source
and xboxdevwiki — never against `bios.bin`.)

### 3.1 The retail boot chain

```
MCPX boot ROM (mcpx.bin)  →  2BL (in bios.bin, encrypted)  →
xboxkrnl.exe (in bios.bin) → loads default.xbe → title runs
```

The kernel is a PE image loaded at virtual `0x80010000`. It initializes the
NV2A GPU, the MCPX southbridge (LPC, SMBus, USB OHCI, nForce NIC, APU/ACI
audio), sets up NT-style paging and interrupts, then loads `default.xbe`.

### 3.2 Our boot chain

We replace the MS BIOS with our own minimal loader plus the kernel image
packed into a 1 MB flash:

```
mcpx.bin → nxldr → xboxkrnl.exe → loads default.xbe
       (BIOS replacement) (the kernel)
```

`boot/nxldr/gen-flash-bin.py` assembles `build/flash.bin`: the `nxldr`
binary (with the Xcodes that MCPX's interpreter runs, and the reset
vector at the top of flash) wraps a 0xff-filled hole that the script
fills with the xz-compressed kernel.  At reset MCPX hands control to
`nxldr`, which decompresses the kernel into RAM and PE-maps it -- no
third-party bootloader, no separate stage.

`tools/run-xemu --flash --dvd <title.iso>` boots the chain. The kernel
mounts the DVD via the in-kernel XDVDFS or CDFS driver and runs
`default.xbe` from the disc.

### 3.3 The XBE format and the kernel/title contract

An XBE (public format; see xboxdevwiki) is a PE-like container:

- `"XBEH"` magic; base address (usually `0x00010000`); section table with
  per-section virtual addr/size, raw addr/size, and flags.
- **Entry point** and **kernel-thunk-table pointer** are stored XOR-encoded.
  Two key sets exist (retail vs. debug); the loader tries both and accepts
  the one yielding an in-image address. nxdk titles use the retail keys.
- A **TLS directory** (the kernel allocates the TLS block and wires it into
  the thread's TEB, NT-style, reachable via `fs:`).
- A **kernel thunk table**: an array of `u32`. If an entry has bit 31 set,
  the low bits are a **kernel export ordinal**; the kernel overwrites the
  entry in-place with the absolute address of its implementation of that
  ordinal.

This is the load-time linkage mechanism. The title never learns the kernel's
base; the kernel patches every thunk.

**The kernel carries a real PE export directory.** The real `xboxkrnl.exe`
exports its ~371 ordinals through a standard PE export directory, and a title
may walk it to resolve an import after load time — so nxkrnl matches that:
`xboxkrnl.exe` exports the ordinals (NONAME, generated by
`ntoskrnl/gen-nxkrnl-exports.py` from nxdk's `xboxkrnl.exe.def`), and the XBE
loader (`ntoskrnl/xbe.c`) resolves the thunk table against that export
directory.

The kernel maps the XBE's sections at its preferred base, sets up TLS + the
main thread's TEB, resolves thunks, and calls the entry point on that thread.

---

## 4. Architecture & approach

### 4.1 Strategy: derive, strip, extend

The Xbox kernel ≈ "Windows 2000 kernel, stripped down, plus Xbox-specific
modules." We mirror that:

1. **Derive** `Ke / Ex / Ob / Io / Ps / Mm / Rtl` from ReactOS `ntoskrnl`.
   They have direct NT analogs; ReactOS gives clean GPL implementations.
2. **Strip** everything titles don't need: the loadable-driver model and
   KMDF, the registry hive *files*, Win32k, plug-and-play, multi-process
   security, the NT native API surface beyond the Xbox subset.
3. **Extend** with Xbox-specific pieces that have *no* NT analog and must be
   written fresh (clean) from public docs:
   - The **XBE loader** (`ntoskrnl/xbe.c`, replaces NT's PE image loader for
     titles).
   - The **Xbox HAL bits ReactOS doesn't already have**: NV2A access,
     SMBus + SMC (power/LED/tray/temp), nForce NIC + Ethernet PHY. ReactOS
     already has the Xbox HAL skeleton (`hal/halx86/xbox/`), which is a
     clean GPL reference for hardware bring-up.
   - Xbox API families: `Av*` (video), `Xc*` (crypto), `Xe*`/`Ex*NonVolatile`
     (EEPROM settings), `Hal*` Xbox extensions, the save/mounting paths.
   - The Xbox memory model and the kernel **ordinal export table**.

API coverage grows **demand-driven**: pick a target homebrew (or test XBE),
dump its thunk table, implement what's missing, repeat. Unimplemented
ordinals resolve to a self-naming bugcheck stub
(`nxkrnl-exports.{c,spec}` generated by `gen-nxkrnl-exports.py`) so coverage
gaps are loud and obvious.

### 4.2 Image and boot delivery

The kernel is a PE/COFF image (`xboxkrnl.exe`), linked at IMAGEBASE
`0x80010000` (retail Xbox kernel placement). PE is the format ReactOS's
headers and code expect, and we derive heavily from ReactOS, so the whole
kernel builds as PE.

Boot delivery is **flash-resident**: `boot/nxldr/gen-flash-bin.py` injects
the xz-compressed `xboxkrnl.exe` into a hole inside the `nxldr` binary.
At reset MCPX hands control to `nxldr`, which decompresses the kernel into
RAM at the chosen physical base and PE-maps it -- no CD read, no separate
loader stage.

Cross-toolchain: **`i686-w64-mingw32` GCC** — ReactOS's
`toolchain-gcc.cmake`. System Clang (≥ 22) does not work; it dropped the
legacy MMX builtins ReactOS's bundled intrinsics headers depend on.

### 4.3 Target hardware assumptions

Retail Xbox 1.0–1.6 as emulated by xemu: Pentium III "Coppermine" class i686
(SSE1, no SSE2 — compile accordingly), 64 MB unified RAM, NV2A GPU, MCPX
southbridge. 128 MB devkit RAM is detected and used by the current build
(the kernel's IMAGEBASE temporarily sits above the low 64 MB; see
[`memory-map.md`](memory-map.md)).

All hardware facts are taken from xemu source + xboxdevwiki, never from
`bios.bin`. xemu is the *near-term* reference because it is what we iterate
against fastest; real silicon is the eventual correctness bar (§1.2), so
where xemu and xboxdevwiki disagree we treat xemu-specific quirks as suspect
rather than canonical.

---

## 5. Status

The headline milestone (§1.4) is met: real titles boot and run on the
nxkrnl chain under xemu, driving their own runtime file I/O against the
kernel's native XDVDFS FSD. Title high-water marks observed end-to-end
(windowed xemu, 128 MB devkit layout):

| Title | Status |
|---|---|
| Halo CE | Main menu + campaign gameplay, input + audio |
| Halo 2 | First-level gameplay |
| api-regression test XBE | full suite passing (retail-validated) |

KeBugCheck renders a working bluescreen, audio runs through mcpx_apu,
input through OHCI. Subsystem detail lives with the code: the memory
model in [`memory-map.md`](memory-map.md), the kernel-visible struct ABI
(KDPC/KTHREAD/KINTERRUPT/OBJECT_ATTRIBUTES/CRITICAL_SECTION shadow
adapters, with KAPC/ERWLOCK pending a title that needs them) in
[`struct-audit.md`](struct-audit.md), and the footprint work in
[`footprint.md`](footprint.md).

### Matching the retail kernel footprint

Everything above runs on the **high devkit-base layout** (kernel
IMAGEBASE `0x84000000`, 128 MB), which cedes all of low RAM to the title.
At the **retail base** (`0x80010000`, 64 MB) the kernel's runtime page
footprint doesn't yet fit below the contiguous pool titles hardcode at
PA `0x61000`, so titles misbehave there — Halo CE and Halo 2 both do.
Matching the retail kernel's *runtime* page footprint — the count of
resident pages, not the compressed image that already fits flash — is
the main open item and the unblock for the retail-base move. Budget,
methodology, and the cut queue live in [`footprint.md`](footprint.md).

Other open work:

- **Long-tail ordinal coverage.** As new titles surface new imports, the
  scaffold bugchecks name the ordinal; each gets implemented in turn.
- **Write-path AV recurses to a double fault.** A title handing a
  garbage buffer to `NtWriteFile` (len > 0) should AV into the title's
  SEH chain like retail does (measured; see
  `tests/xbe/api-regression/io/bad_ptr.c`); on nxkrnl the exception
  dispatch re-faults repeatedly until the kernel stack double-faults
  (#DF at ~64 KB used).  Independent of the PSEH choice; likely the
  dispatcher faulting inside its own unwind path.  Reproduce with the
  compile-gated `BADPTR_FATAL_PROBES` rows in that test file.
- **Retire the residual ARM3 bring-up.** `Mm` is already a from-scratch
  rewrite (nxmm, `ntoskrnl/xb/mm/`) that owns its own allocators and VA
  windows and displaced ARM3's pool/VM/fault/cache; only ARM3's boot
  bring-up still runs, and retiring it is the last mile. See
  [`footguns.md`](footguns.md) for why the *in-place* approach failed.

---

## 6. Versioning

nxkrnl has its own release version in the root `VERSION` file (the single
source of truth), tagged `vX.Y.Z`. It follows **SemVer remapped for a
kernel**: we don't own the Xbox ABI, so there is no "breaking API change"
axis — the numbers track *maturity / capability*.

- **`0.x` while it isn't ready for prime time** (where it is today). `MINOR`
  marks capability milestones (titles under xemu → real hardware → broad
  retail-title compat); `PATCH` is fixes.
- **`1.0.0`** is reserved for a defined bar — runs a target set of titles
  reliably on real hardware — not reached until we can name and clear it.

Two other version numbers are deliberately **separate** from the release
version, and must not be coupled to it:

- **`XboxKrnlVersion` (ordinal 324)** is a *retail-compatibility constant*.
  It mimics a real retail kernel build (`Major` stays 1; titles gate on it)
  and is a per-title-tunable knob — see §3.3 for the ordinal ABI it belongs to.
- **Build identity** for bug reports is the release version plus the short
  git commit (`ntoskrnl/xb/gen-version.py`), shown on the boot-logo overlay
  and the debug serial at boot.
