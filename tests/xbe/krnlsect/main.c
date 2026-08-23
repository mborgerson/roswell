/*
 * krnlsect: dump the running kernel's PE section table at runtime, on
 * BOTH nxkrnl and the retail kernel (boot xemu with bios.bin), so the
 * two image layouts can be diffed directly.
 *
 * The Xbox kernel is loaded at the fixed VA 0x80010000 (XBOX_KERNEL_BASE)
 * with its PE header intact -- reading that header is black-box
 * observation of a loaded image, never disassembly of bios.bin.  We
 * parse the DOS + PE headers with raw offsets (no header struct
 * dependency) and print every section's name, virtual size, RVA, and
 * whether it is DISCARDABLE (freed after init).  The resident total is
 * the sum of non-DISCARDABLE virtual sizes, page-rounded.
 *
 * Output on port 0xE9 so retail is captured too.
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

#define KBASE        0x80010000u
#define RD16(p)      (*(volatile uint16_t *)(uintptr_t)(p))
#define RD32(p)      (*(volatile uint32_t *)(uintptr_t)(p))
#define DISCARDABLE  0x02000000u
#define MEM_NOT_PAGED 0x08000000u
#define PAGE_RND(x)  (((x) + 0xFFFu) & ~0xFFFu)

int main(void)
{
    uint32_t base = KBASE;
    if (RD16(base) != 0x5A4D)            /* 'MZ' */
    {
        say("krnlsect: no MZ at %08x (got %04x)\n", base, RD16(base));
        return 0;
    }
    uint32_t nt = base + RD32(base + 0x3C);   /* e_lfanew */
    if (RD32(nt) != 0x00004550)          /* 'PE\0\0' */
    {
        say("krnlsect: no PE sig at %08x (got %08x)\n", nt, RD32(nt));
        return 0;
    }
    uint32_t numSec  = RD16(nt + 6);          /* FileHeader.NumberOfSections */
    uint32_t optSize = RD16(nt + 20);         /* FileHeader.SizeOfOptionalHeader */
    uint32_t sizeOfImage = RD32(nt + 24 + 56);/* OptionalHeader.SizeOfImage */
    uint32_t sect = nt + 24 + optSize;        /* first section header */

    say("krnlsect: base=%08x sections=%u SizeOfImage=%u (%u pg)\n",
        base, numSec, sizeOfImage, PAGE_RND(sizeOfImage) >> 12);

    uint32_t resident = 0, discarded = 0;
    for (uint32_t i = 0; i < numSec && i < 32; i++)
    {
        uint32_t s = sect + i * 40;
        char name[9];
        for (int k = 0; k < 8; k++) name[k] = (char)*(volatile uint8_t *)(uintptr_t)(s + k);
        name[8] = 0;
        uint32_t vsize = RD32(s + 8);
        uint32_t rva   = RD32(s + 12);
        uint32_t rawsz = RD32(s + 16);
        uint32_t chr   = RD32(s + 36);
        uint32_t vpg   = PAGE_RND(vsize) >> 12;
        say("  %-8s rva=%08x vsize=%7u (%3u pg) raw=%7u %s%s\n",
            name, rva, vsize, vpg, rawsz,
            (chr & DISCARDABLE) ? "DISCARD " : "RESIDENT",
            (chr & MEM_NOT_PAGED) ? " nopage" : "");
        if (chr & DISCARDABLE) discarded += PAGE_RND(vsize);
        else                   resident  += PAGE_RND(vsize);
    }
    say("krnlsect: RESIDENT=%u (%u pg)  DISCARDABLE=%u (%u pg)\n",
        resident, resident >> 12, discarded, discarded >> 12);
    say("krnlsect: done\n");
    return 0;
}
