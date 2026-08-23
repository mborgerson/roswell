/*
 * PROJECT:     nxkrnl (ntoskrnl)
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     nxmm fault handler -- faults are bugs, not work.
 *
 * With eager commit (nxvm), the fixed-pool cache (nxcache), nonpaged
 * pool (nxpool), and no sections or paging, no legitimate not-present
 * or protection fault exists: every valid VA is mapped at creation.
 * The only benign case kept is the kernel-PDE sync check.  Everything
 * else reports failure and lets the exception machinery produce the
 * existing diagnostics (trap0E text-address dump, bugcheck on
 * unhandled kernel exceptions).
 */

/* INCLUDES *****************************************************************/

#include <ntoskrnl.h>
#define NDEBUG
#include <debug.h>

#define MODULE_INVOLVED_IN_ARM3
#include <mm/ARM3/miarm.h>

/* mm/i386/page.c -- lazily syncs a kernel PDE; TRUE if it fixed one.
 * (cdecl: defined without NTAPI.) */
BOOLEAN Mmi386MakeKernelPageTableGlobal(PVOID Address);

/* FAULT ENTRY **************************************************************/

NTSTATUS
NTAPI
MmAccessFault(
    IN ULONG FaultCode,
    IN PVOID Address,
    IN KPROCESSOR_MODE Mode,
    IN PVOID TrapInformation)
{
    UNREFERENCED_PARAMETER(Mode);
    UNREFERENCED_PARAMETER(TrapInformation);

    /* Lazily synced kernel page-directory entry: the one benign case. */
    if ((ULONG_PTR)Address >= (ULONG_PTR)MmSystemRangeStart)
    {
        if (Mmi386MakeKernelPageTableGlobal(Address))
            return STATUS_SUCCESS;
    }

    DPRINT1("va=%p code=%lx ip=%p -- no fault is legitimate here\n",
            Address, FaultCode,
            TrapInformation ? (PVOID)((PKTRAP_FRAME)TrapInformation)->Eip
                            : NULL);

#if DBG && defined(NXK_MM_POOLPAGES)
    {
        ULONG NxPoolPagesLastFreeRa(PVOID);
        ULONG Ra = NxPoolPagesLastFreeRa(Address);
        if (Ra != 0)
            DPRINT1("pool window page last freed by RA=%08lx\n", Ra);
    }
#endif

    return (FaultCode & 1) ? STATUS_ACCESS_VIOLATION
                           : STATUS_ACCESS_VIOLATION;
}

/* VA -> PA *****************************************************************/

/* Walks the live page tables; handles the 4 MB PSE windows (KSEG0, WC,
 * MMIO) and 4 KB mappings alike.  Zero for anything unmapped. */
PHYSICAL_ADDRESS
NTAPI
MmGetPhysicalAddress(
    IN PVOID Address)
{
    PHYSICAL_ADDRESS Pa = { .QuadPart = 0 };
    PMMPDE Pde = MiAddressToPde(Address);

    if (Pde->u.Hard.Valid)
    {
        if (Pde->u.Hard.LargePage)
        {
            Pa.QuadPart = (Pde->u.Long & 0xFFC00000UL) |
                          ((ULONG_PTR)Address & 0x003FFFFFUL);
        }
        else
        {
            PMMPTE Pte = MiAddressToPte(Address);
            if (Pte->u.Hard.Valid)
            {
                Pa.QuadPart = (Pte->u.Long & ~(ULONG)(PAGE_SIZE - 1)) |
                              ((ULONG_PTR)Address & (PAGE_SIZE - 1));
            }
        }
    }
    return Pa;
}

/* RESIDUAL ARM3/ROSMM LINK SURFACE *****************************************/

/* Globals init code writes; nothing reads them meaningfully anymore.
 * (MmNumberOfPagingFiles feeds the DBG-tree debugger data block.) */
ULONG MmNumberOfPagingFiles;
KEVENT MmWaitPageEvent;
KGUARDED_MUTEX MmSectionCommitMutex;
KGUARDED_MUTEX MmSectionBasedMutex;
PVOID MmHighSectionBase;
ULONG_PTR MmSubsectionBase;
PMMWSL MmWorkingSetList;

