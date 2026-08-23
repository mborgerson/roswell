/*
 * syspte: complete attribution of the system-PTE space (VA 0x84000000..
 * 0xB0000000).  Runs on nxkrnl (and retail, where the region is empty)
 * via the i386 self-mapped page tables.
 *
 * For every present 4 KB PTE in the region we read the target PA and split:
 *   - MMIO target (PA >= top of RAM): an MmMapIoSpace mapping (device
 *     registers) -- costs NO RAM, only the PT page holding the PTE.
 *   - RAM target  (PA <  top of RAM): a real frame (ncache DMA buffer, MDL
 *     map, etc.).  These are the only *potentially* reclaimable frames, and
 *     only if nothing still needs them.
 * Plus the PT pages themselves (one resident frame per present PDE).
 *
 * Output on port 0xE9 (xemu isa-debugcon).
 */
#include <xboxkrnl/xboxkrnl.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>

static void e9_putc(char c) { __asm__ __volatile__("outb %0, $0xE9" : : "a"((uint8_t)c)); }
static void e9s(const char *s) { while (*s) e9_putc(*s++); }
static void say(const char *fmt, ...)
{
    char b[256];
    va_list ap; va_start(ap, fmt);
    int n = vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if (n > (int)sizeof b - 1) n = sizeof b - 1;
    b[n] = 0;
    e9s(b);
    DbgPrint("%s", b);
}

#define PD      ((volatile uint32_t *)0xC0300000u)
#define PTBASE  0xC0000000u
#define LO      0x84000000u
#define HI      0xB0000000u

int main(void)
{
    MM_STATISTICS st; st.Length = sizeof st;
    uint32_t total = 0;
    if (MmQueryStatistics(&st) == 0) total = st.TotalPhysicalPages;
    uint32_t ram_top_pfn = total ? total : 0x8000u;   /* default 128 MB */
    say("syspte: total_ram_pages=%u (ram_top_pfn=0x%x)\n", total, ram_top_pfn);

    uint32_t pt_pages = 0;          /* present PDEs -> PT page each */
    uint32_t pte_ram = 0, pte_mmio = 0;
    uint32_t first_va = 0, last_va = 0;
    /* RAM target histogram by 8 MB PA bucket (0..128 MB -> 16 buckets). */
    uint32_t ram_hist[16] = {0};
    /* MMIO target histogram by top byte of PA. */
    uint32_t mmio_hist[256] = {0};
    uint32_t shown = 0;

    for (uint32_t i = (LO >> 22); i < (HI >> 22); i++)
    {
        uint32_t pde = PD[i];
        if (!(pde & 1)) continue;
        if (pde & 0x80) { say("syspte: PSE PDE at va=%08x (unexpected)\n", i << 22); continue; }
        pt_pages++;
        volatile uint32_t *pt = (volatile uint32_t *)(PTBASE + (i << 12));
        for (uint32_t j = 0; j < 1024; j++)
        {
            uint32_t pte = pt[j];
            if (!(pte & 1)) continue;
            uint32_t pfn = pte >> 12;
            uint32_t va  = (i << 22) | (j << 12);
            uint32_t pa  = pfn << 12;
            if (!first_va) first_va = va;
            last_va = va;
            if (pfn < ram_top_pfn)
            {
                pte_ram++;
                uint32_t b = pa >> 23;                 /* 8 MB buckets */
                if (b < 16) ram_hist[b]++;
                if (shown < 12) {
                    /* fingerprint: first two dwords of the frame's content,
                     * and whether it also has a KSEG0 identity alias. */
                    volatile uint32_t *c = (volatile uint32_t *)(uintptr_t)va;
                    uint32_t kseg_pde = PD[(0x80000000u + pa) >> 22];
                    int kseg = 0;
                    if (kseg_pde & 1) {
                        if (kseg_pde & 0x80) kseg = 1;
                        else { volatile uint32_t *kpt = (volatile uint32_t *)
                            (PTBASE + (((0x80000000u + pa) >> 22) << 12));
                            kseg = (kpt[((0x80000000u + pa) >> 12) & 0x3FF] & 1); }
                    }
                    say("syspte:  RAM va=%08x pa=%08x %s kseg0=%d [%08x %08x]\n",
                        va, pa, (pte & 0x10) ? "(cd)" : "", kseg, c[0], c[1]);
                    shown++;
                }
            }
            else
            {
                pte_mmio++;
                mmio_hist[(pa >> 24) & 0xFF]++;
            }
        }
    }

    say("syspte: region 0x84000000-0xB0000000  first=%08x last=%08x\n", first_va, last_va);
    say("syspte: PT_pages=%u  present_PTEs=%u (RAM=%u  MMIO=%u)\n",
        pt_pages, pte_ram + pte_mmio, pte_ram, pte_mmio);
    say("syspte: RAM-frame cost = PT_pages + RAM_targets = %u + %u = %u\n",
        pt_pages, pte_ram, pt_pages + pte_ram);
    say("syspte: RAM target by 8MB bucket:\n");
    for (uint32_t b = 0; b < 16; b++)
        if (ram_hist[b]) say("syspte:   PA %3u-%3u MB : %u\n", b*8, b*8+8, ram_hist[b]);
    say("syspte: MMIO target by PA top byte:\n");
    for (uint32_t b = 0; b < 256; b++)
        if (mmio_hist[b]) say("syspte:   PA %02x000000 : %u\n", b, mmio_hist[b]);
    say("syspte: done\n");
    return 0;
}
