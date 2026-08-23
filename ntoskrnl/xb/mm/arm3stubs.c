/*
 * PROJECT:     nxkrnl (ntoskrnl)
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Bugchecking stubs for ARM3 routines whose definitions the
 *              nxmm configuration removes from the build.
 *
 * A few retained ARM3/ps callers still reference these routines from
 * branches that cannot execute on this configuration (section-backed
 * process creation, teardown of the last process).  LTO links fold
 * those branches away, but a plain link needs the symbols to resolve.
 * Bugchecking here keeps the "unreachable" assumption loud if it is
 * ever wrong.
 */

#include <ntoskrnl.h>
#include <debug.h>
#include <mm/ARM3/miarm.h>

#ifdef NXK_MM_VM
VOID
NTAPI
MiDeleteVirtualAddresses(
    _In_ ULONG_PTR Va,
    _In_ ULONG_PTR EndingAddress,
    _In_opt_ PMMVAD Vad)
{
    KeBugCheck(MEMORY_MANAGEMENT);
}
#endif

#if defined(NXK_MM_VM) && defined(NXK_MM_CACHE)
VOID
NTAPI
MiRemoveNode(
    IN PMMADDRESS_NODE Node,
    IN PMM_AVL_TABLE Table)
{
    KeBugCheck(MEMORY_MANAGEMENT);
}
#endif

#ifdef NXK_MM_CACHE
VOID
NTAPI
MiRemoveMappedView(
    IN PEPROCESS CurrentProcess,
    IN PMMVAD Vad)
{
    KeBugCheck(MEMORY_MANAGEMENT);
}

VOID
NTAPI
MiRosCleanupMemoryArea(
    PEPROCESS Process,
    PMMVAD Vad)
{
    KeBugCheck(MEMORY_MANAGEMENT);
}

PFILE_OBJECT
NTAPI
MmGetFileObjectForSection(
    IN PVOID Section)
{
    KeBugCheck(MEMORY_MANAGEMENT);
    return NULL;
}

NTSTATUS
NTAPI
MmMapViewOfSection(
    IN PVOID SectionObject,
    IN PEPROCESS Process,
    IN OUT PVOID *BaseAddress,
    IN ULONG_PTR ZeroBits,
    IN SIZE_T CommitSize,
    IN OUT PLARGE_INTEGER SectionOffset OPTIONAL,
    IN OUT PSIZE_T ViewSize,
    IN SECTION_INHERIT InheritDisposition,
    IN ULONG AllocationType,
    IN ULONG Protect)
{
    KeBugCheck(MEMORY_MANAGEMENT);
    return STATUS_NOT_IMPLEMENTED;
}
#endif
