/*
 * meminv: complete runtime memory inventory -- runs on BOTH nxkrnl and
 * the retail kernel (boot xemu with bios.bin) for a direct diff.
 *
 * Counts UNIQUE physical frames (deduped via a bitmap), not mappings, so
 * every allocated page is attributed exactly once and the categories are
 * directly comparable.  Method (all architectural, no kernel-private
 * structures):
 *   - MmQueryStatistics for total/avail (free).
 *   - Walk the i386 self-mapped page tables (PD 0xC0300000, PTs
 *     0xC0000000 -- identical on retail).  Every present 4 KB PTE backs a
 *     runtime allocation; dedupe its PFN and bucket by VA region.  PSE
 *     4 MB PDEs are the KSEG0/WC/MMIO identity windows (not allocations).
 *   - Pinned remainder = used - (4K-deduped) = frames reachable only via
 *     KSEG0: kernel image + contiguous allocations + reserves.
 *
 * Output to port 0xE9 (xemu isa-debugcon) so retail is captured too.
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
#define MAXPFN  0x10000u                 /* 64 MB / 4 KB */

static uint32_t seen[MAXPFN / 32];       /* dedup bitmap (8 KB) */
static int mark(uint32_t pfn)
{
    if (pfn >= MAXPFN) return 0;         /* above 64 MB (devkit) -- ignore */
    uint32_t w = pfn >> 5, b = 1u << (pfn & 31);
    if (seen[w] & b) return 0;
    seen[w] |= b;
    return 1;
}

int main(void)
{
    MM_STATISTICS st;
    st.Length = sizeof st;
    uint32_t total = 0, avail = 0;
    if (MmQueryStatistics(&st) == 0)
    {
        total = st.TotalPhysicalPages; avail = st.AvailablePages;
        say("meminv: stats total=%u avail=%u pool=%u stack=%u cache=%u image=%u\n",
            total, avail, st.PoolPagesCommitted, st.StackPagesCommitted,
            st.CachePagesCommitted, st.ImagePagesCommitted);
    }

    /* Unique-frame counters by category. */
    uint32_t u_title = 0, u_pool = 0, u_aper = 0, u_pt = 0;
    uint32_t u_syspte = 0, u_pfndb = 0, u_kern = 0, u_self = 0;
    uint32_t pse_kseg = 0, pse_wc = 0, pse_mmio = 0;

    for (uint32_t i = 0; i < 1024; i++)
    {
        uint32_t pde = PD[i];
        if (!(pde & 1)) continue;
        uint32_t vbase = i << 22;
        if (pde & 0x80)
        {
            if (vbase >= 0x80000000u && vbase < 0x84000000u) pse_kseg++;
            else if (vbase >= 0xF0000000u && vbase < 0xF8000000u) pse_wc++;
            else if (vbase >= 0xF8000000u) pse_mmio++;
            continue;
        }
        /* The page table itself is an allocated frame. */
        u_pt += mark(pde >> 12);
        volatile uint32_t *pt = (volatile uint32_t *)(PTBASE + (i << 12));
        for (uint32_t j = 0; j < 1024; j++)
        {
            if (!(pt[j] & 1)) continue;
            uint32_t pfn = pt[j] >> 12;
            if (!mark(pfn)) continue;             /* count each frame once */
            uint32_t va = vbase | (j << 12);
            if (va < 0x80000000u) u_title++;
            else if (va >= 0xC0000000u && va < 0xC0400000u) u_self++;
            else if (va >= 0xC1000000u && va < 0xC2000000u) u_pool++;
            else if (va >= 0xD0000000u && va < 0xD4000000u) u_aper++;
            else if (va >= 0x84000000u && va < 0xB0000000u) u_syspte++;
            else if (va >= 0xB0000000u && va < 0xC0000000u) u_pfndb++;
            else u_kern++;
        }
    }

    uint32_t map4k = u_title + u_pool + u_aper + u_pt + u_syspte +
                     u_pfndb + u_kern + u_self;
    uint32_t used = (total > avail) ? (total - avail) : 0;
    uint32_t pinned = (used > map4k) ? (used - map4k) : 0;  /* image+contig+reserve */

    say("meminv: PSE windows kseg=%u wc=%u mmio=%u\n", pse_kseg, pse_wc, pse_mmio);
    say("meminv: FRAMES used=%u = title=%u pool=%u aper=%u pagetab=%u "
        "syspte=%u pfndb=%u kern=%u selfmap=%u + pinned(img/contig/rsv)=%u\n",
        used, u_title, u_pool, u_aper, u_pt, u_syspte, u_pfndb, u_kern, u_self, pinned);
    say("meminv: KERNEL-side = pool+aper+pagetab+syspte+pfndb+kern+selfmap+pinned "
        "= %u frames\n",
        u_pool + u_aper + u_pt + u_syspte + u_pfndb + u_kern + u_self + pinned);

    static const uint32_t mb[] = {60,56,52,48,44,40,36,32};
    for (uint32_t k = 0; k < sizeof mb / sizeof mb[0]; k++)
    {
        PVOID p = MmAllocateContiguousMemory((SIZE_T)mb[k] << 20);
        if (p) { say("meminv: maxcontig >= %u MB\n", mb[k]); MmFreeContiguousMemory(p); break; }
    }
    say("meminv: done\n");
    return 0;
}
