/*
 * PROJECT:     nxkrnl -- a free kernel for the original Xbox
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Kernel-ordinal strace -- wrap every thunk in a small
 *              trampoline that prints the ordinal before forwarding.
 *
 * Compiled in only when XB_STRACE is on; the public helpers fold to
 * no-ops in the header otherwise, so callers stay unconditional.
 *
 * Trampoline layout (24 bytes per ordinal; offsets are decimal):
 *     0   60                  pusha
 *     1   FF 74 24 20         push  dword [esp+0x20]   ; title's ret addr
 *     5   68 oo oo oo oo      push  imm32 (ordinal)
 *    10   E8 rr rr rr rr      call  rel32 -> XbStraceLog
 *    15   83 C4 08            add   esp, 8
 *    18   61                  popa
 *    19   E9 rr rr rr rr      jmp   rel32 -> real fn
 *
 * The title's return address sits one slot above the eight pusha'd
 * registers, handed to the logger so each line names the call site.
 * The trampoline returns no stack to the caller -- the real fn's `ret N`
 * or the caller's `add esp, N` cleans up the args.  pusha/popa preserves
 * all GPRs so the real fn sees the title's regs intact (matters for the
 * fastcall Interlocked* ordinals -- args in ECX/EDX).  `call rel32` to
 * the logger requires the trampoline buffer within +-2 GiB of it, which
 * NonPagedPool satisfies on the 64 MiB address space.
 *
 * Re-entrancy: the logger uses DbgPrint, itself reached through a
 * trampoline; a per-CPU flag short-circuits the recursion.
 */

#include <ntdef.h>
#include <ntifs.h>

#include <xb-debug.h>
#include "strace.h"

#if XB_STRACE

#define XB_STRACE_TAG          'rTxN'
#define XB_STRACE_TRAMP_SIZE   24
#define XB_STRACE_MAX_ORD      400

static volatile LONG XbStraceInside;          /* re-entrancy guard */

static const char *
XbStraceName(ULONG Ordinal)
{
    /* Just the ordinals the triangle imports today: enough to recognise the
     * last call in the serial log.  Others print as "ord N". */
    switch (Ordinal) {
    case 1:   return "AvGetSavedDataAddress";
    case 2:   return "AvSendTVEncoderOption";
    case 3:   return "AvSetDisplayMode";
    case 4:   return "AvSetSavedDataAddress";
    case 8:   return "DbgPrint";
    case 24:  return "ExQueryNonVolatileSetting";
    case 44:  return "HalGetInterruptVector";
    case 45:  return "HalReadSMBusValue";
    case 46:  return "HalReadWritePCISpace";
    case 47:  return "HalRegisterShutdownNotification";
    case 49:  return "HalReturnToFirmware";
    case 50:  return "HalWriteSMBusValue";
    case 54:  return "InterlockedExchange";
    case 67:  return "IoCreateSymbolicLink";
    case 69:  return "IoDeleteSymbolicLink";
    case 98:  return "KeConnectInterrupt";
    case 99:  return "KeDelayExecutionThread";
    case 100: return "KeDisconnectInterrupt";
    case 103: return "KeGetCurrentIrql";
    case 104: return "KeGetCurrentThread";
    case 107: return "KeInitializeDpc";
    case 109: return "KeInitializeInterrupt";
    case 119: return "KeInsertQueueDpc";
    case 128: return "KeQuerySystemTime";
    case 137: return "KeRemoveQueueDpc";
    case 143: return "KeSetBasePriorityThread";
    case 165: return "MmAllocateContiguousMemory";
    case 166: return "MmAllocateContiguousMemoryEx";
    case 168: return "MmClaimGpuInstanceMemory";
    case 171: return "MmFreeContiguousMemory";
    case 178: return "MmPersistContiguousMemory";
    case 180: return "MmQueryAllocationSize";
    case 184: return "NtAllocateVirtualMemory";
    case 186: return "ZwClearEvent";
    case 187: return "NtClose";
    case 189: return "NtCreateEvent";
    case 190: return "NtCreateFile";
    case 192: return "NtCreateMutant";
    case 193: return "NtCreateSemaphore";
    case 199: return "NtFreeVirtualMemory";
    case 201: return "NtOpenDirectoryObject";
    case 202: return "NtOpenFile";
    case 203: return "NtOpenSymbolicLinkObject";
    case 205: return "NtPulseEvent";
    case 207: return "NtQueryDirectoryFile";
    case 208: return "NtQueryDirectoryObject";
    case 210: return "NtQueryFullAttributesFile";
    case 211: return "NtQueryInformationFile";
    case 217: return "NtQueryVirtualMemory";
    case 218: return "NtQueryVolumeInformationFile";
    case 219: return "NtReadFile";
    case 221: return "NtReleaseMutant";
    case 222: return "NtReleaseSemaphore";
    case 224: return "NtResumeThread";
    case 225: return "NtSetEvent";
    case 226: return "NtSetInformationFile";
    case 231: return "NtSuspendThread";
    case 233: return "NtWaitForSingleObject";
    case 234: return "NtWaitForSingleObjectEx";
    case 235: return "NtWaitForMultipleObjectsEx";
    case 236: return "NtWriteFile";
    case 238: return "ZwYieldExecution";
    case 246: return "ObReferenceObjectByHandle";
    case 250: return "ObfDereferenceObject";
    case 255: return "PsCreateSystemThreadEx";
    case 258: return "PsTerminateSystemThread";
    case 277: return "RtlEnterCriticalSection";
    case 289: return "RtlInitAnsiString";
    case 291: return "RtlInitializeCriticalSection";
    case 294: return "RtlLeaveCriticalSection";
    case 301: return "RtlNtStatusToDosError";
    case 302: return "RtlRaiseException";
    case 304: return "RtlTimeFieldsToTime";
    case 305: return "RtlTimeToTimeFields";
    case 306: return "RtlTryEnterCriticalSection";
    case 335: return "XcSHAInit";
    case 336: return "XcSHAUpdate";
    case 337: return "XcSHAFinal";
    default:  return NULL;
    }
}

