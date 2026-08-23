/*
 * PROJECT:     nxkrnl (ntoskrnl)
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     IoSpace, VA<->PA conversion, IsAddressValid, SetAddressProtect.
 *
 * Constant-time helpers that exploit the KSEG0 identity window.  The WC /
 * MMIO / KSEG0 windows are already painted by xb/mm/init.c, so MmMapIoSpace
 * just picks the right base for the requested PA + cacheability.
 */

/* INCLUDES *****************************************************************/

#include <ntoskrnl.h>
#define NDEBUG
#include <debug.h>
#include <xb-debug.h>

#include "mm.h"

/* IS-ADDRESS-VALID *********************************************************/

/* BOOLEAN MmIsAddressValid(PVOID Va).
 *
 * Retail returns TRUE if the page is mapped + present (i.e. a load/store
 * would not fault).  Our address space is statically painted at boot,
 * so the question reduces to "is this VA inside one of our windows?"
 * Anything in the kernel image, KSEG0 alias, WC alias, UC MMIO, or the
 * sysmem aperture is always present; anything else is treated as not
 * mapped. */
BOOLEAN
NTAPI
NxMmIsAddressValid(IN PVOID Va)
{
    ULONG_PTR V = (ULONG_PTR)Va;

    if (V >= NXK_KSEG0_BASE && V < NXK_KSEG0_BASE + NXK_KSEG0_RAM_SIZE)
        return TRUE;
    if (V >= NXK_WC_BASE && V < NXK_MMIO_LIMIT)
        return TRUE;
    if (V >= NXK_SYSMEM_BASE && V < NXK_SYSMEM_LIMIT)
        return TRUE;
    /* PTE-mapped VAs (pool, title commits): walk the page tables, like
     * retail does -- mm/bigpool pins live pool TRUE / freed pool FALSE. */
    return MmIsAddressValid(Va);
}

/* VA -> PA *****************************************************************/

/* ULONG_PTR MmGetPhysicalAddress(PVOID Va).  Constant-time for the KSEG0
 * alias / device MMIO / WC RAM aperture; falls back to walking the page
 * tables for VAs outside our static windows (pool, title commits, and the
 * demand-mapped 0xD0000000 system-VA aperture). */
ULONG_PTR
NTAPI
NxMmGetPhysicalAddress(IN PVOID Va)
{
    ULONG_PTR V = (ULONG_PTR)Va;

    if (V >= NXK_KSEG0_BASE && V < NXK_KSEG0_BASE + NXK_KSEG0_RAM_SIZE)
        return V & NXK_PHYS_MASK;
    if (V >= NXK_WC_BASE && V < NXK_MMIO_LIMIT)
        return V;       /* identity-mapped MMIO + WC mirror */

    /* Anything else: defer to ARM3's page-table walker. */
    return MmGetPhysicalAddress(Va).LowPart;
}

/* SET-ADDRESS-PROTECT ******************************************************/

/* MmSetAddressProtect(Base, NumberOfBytes, NewProtect).  nxvm applies
 * real PTE protection in the title VA range.  The static windows
 * (KSEG0 = WB RAM, 0xF* aperture = WC/UC) stay a no-op: their bits are
 * already reflected in the PSE PDE, and real protect there would need
 * 4 KB PTEs carved out of the 4 MB large pages. */
VOID
NTAPI
NxMmSetAddressProtect(
    IN PVOID BaseAddress,
    IN ULONG NumberOfBytes,
    IN ULONG NewProtect)
{
#ifdef NXK_MM_VM
    if (NxVmOwnsAddress(BaseAddress))
    {
        NxVmSetAddressProtect(BaseAddress, NumberOfBytes, NewProtect);
        return;
    }
#endif
    UNREFERENCED_PARAMETER(BaseAddress);
    UNREFERENCED_PARAMETER(NumberOfBytes);
    UNREFERENCED_PARAMETER(NewProtect);
}

/* MAP-IO-SPACE *************************************************************/

/* Win32 PAGE_* protect bits the title passes through MmMapIoSpace /
 * MmAllocateContiguousMemoryEx.  Only the cacheability bits matter
 * here; access bits are honoured by the static window mapping. */
#define NXK_PAGE_NOCACHE        0x00000200UL
#define NXK_PAGE_WRITECOMBINE   0x00000400UL

/*
 * MmMapIoSpace(PhysicalAddress, NumberOfBytes, ProtectionType).
 * Returns a KSEG0/WC/MMIO-window VA aliasing the requested physical
 * range.  The three windows are already painted by
 * NxkMmEnsureXboxWindows -- this routine just picks the right VA base
 * given the PA + cacheability hint, no PTE work needed.
 *
 *   - PA >= 0xF0000000 (WC alias + device MMIO + flash mirrors): the
 *     window is identity-mapped, return PA verbatim.  This is the
 *     common case for titles wiring up NV2A (0xFD000000), APU
 *     (0xFE800000), USB (0xFED00000), AC97 (0xFEC00000), or NVNET
 *     (0xFEF00000) registers.
 *   - PA < 64 MB (RAM): the same physical page is visible through
 *     KSEG0 (cached WB) and through the 0xF0000000+ WC aperture.
 *     PROT carrying PAGE_WRITECOMBINE selects the WC alias; everything
 *     else lands on KSEG0.  PAGE_NOCACHE on RAM has no clean home
 *     here (we don't paint a strong-UC RAM aperture), so treat it as
 *     write-combine -- titles using PAGE_NOCACHE on RAM are usually
 *     after non-cached GPU staging which WC also serves.
 *   - PA in the 64 MB -- 0xF0000000 hole: not RAM, not a known device
 *     window.  Return NULL so the title sees the failure rather than
 *     silently corrupting state via an unmapped VA.
 */
PVOID
NTAPI
NxMmMapIoSpace(
    IN ULONG_PTR PhysicalAddress,
    IN SIZE_T    NumberOfBytes,
    IN ULONG     ProtectionType)
{
    PVOID Result;
    if (NumberOfBytes == 0)
        Result = NULL;
    else if (PhysicalAddress >= NXK_WC_BASE && PhysicalAddress < NXK_MMIO_LIMIT)
        /* Device MMIO + flash mirrors + WC RAM aperture: identity-mapped. */
        Result = (PVOID)PhysicalAddress;
    else if (PhysicalAddress < NXK_KSEG0_RAM_SIZE)
    {
        /* RAM: pick the alias based on the requested cacheability. */
        if (ProtectionType & (NXK_PAGE_WRITECOMBINE | NXK_PAGE_NOCACHE))
            Result = (PVOID)(NXK_WC_BASE | (PhysicalAddress & NXK_PHYS_MASK));
        else
            Result = (PVOID)(NXK_KSEG0_BASE | (PhysicalAddress & NXK_PHYS_MASK));
    }
    else
        Result = NULL;

    XbDbg("MmMapIoSpace(PA=%08lx sz=%lx prot=%lx) -> %p\n",
          (ULONG)PhysicalAddress, (ULONG)NumberOfBytes, ProtectionType, Result);
    return Result;
}

/* MmUnmapIoSpace(BaseAddress, NumberOfBytes).  No-op: the WC/MMIO/KSEG0
 * windows are static PSE entries that outlive any single mapping. */
VOID
NTAPI
NxMmUnmapIoSpace(
    IN PVOID BaseAddress,
    IN SIZE_T NumberOfBytes)
{
    UNREFERENCED_PARAMETER(BaseAddress);
    UNREFERENCED_PARAMETER(NumberOfBytes);
}

/* EOF */
