/*
 * Xbox-ordinal strace.  Flip XB_STRACE to 1 to wrap every kernel-ordinal
 * thunk in a trampoline that logs the call before forwarding.  Disabled by
 * default; the wrap helpers fold to no-ops so the call sites stay clean.
 */
#pragma once

#define XB_STRACE  0

#if XB_STRACE
PVOID XbStraceWrap(ULONG Ordinal, PVOID RealFn);
VOID  XbStraceValidatePool(_In_ PCSTR Where);
#else
static __inline PVOID XbStraceWrap(ULONG Ordinal, PVOID RealFn)
{
    UNREFERENCED_PARAMETER(Ordinal);
    return RealFn;
}
static __inline VOID XbStraceValidatePool(_In_ PCSTR Where)
{
    UNREFERENCED_PARAMETER(Where);
}
#endif

/*
 * Per-call syscall/API trace.  Folds to nothing unless XB_STRACE is on, so the
 * detailed Xbox-side traces (NtCreateFile/NtOpenFile/NtFsControlFile, ...) stay
 * out of the default DBG serial log but come back in an strace build.  Expands
 * to XbDbg, so it is silent in release regardless.  (Callers include
 * <xb-debug.h>.)
 */
#if XB_STRACE
#define XbStraceDbg(...)  XbDbg(__VA_ARGS__)
#else
#define XbStraceDbg(...)  ((void)0)
#endif
