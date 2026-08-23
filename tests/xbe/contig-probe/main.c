/*
 * Contiguous-PA probe -- find the largest single contiguous
 * allocation MmAllocateContiguousMemoryEx will grant.
 *
 * Background: max contig is ~55 MB on retail.  Boot-time reservations
 * (the kernel image and the appended-image blob) carve out holes;
 * LoaderFirmwareTemporary should be reclaimed by Mm Phase 0 but whether
 * that actually happens on our build needs measuring.
 *
 * Strategy: bisect on the requested size, calling MmAllocate with a
 * 1-byte alignment and printing whatever it returns / refuses.  The
 * answer is the largest power-of-two-ish size that succeeds.
 */
#include <xboxkrnl/xboxkrnl.h>
#include <stdint.h>

static void probe(SIZE_T bytes)
{
    PVOID p = MmAllocateContiguousMemoryEx(bytes, 0, 0x3FFFFFF, 0,
                                           PAGE_READWRITE);
    DbgPrint("contig-probe: MmAllocate(%lu KB) -> %p\n",
             (unsigned long)(bytes >> 10), p);
    if (p)
        MmFreeContiguousMemory(p);
}

int main(void)
{
    /* Try a coarse sweep first (1, 2, 4, 8, ... MB) to bracket the
     * largest that succeeds, then a fine sweep around the boundary. */
    SIZE_T sizes_coarse[] = {
        1ul * 1024 * 1024,   /*  1 MB */
        2ul * 1024 * 1024,
        4ul * 1024 * 1024,
        8ul * 1024 * 1024,
        16ul * 1024 * 1024,
        32ul * 1024 * 1024,
        40ul * 1024 * 1024,
        45ul * 1024 * 1024,
        48ul * 1024 * 1024,
        50ul * 1024 * 1024,
        52ul * 1024 * 1024,
        54ul * 1024 * 1024,
        55ul * 1024 * 1024,
        56ul * 1024 * 1024,
        58ul * 1024 * 1024,
        60ul * 1024 * 1024,
    };
    LONG i;

    DbgPrint("contig-probe: starting coarse sweep\n");
    for (i = 0; i < (LONG)(sizeof(sizes_coarse)/sizeof(sizes_coarse[0])); i++)
        probe(sizes_coarse[i]);

    DbgPrint("contig-probe: done\n");
    return 0;
}