/* Static kernel memory-area markers: the marea tree is gone, and the
 * only caller asserts on the status and discards the area. */
NTSTATUS
NTAPI
MmCreateMemoryArea(
    PMMSUPPORT AddressSpace,
    ULONG Type,
    PVOID *BaseAddress,
    SIZE_T Length,
    ULONG Protection,
    PMEMORY_AREA *Result,
    ULONG AllocationFlags,
    ULONG AllocationGranularity)
{
    UNREFERENCED_PARAMETER(AddressSpace);
    UNREFERENCED_PARAMETER(Type);
    UNREFERENCED_PARAMETER(BaseAddress);
    UNREFERENCED_PARAMETER(Length);
    UNREFERENCED_PARAMETER(Protection);
    UNREFERENCED_PARAMETER(AllocationFlags);
    UNREFERENCED_PARAMETER(AllocationGranularity);
    if (Result) *Result = NULL;
    return STATUS_SUCCESS;
}

/* Driver/INIT unmap: pages stay mapped -- their VA is never reused, and
 * the INIT reclaim runs through the title-heap donation first. */
PFN_COUNT
NTAPI
MiDeleteSystemPageableVm(
    IN PMMPTE PointerPte,
    IN PFN_NUMBER PageCount,
    IN ULONG Flags,
    OUT PPFN_NUMBER ValidPages)
{
    UNREFERENCED_PARAMETER(PointerPte);
    UNREFERENCED_PARAMETER(PageCount);
    UNREFERENCED_PARAMETER(Flags);
    if (ValidPages) *ValidPages = 0;
    return 0;
}

/* With one page directory there is never a PDE to sync here; reaching
 * this means the page table genuinely is not present. */
NTSTATUS
FASTCALL
MiCheckPdeForPagedPool(
    IN PVOID Address)
{
    UNREFERENCED_PARAMETER(Address);
    return STATUS_ACCESS_VIOLATION;
}

VOID
NTAPI
MiInitializeWorkingSetList(
    _Inout_ PMMSUPPORT WorkingSet)
{
    UNREFERENCED_PARAMETER(WorkingSet);
}

BOOLEAN
NTAPI
MiInitializeSystemSpaceMap(
    IN PMMSESSION InputSession OPTIONAL)
{
    UNREFERENCED_PARAMETER(InputSession);
    return TRUE;
}

BOOLEAN
NTAPI
MmIsFileObjectAPagingFile(
    PFILE_OBJECT FileObject)
{
    UNREFERENCED_PARAMETER(FileObject);
    return FALSE;
}

/* FSD truncation/deletion checks: no image or data sections exist. */
BOOLEAN
NTAPI
MmFlushImageSection(
    IN PSECTION_OBJECT_POINTERS SectionObjectPointer,
    IN MMFLUSH_TYPE FlushType)
{
    UNREFERENCED_PARAMETER(SectionObjectPointer);
    UNREFERENCED_PARAMETER(FlushType);
    return TRUE;
}

BOOLEAN
NTAPI
MmCanFileBeTruncated(
    IN PSECTION_OBJECT_POINTERS SectionObjectPointer,
    IN PLARGE_INTEGER NewFileSize)
{
    UNREFERENCED_PARAMETER(SectionObjectPointer);
    UNREFERENCED_PARAMETER(NewFileSize);
    return TRUE;
}

VOID
NTAPI
MmInitializeRmapList(VOID)
{
}

NTSTATUS
NTAPI
MmInitSectionImplementation(VOID)
{
    return STATUS_SUCCESS;
}

VOID
NTAPI
MmInitPagingFile(VOID)
{
}

VOID
NTAPI
MmInitializeBalancer(
    ULONG NrAvailablePages,
    ULONG NrSystemPages)
{
    UNREFERENCED_PARAMETER(NrAvailablePages);
    UNREFERENCED_PARAMETER(NrSystemPages);
}
