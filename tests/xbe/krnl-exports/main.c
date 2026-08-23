/*
 * krnl-exports: dump the running kernel's PE export directory over COM1.
 *
 * Parses the export table at the Xbox kernel image base (0x80010000) and
 * emits, for every ordinal, whether its AddressOfFunctions[] slot is a
 * real RVA or a zero gap.  Run it --official to read the RETAIL kernel's
 * true export set -- the question being which ordinals (DbgPrint and
 * friends) retail actually exports, so we know which of our .def entries
 * are over-exports we can drop.
 *
 * Output goes out the LPC47M157 COM1 (I/O 0x3F8), programmed here, so it
 * works under the retail kernel which doesn't wire it up for us.  We only
 * read kernel headers (ring 0, always mapped) and use port I/O -- no
 * kernel-private state -- so the same .xbe runs under either kernel.
 */

#include <xboxkrnl/xboxkrnl.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>

/* ---------- COM1 serial (self-programmed, no DbgPrint) ---------- */

#define COM1 0x3F8
#define COM1_THR (COM1 + 0)
#define COM1_DLL (COM1 + 0)
#define COM1_IER (COM1 + 1)
#define COM1_DLH (COM1 + 1)
#define COM1_FCR (COM1 + 2)
#define COM1_LCR (COM1 + 3)
#define COM1_MCR (COM1 + 4)
#define COM1_LSR (COM1 + 5)
#define LSR_THRE 0x20
#define LSR_TEMT 0x40
#define LCR_DLAB 0x80
#define LCR_8N1  0x03

static inline void outb(uint16_t port, uint8_t val) {
    __asm__ __volatile__("outb %0, %1" : : "a"(val), "Nd"(port));
}
static inline uint8_t inb(uint16_t port) {
    uint8_t ret;
    __asm__ __volatile__("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

static void com_init(void) {
    static int initialized = 0;
    if (initialized) return;
    initialized = 1;
    outb(COM1_IER, 0x00);
    outb(COM1_LCR, LCR_DLAB);
    outb(COM1_DLL, 0x01);              /* 115200 8N1 */
    outb(COM1_DLH, 0x00);
    outb(COM1_LCR, LCR_8N1);
    outb(COM1_FCR, 0xC7);
    outb(COM1_MCR, 0x0B);
}
static void com_putc(char c) {
    while ((inb(COM1_LSR) & LSR_THRE) == 0) { }
    outb(COM1_THR, (uint8_t)c);
}
static void com_puts(const char *s) {
    com_init();
    while (*s) {
        if (*s == '\n') com_putc('\r');
        com_putc(*s++);
    }
}
static void com_printf(const char *fmt, ...) {
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if (n > (int)sizeof(buf) - 1) n = sizeof(buf) - 1;
    buf[n] = 0;
    com_puts(buf);
}
static void com_drain(void) {
    while ((inb(COM1_LSR) & LSR_TEMT) == 0) { }
}

/* ---------- PE export-table walk ---------- */

#define KBASE 0x80010000u

static uint32_t rd32(uint32_t va) { return *(const volatile uint32_t *)va; }
static uint16_t rd16(uint32_t va) { return *(const volatile uint16_t *)va; }

static void dump_exports(uint32_t base) {
    com_printf("kexp: --- base=%08x ---\n", base);

    if (rd16(base) != 0x5A4D) {            /* 'MZ' */
        com_printf("kexp: no MZ at %08x (got %04x)\n", base, rd16(base));
        return;
    }
    uint32_t e_lfanew = rd32(base + 0x3C);
    uint32_t nt = base + e_lfanew;
    if (rd32(nt) != 0x00004550) {          /* 'PE\0\0' */
        com_printf("kexp: no PE sig at %08x (got %08x)\n", nt, rd32(nt));
        return;
    }
    /* OptionalHeader starts at nt+24; DataDirectory at opt+96; EXPORT=entry 0 */
    uint32_t opt = nt + 24;
    uint32_t exp_rva  = rd32(opt + 96 + 0);
    uint32_t exp_size = rd32(opt + 96 + 4);
    com_printf("kexp: e_lfanew=%x exp_rva=%x exp_size=%x\n",
               e_lfanew, exp_rva, exp_size);
    if (!exp_rva) { com_printf("kexp: NO EXPORT DIRECTORY\n"); return; }

    uint32_t ed = base + exp_rva;
    uint32_t ord_base = rd32(ed + 16);
    uint32_t nfunc    = rd32(ed + 20);
    uint32_t nnames   = rd32(ed + 24);
    uint32_t aof_rva  = rd32(ed + 28);
    uint32_t aof = base + aof_rva;
    com_printf("kexp: ordBase=%u nFunc=%u nNames=%u aof_rva=%x\n",
               ord_base, nfunc, nnames, aof_rva);

    uint32_t present = 0, missing = 0;
    for (uint32_t i = 0; i < nfunc; i++) {
        uint32_t rva = rd32(aof + i * 4);
        uint32_t ord = ord_base + i;
        if (rva == 0) { missing++; com_printf("kexp: GAP ord %u\n", ord); }
        else          { present++; com_printf("kexp: EXP %u %08x\n", ord, rva); }
    }
    com_printf("kexp: present=%u missing=%u total=%u\n",
               present, missing, nfunc);

    /* explicit spot-checks the question is about */
    if (8 >= ord_base && 8 < ord_base + nfunc) {
        uint32_t r = rd32(aof + (8 - ord_base) * 4);
        com_printf("kexp: DbgPrint@8 -> %s (rva=%08x)\n",
                   r ? "PRESENT" : "GAP", r);
    }
}

int main(void) {
    com_puts("kexp: == krnl-exports begin ==\n");
    dump_exports(KBASE);
    com_puts("kexp: == krnl-exports end ==\n");
    com_drain();

    /* SMC power-off: clean exit under both retail and nxkrnl. */
    LARGE_INTEGER d = { .QuadPart = -((LONGLONG)500 * 10000) };
    KeDelayExecutionThread(KernelMode, FALSE, &d);
    HalWriteSMBusValue(0x20, 0x02, FALSE, 0x80);
    for (;;) { __asm__ __volatile__("hlt"); }
    return 0;
}