/*
 * Pool free-list validator.  Walk MmNonPagedPoolFreeListHead -- if any
 * entry's Flink/Blink don't round-trip, the pool free list is corrupted.
 * Called from XbStraceLog at the start of every traced ordinal (BEFORE
 * the real function runs), so the first time it fires we know "ordinal N
 * is about to run, and the pool was corrupted by the ordinal *before* N
 * in the strace log".  Lock-free walk at HIGH_LEVEL: pool inserts/removes
 * serialize on a queued spinlock we'd deadlock against if we acquired it
 * (the surrounding strace path may itself be deep inside a pool op).
 * Single-CPU box, IRQL raised to keep DPCs out -- if a write hits
 * mid-walk we'd see the same corruption again next call.  4 lists, each
 * capped at 4096 entries to bound the cost.
 */
#ifndef NXK_MM_POOL
extern LIST_ENTRY MmNonPagedPoolFreeListHead[4];        /* MI_MAX_FREE_PAGE_LISTS */
#endif

/*
 * Walk every entry of every pool free-list head looking for bad
 * Flink/Blink.  Return value: low 16 bits = list index + 1 (0 = clean);
 * high 16 bits = hop count where it broke.  At HIGH_LEVEL with a bounded
 * hop cap to keep the walk finite even if a cycle exists.
 */
static ULONG
XbStraceWalkPool(_Out_ PLIST_ENTRY *BadEntry)
{
#ifdef NXK_MM_POOL
    /* nxmm pool has no ARM3 free lists to validate. */
    *BadEntry = NULL;
    return 0;
#else
    KIRQL old;
    ULONG i;
    ULONG bad = 0;

    *BadEntry = NULL;
    KeRaiseIrql(HIGH_LEVEL, &old);
    for (i = 0; i < 4; i++)
    {
        PLIST_ENTRY head = &MmNonPagedPoolFreeListHead[i];
        PLIST_ENTRY e = head->Flink;
        ULONG hops = 0;
        while (e != head && hops < 4096)
        {
            /* A real free-list entry must be a kernel VA (>= 0x80000000) and
             * below the Xbox MMIO base (NxkMmReserveXboxWindows caps the
             * nonpaged pool there).  ARM3 initial nonpaged pool sits around
             * 0xB0088000+, nonpaged-pool expansion just under 0xF0000000. */
            if ((ULONG_PTR)e < 0x80000000UL || (ULONG_PTR)e >= 0xF0000000UL ||
                e->Flink->Blink != e || e->Blink->Flink != e)
            {
                *BadEntry = e;
                bad = (i + 1) | (hops << 16);
                goto done;
            }
            hops++;
            e = e->Flink;
        }
    }
done:
    KeLowerIrql(old);
    return bad;
#endif /* NXK_MM_POOL */
}

