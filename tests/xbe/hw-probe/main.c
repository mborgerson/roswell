/*
 * hw-probe: dump key hardware-configuration state at boot, then exit.
 *
 * Designed to run against both nxkrnl (our kernel) and the retail kernel
 * (boot xemu with bios.bin), so we can diff the two on real questions like:
 *
 *   - What does each kernel write into the master/slave PIC ELCR?  i.e.
 *     which IRQs are programmed edge vs level on each side.
 *   - How does each kernel program the LPC INT_IRQ_ROUT (config 0x6C),
 *     which decides which IRQ each internal PCI device maps to.
 *   - What's the OHCI/PCI state at the time the title is reached
 *     (HcControl, interrupt enable bits, BusMaster/MemSpace).
 *
 * We dump only via port-I/O and MMIO -- no kernel-private state -- so
 * the same .xbe runs under both kernels.  Each line is tagged "hwprobe:"
 * so it's easy to grep out of either kernel's serial log.
 */

#include <xboxkrnl/xboxkrnl.h>
#include <hal/debug.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>

/* ---------- low-level helpers ---------- */

static inline uint8_t inb(uint16_t port) {
    uint8_t v;
    __asm__ __volatile__("inb %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}
static inline void outb(uint16_t port, uint8_t v) {
    __asm__ __volatile__("outb %0, %1" : : "a"(v), "Nd"(port));
}
static inline uint32_t read_eflags(void) {
    uint32_t f;
    __asm__ __volatile__("pushfl; popl %0" : "=r"(f));
    return f;
}

/* Port 0xE9 debugcon output -- xemu's isa-debugcon device prints whatever
 * is written here to xemu's stdout/stderr, regardless of whether the guest
 * kernel has any serial driver up.  This lets us get output under retail
 * (which doesn't drive the lpc47m157 UART) as well as under nxkrnl. */
static void e9_putc(char c) {
    __asm__ __volatile__("outb %0, $0xE9" : : "a"((uint8_t)c));
}
static void e9_puts(const char *s) {
    while (*s) e9_putc(*s++);
}
static void say(const char *fmt, ...) {
    char buf[256];
    va_list ap; va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if (n > (int)sizeof buf - 1) n = sizeof buf - 1;
    buf[n] = 0;
    e9_puts(buf);
    /* Also send to DbgPrint -- works under nxkrnl, no-op under retail. */
    DbgPrint("%s", buf);
}
#define DbgPrint say

/* PCI config space via the legacy CF8h/CFCh mechanism.  Works the same on
 * Xbox as on a vanilla PC for the chipset devices we care about. */
static uint32_t pci_rd32(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t reg) {
    uint32_t addr = 0x80000000u | ((uint32_t)bus << 16) |
                    ((uint32_t)dev << 11) | ((uint32_t)fn << 8) |
                    (reg & 0xFCu);
    __asm__ __volatile__("outl %0, %1" :: "a"(addr), "Nd"((uint16_t)0xCF8));
    uint32_t v;
    __asm__ __volatile__("inl %1, %0" : "=a"(v) : "Nd"((uint16_t)0xCFC));
    return v;
}

/* ---------- PIC / ELCR ---------- */

static void dump_pic(void) {
    uint8_t imr1 = inb(0x21);
    uint8_t imr2 = inb(0xA1);

    /* OCW3 trick: write 0x0A to read IRR, 0x0B to read ISR. */
    outb(0x20, 0x0A); uint8_t irr1 = inb(0x20);
    outb(0x20, 0x0B); uint8_t isr1 = inb(0x20);
    outb(0xA0, 0x0A); uint8_t irr2 = inb(0xA0);
    outb(0xA0, 0x0B); uint8_t isr2 = inb(0xA0);

    /* ELCR: master at 0x4D0, slave at 0x4D1.  Bit N == 1 means IRQ N is
     * level triggered; bit N == 0 means edge.  This is THE answer to "is
     * the OHCI IRQ being delivered edge or level". */
    uint8_t elcr_m = inb(0x4D0);
    uint8_t elcr_s = inb(0x4D1);

    uint32_t flags = read_eflags();

    DbgPrint("hwprobe: PIC1 imr=0x%02X irr=0x%02X isr=0x%02X elcr=0x%02X\n",
             imr1, irr1, isr1, elcr_m);
    DbgPrint("hwprobe: PIC2 imr=0x%02X irr=0x%02X isr=0x%02X elcr=0x%02X\n",
             imr2, irr2, isr2, elcr_s);

    /* Break out the master ELCR per IRQ so the diff is obvious. */
    DbgPrint("hwprobe: ELCR master per-irq: "
             "IRQ0=%c IRQ1=%c IRQ2=%c IRQ3=%c IRQ4=%c IRQ5=%c IRQ6=%c IRQ7=%c "
             "(L=level E=edge)\n",
             (elcr_m & 0x01) ? 'L' : 'E',
             (elcr_m & 0x02) ? 'L' : 'E',
             (elcr_m & 0x04) ? 'L' : 'E',
             (elcr_m & 0x08) ? 'L' : 'E',
             (elcr_m & 0x10) ? 'L' : 'E',
             (elcr_m & 0x20) ? 'L' : 'E',
             (elcr_m & 0x40) ? 'L' : 'E',
             (elcr_m & 0x80) ? 'L' : 'E');
    DbgPrint("hwprobe: ELCR slave per-irq:  "
             "IRQ8=%c IRQ9=%c IRQ10=%c IRQ11=%c IRQ12=%c IRQ13=%c IRQ14=%c IRQ15=%c\n",
             (elcr_s & 0x01) ? 'L' : 'E',
             (elcr_s & 0x02) ? 'L' : 'E',
             (elcr_s & 0x04) ? 'L' : 'E',
             (elcr_s & 0x08) ? 'L' : 'E',
             (elcr_s & 0x10) ? 'L' : 'E',
             (elcr_s & 0x20) ? 'L' : 'E',
             (elcr_s & 0x40) ? 'L' : 'E',
             (elcr_s & 0x80) ? 'L' : 'E');

    DbgPrint("hwprobe: EFLAGS.IF=%u\n", (flags >> 9) & 1);
}

/* ---------- LPC / chipset interrupt routing ---------- */

static const char *nibble_irq(uint32_t v, int shift) {
    static char buf[4];
    unsigned n = (v >> shift) & 0xF;
    if (n == 0) return "off";
    if (n > 15) return "?";
    /* Caller is single-threaded; static buffer is fine. */
    buf[0] = '0' + (n / 10);
    buf[1] = (n % 10) < 10 ? '0' + (n % 10) : '?';
    buf[2] = 0;
    if (n < 10) { buf[0] = '0' + n; buf[1] = 0; }
    return buf;
}

static void dump_lpc(void) {
    /* LPC = bus 0, slot 1, fn 0 on the Xbox.  Config 0x6C is INT_IRQ_ROUT:
     * eight nibbles, each mapping an internal device's PIRQ index to a
     * legacy IRQ number.  Bit/nibble layout per Xbox docs:
     *   [3:0]   USB0  (OHCI USB1)
     *   [7:4]   USB1  (OHCI USB2)
     *   [11:8]  NIC
     *   [15:12] APU
     *   [19:16] ACI
     *   [23:20] reserved
     *   [27:24] IDE
     *   [31:28] reserved
     */
    uint32_t r = pci_rd32(0, 1, 0, 0x6C);
    DbgPrint("hwprobe: LPC.INT_IRQ_ROUT=0x%08X\n", r);
    DbgPrint("hwprobe:   USB0->IRQ%s  USB1->IRQ%s  NIC->IRQ%s  APU->IRQ%s  ACI->IRQ%s  IDE->IRQ%s\n",
             nibble_irq(r, 0),
             nibble_irq(r, 4),
             nibble_irq(r, 8),
             nibble_irq(r, 12),
             nibble_irq(r, 16),
             nibble_irq(r, 24));
}

/* ---------- PCI per-device ---------- */

static void dump_pci_dev(const char *tag, uint8_t bus, uint8_t dev, uint8_t fn) {
    uint32_t vid_did = pci_rd32(bus, dev, fn, 0x00);
    if (vid_did == 0xFFFFFFFFu) {
        DbgPrint("hwprobe: %s (%u:%u.%u): not present\n", tag, bus, dev, fn);
        return;
    }
    uint32_t cmd_sts = pci_rd32(bus, dev, fn, 0x04);
    uint32_t class_rev = pci_rd32(bus, dev, fn, 0x08);
    uint32_t bar0 = pci_rd32(bus, dev, fn, 0x10);
    uint32_t int_pin_line = pci_rd32(bus, dev, fn, 0x3C);

    DbgPrint("hwprobe: %s (%u:%u.%u) vid=0x%04X did=0x%04X class=0x%06X "
             "cmd=0x%04X sts=0x%04X bar0=0x%08X line=%u pin=%u\n",
             tag, bus, dev, fn,
             vid_did & 0xFFFF, (vid_did >> 16) & 0xFFFF,
             class_rev >> 8,
             cmd_sts & 0xFFFF, (cmd_sts >> 16) & 0xFFFF,
             bar0,
             int_pin_line & 0xFF, (int_pin_line >> 8) & 0xFF);
}

/* ---------- OHCI MMIO ---------- */

static uint32_t mmio_r(uint32_t base, uint32_t off) {
    return *(volatile uint32_t *)(base + off);
}

static void dump_ohci(const char *tag, uint32_t base) {
    uint32_t rev    = mmio_r(base, 0x00);
    uint32_t ctl    = mmio_r(base, 0x04);
    uint32_t cmd    = mmio_r(base, 0x08);
    uint32_t isr    = mmio_r(base, 0x0C);
    uint32_t ier    = mmio_r(base, 0x10);
    uint32_t fminv  = mmio_r(base, 0x34);
    uint32_t fmnum  = mmio_r(base, 0x3C);
    uint32_t rhdA   = mmio_r(base, 0x48);
    int ndp = rhdA & 0xFF;
    int hcfs = (ctl >> 6) & 3;
    static const char *hcfs_names[] = {"Reset", "Resume", "Oper", "Suspend"};
    DbgPrint("hwprobe: %s rev=0x%02X ctl=0x%08X(HCFS=%s) cmdsts=0x%08X "
             "intsts=0x%08X inten=0x%08X fminv=0x%08X fmnum=0x%04X ndp=%d\n",
             tag, rev & 0xFF, ctl, hcfs_names[hcfs], cmd,
             isr, ier, fminv, fmnum & 0xFFFF, ndp);
}

/* ---------- main ---------- */

void main(void) {
    /* Tag start/end so it's easy to grep boundaries in either kernel's log. */
    DbgPrint("hwprobe: ==== begin ====\n");

    dump_pic();
    dump_lpc();

    /* Internal devices we care about.  Slot-number convention is the
     * standard Xbox PCI map. */
    dump_pci_dev("HOST",     0, 0, 0);
    dump_pci_dev("LPC",      0, 1, 0);
    dump_pci_dev("SMBUS",    0, 1, 1);
    dump_pci_dev("USB0",     0, 2, 0);
    dump_pci_dev("USB1",     0, 3, 0);
    dump_pci_dev("NIC",      0, 4, 0);
    dump_pci_dev("APU",      0, 5, 0);
    dump_pci_dev("ACI",      0, 6, 0);
    dump_pci_dev("IDE",      0, 9, 0);
    dump_pci_dev("AGPHOST",  0, 8, 0);
    dump_pci_dev("NV2A",     1, 0, 0);

    /* OHCI BARs are at fixed retail addresses; both kernels program the
     * same physical addresses since the BIOS already set BAR0. */
    dump_ohci("USB1", 0xFED00000u);
    dump_ohci("USB2", 0xFED08000u);

    DbgPrint("hwprobe: ==== end ====\n");

    /* Give the serial drainer a moment, then exit. */
    LARGE_INTEGER d; d.QuadPart = -((LONGLONG)200 * 10000);
    KeDelayExecutionThread(KernelMode, FALSE, &d);

    HalReturnToFirmware(HalQuickRebootRoutine);
}
