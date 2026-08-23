/*
 * KeDelay accumulation test XBE -- exercises KeDelayExecutionThread.
 *
 * Background: titles consistently hang after roughly 500-1000 ms of
 * cumulative KeDelay-driven sleep (PhyInitialize at 3-5 iter * 200 ms
 * hangs near iter 4; xboxdash sits in its main loop calling KeDelay then
 * stops making syscalls).  KeTickCount appears to stop advancing once we
 * trip the bug.  This test does N back-to-back KeDelay(STEP_MS) calls
 * with a print *before* each one so we can see which one is the first to
 * not return.  No other ordinals involved (no MII, no Mm, no Ps), so the
 * fault, if any, is purely in the kernel's wait / timer machinery.
 *
 * Output we expect on a healthy kernel: N+1 "iter=K t=..." lines
 * followed by "done".  Output we expect on the buggy one: a sequence of
 * iter lines, then silence (KeDelay never returns).
 */
#include <xboxkrnl/xboxkrnl.h>
#include <stdint.h>

#define STEP_MS  200   /* 200 ms per call -- matches PHY autoneg step     */
#define ITERS    20    /* total budget 4 s -- well past the observed bug  */

/* NV NIC MII access registers; the Xbox kernel maps these UC at the
 * matching VAs once NxkMmEnsureXboxWindows has fired -- we trigger that
 * here by allocating one page of contiguous memory before our loop. */
#define NV_MII_CONTROL  (*(volatile unsigned long *)0xFEF00190)
#define NV_MII_DATA     (*(volatile unsigned long *)0xFEF00194)

/* Read normal RAM (the page MmAllocate'd below is WB) -- not UC MMIO.
 * Tests whether the hang is specific to UC pages or to any memory read. */
static volatile unsigned long *ram_buf;

static void paint_windows(void)
{
    /* MmAllocateContiguousMemoryEx fires NxkMmEnsureXboxWindows on first
     * call.  We don't care about the buffer; just want the side effect. */
    PVOID p = MmAllocateContiguousMemoryEx(0x1000, 0, 0x3FFFFFF, 0,
                                           PAGE_READWRITE);
    ram_buf = (volatile unsigned long *)p;
    DbgPrint("kedelay-test: MmAllocateContiguousMemoryEx -> %p\n", p);
}

static void phy_like_mmio(void)
{
    volatile unsigned long tmp;
    tmp = *ram_buf;
    (void)tmp;
}

int main(void)
{
    LARGE_INTEGER interval;
    NTSTATUS      status;
    LONG          i;

    /* KeDelayExecutionThread takes a relative 100-ns-unit interval; a
     * negative value means "from now."  -1000000 = 100 ms. */
    interval.QuadPart = -(LONGLONG)STEP_MS * 10000;

    /* Allocate one contiguous page (fires NxkMmEnsureXboxWindows) but
     * never touch the buffer.  If THIS run also hangs at iter ~5-6, the
     * trigger is the painter / page-table change, not any subsequent
     * memory access. */
    paint_windows();

    for (i = 0; i < ITERS; i++)
    {
        /* Print BEFORE the call so we can pinpoint which KeDelay hangs. */
        DbgPrint("kedelay-test: iter=%ld t=%lu ticks  (before KeDelay)\n",
                 (long)i, (unsigned long)KeTickCount);
        status = KeDelayExecutionThread(KernelMode, FALSE, &interval);
        if (!NT_SUCCESS(status))
        {
            DbgPrint("kedelay-test: KeDelay returned %08lx -- aborting\n",
                     (unsigned long)status);
            break;
        }
    }

    DbgPrint("kedelay-test: done after %ld iter, KeTickCount=%lu\n",
             (long)i, (unsigned long)KeTickCount);
    return 0;
}