static volatile LONG XbStracePoolReported;            /* per-incident latch */
static PLIST_ENTRY    XbStracePoolLastBadEntry;

VOID
XbStraceValidatePool(_In_ PCSTR Where)
{
    PLIST_ENTRY bad_entry = NULL;
    ULONG bad = XbStraceWalkPool(&bad_entry);
    if (bad == 0)
    {
        /* Pool clean at this checkpoint -- reset the latch so the next
         * corruption detection fires (with this checkpoint as the upper
         * bound). */
        XbStracePoolLastBadEntry = NULL;
        XbStracePoolReported = 0;
        return;
    }
    /* Only print when the bad entry changes, or once per detection burst. */
    if (bad_entry == XbStracePoolLastBadEntry &&
        InterlockedCompareExchange((volatile LONG *)&XbStracePoolReported,
                                    1, 1) == 1)
        return;
    XbStracePoolLastBadEntry = bad_entry;
    XbStracePoolReported = 1;
    {
        ULONG list = (bad & 0xFFFF) - 1;
        ULONG hops = (bad >> 16);
        DbgPrint("list[%lu] BROKEN at hop %lu entry=%p "
                 "(flink=%p flink->blink=%p blink=%p blink->flink=%p); "
                 "checkpoint=%s\n",
                 list, hops, bad_entry,
                 bad_entry->Flink, bad_entry->Flink->Blink,
                 bad_entry->Blink, bad_entry->Blink->Flink, Where);
    }
}

static VOID __cdecl
XbStraceLog(ULONG Ordinal, ULONG_PTR CallerEip)
{
    const char *name;

    if (InterlockedExchange((volatile LONG *)&XbStraceInside, 1) != 0)
        return;
    name = XbStraceName(Ordinal);

    /* Pool-corruption hunt: check BEFORE we run the real ordinal.  First
     * report identifies the ordinal IMMEDIATELY AFTER the corrupter in the
     * strace log -- bisect by suppressing the suspect and re-running. */
    XbStraceValidatePool(name != NULL ? name : "?");

    if (name != NULL)
        XbDbg("ord %lu (%s) <- %08lx\n", Ordinal, name, CallerEip);
    else
        XbDbg("ord %lu <- %08lx\n", Ordinal, CallerEip);
    XbStraceInside = 0;
}

PVOID
XbStraceWrap(_In_ ULONG Ordinal, _In_ PVOID RealFn)
{
    static UCHAR *arena;
    static UCHAR *cur;
    static SIZE_T left;

    UCHAR *t;
    LONG rel;

    if (left < XB_STRACE_TRAMP_SIZE)
    {
        SIZE_T sz = XB_STRACE_TRAMP_SIZE * XB_STRACE_MAX_ORD;
        arena = (UCHAR *)ExAllocatePoolWithTag(NonPagedPool, sz, XB_STRACE_TAG);
        if (arena == NULL)
            return RealFn;          /* fall through to direct dispatch */
        cur = arena;
        left = sz;
    }

    t = cur;
    cur  += XB_STRACE_TRAMP_SIZE;
    left -= XB_STRACE_TRAMP_SIZE;

    t[0] = 0x60;                                /* pusha */
    t[1] = 0xFF; t[2] = 0x74; t[3] = 0x24; t[4] = 0x20;
                                                /* push [esp+0x20] (title ret) */
    t[5] = 0x68;                                /* push imm32 (ordinal) */
    *(ULONG *)(t + 6) = Ordinal;
    t[10] = 0xE8;                               /* call rel32 XbStraceLog */
    rel = (LONG)((ULONG_PTR)&XbStraceLog - ((ULONG_PTR)(t + 10) + 5));
    *(LONG *)(t + 11) = rel;
    t[15] = 0x83; t[16] = 0xC4; t[17] = 0x08;   /* add esp, 8 */
    t[18] = 0x61;                               /* popa */
    t[19] = 0xE9;                               /* jmp rel32 real_fn */
    rel = (LONG)((ULONG_PTR)RealFn - ((ULONG_PTR)(t + 19) + 5));
    *(LONG *)(t + 20) = rel;
    return t;
}

#endif /* XB_STRACE */
