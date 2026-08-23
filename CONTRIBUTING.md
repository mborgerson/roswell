# Contributing to nxkrnl

`nxkrnl` is a from-scratch, **clean-room** reimplementation of the original
Xbox kernel. Its legality and credibility depend on every contributor
following the clean-room discipline below. Please read this before writing
any code.

## Clean-room policy

nxkrnl is built only from sources we are confident are legally clean.

### Permitted sources

- **ReactOS** — an independent clean-room reimplementation of the Windows NT
  kernel (GPL-2.0). Primary derivation base for the NT-analog subsystems
  (`Ke`/`Ex`/`Ob`/`Io`/`Ps`/`Mm`/`Rtl`).
- **cromwell** — the community legal Xbox BIOS replacement (GPL-2.0).
- **nxdk** — the open Xbox homebrew SDK (MIT). Its `xboxkrnl.h` is a clean,
  community-reverse-engineered declaration of the kernel API — function
  signatures and ordinal numbers.
- **xemu / XQEMU** source — documents the behavior of the emulated hardware.
- Public community documentation — xboxdevwiki and public homebrew docs.

### Forbidden sources

Do not read, search for, copy from, or transcribe:

- **The Microsoft XDK** — headers, libraries, samples, or documentation.
- **Leaked Microsoft source code** of any kind.
- **Microsoft debug symbols or PDBs** for the Xbox kernel.

### Interface facts vs. implementation

The kernel **API contract** — ordinal numbers, function signatures, struct
layouts — is an interface fact, like any documented API, and may be used from
clean documentation. The **implementation behind it** must be original work
or derived from a permitted source.

## Licensing

nxkrnl is **GPL-2.0** as a combined work (see [COPYING](COPYING)): the
kernel links GPL-2.0-only code (the xbox-linux-derived TV-encoder drivers
in `hal/halx86/xbox/video/`), so the whole cannot be offered "or later".
New first-party files should still carry `GPL-2.0-or-later` headers —
that grant maximizes reuse and stays compatible with the v2-only
combination.  Code ported from an outside source keeps that source's
license in its header; check whether the original says "or later" before
labeling it.  Standalone tooling and test programs may carry a more
permissive license where explicitly noted.

## Commit & branch conventions

- One logical change per commit, in **Conventional Commits** style scoped to
  the subsystem: `kernel(mm): ...`, `hal(smbus): ...`, `ldr: ...`,
  `build: ...`, `docs: ...`.
- Branch per feature; PR-style review; `tools/verify` must pass before a
  change lands.
- Update `docs/` in the **same commit** as the code it describes.

## Vendored ReactOS files

When a vendored file's body is entirely unused on `SARCH=xbox`, don't
`git rm` it — unlink it from the build (remove it from `ntos.cmake` or its
subsystem's `CMakeLists`) instead. Deleting breaks the upstream-merge story,
`--gc-sections` already drops the unused code from the image, and keeping the
file makes our diff-vs-upstream reviewable — any vendored file we touched
should carry a small, surgical diff, not "deleted entire file."
`tools/dead-objs.py` finds `.c` files that compile in but contribute zero live
symbols; if one you edited turns out to be unlinked, revert the edit.
