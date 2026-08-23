/*
 * OHCI / PCI / IRQ snapshot.  Dumps everything a title needs to know
 * about how the kernel programmed the USB host controllers, so we can
 * compare a nxkrnl boot against a retail-kernel boot side-by-side.
 *
 * Dumps:
 *   1. PCI config space for USB1 (00:02.0) and USB2 (00:03.0):
 *      vendor/device, command, status, BAR0 (MMIO base), interrupt
 *      line/pin, and the chipset-specific register at offset 0x50.
 *   2. OHCI MMIO state pre- and post- usbh_core_init():
 *      HcRevision, HcControl (CBSR/IE/CLE/BLE/PLE/HCFS),
 *      HcCommandStatus, HcInterruptStatus/Enable, HcHCCA pointer,
 *      HcControl/Bulk/Done HeadED, HcFmInterval, HcFmRemaining,
 *      HcFmNumber, HcRhDescriptorA/B, HcRhStatus, all port status.
 *   3. The IRQ vector each kernel hands back for the title's
 *      HalGetInterruptVector(USB_IRQ, ...) call.
 *
 * Read the prints into a side-by-side diff to see what nxkrnl is doing
 * differently from retail.
 */
#include <xboxkrnl/xboxkrnl.h>
#include <usbh_lib.h>
#include <stdint.h>

#define USB1_PCI_DEV  2  /* 00:02.0 -- OHCI USB1 */
#define USB2_PCI_DEV  3  /* 00:03.0 -- OHCI USB2 */

#define USB1_MMIO_BASE  0xFED00000u
#define USB2_MMIO_BASE  0xFED08000u

static uint32_t pci_read32(int dev, int off)
{
    /* PCI config is read via HalReadWritePCISpace, but that function is
     * export-only -- inline the CF8/CFC dance the same way the HAL does,
     * since this is a no-bus-handler probe. */
    uint32_t addr = 0x80000000u | (dev << 11) | (off & 0xFC);
    __asm__ volatile("outl %0, %1" :: "a"(addr), "Nd"((uint16_t)0xCF8));
    uint32_t val;
    __asm__ volatile("inl %1, %0" : "=a"(val) : "Nd"((uint16_t)0xCFC));
    return val;
}

static void dump_pci(const char *tag, int dev)
{
    uint32_t vid_did = pci_read32(dev, 0x00);
    uint32_t cmd_sts = pci_read32(dev, 0x04);
    uint32_t classrev = pci_read32(dev, 0x08);
    uint32_t bar0    = pci_read32(dev, 0x10);
    uint32_t intr    = pci_read32(dev, 0x3C);
    uint32_t reg50   = pci_read32(dev, 0x50);

    DbgPrint("%s PCI(00:%02d.0): vendor/device=0x%08X cmd/sts=0x%08X class/rev=0x%08X\n",
             tag, dev, vid_did, cmd_sts, classrev);
    DbgPrint("%s   BAR0=0x%08X  interrupt(line/pin/min/max)=0x%08X  reg0x50=0x%08X\n",
             tag, bar0, intr, reg50);
    DbgPrint("%s   cmd: BusMaster=%d MemSpace=%d IOSpace=%d  interrupt-line=%d pin=%d\n",
             tag,
             (cmd_sts >> 2) & 1, (cmd_sts >> 1) & 1, (cmd_sts >> 0) & 1,
             intr & 0xFF, (intr >> 8) & 0xFF);
}

static uint32_t mmio_r(uint32_t base, uint32_t off)
{
    return *(volatile uint32_t *)(base + off);
}

#define HC_REVISION       0x00
#define HC_CONTROL        0x04
#define HC_CMDSTATUS      0x08
#define HC_INT_STATUS     0x0C
#define HC_INT_ENABLE     0x10
#define HC_HCCA           0x18
#define HC_CTRL_HEAD_ED   0x20
#define HC_BULK_HEAD_ED   0x28
#define HC_DONE_HEAD      0x30
#define HC_FM_INTERVAL    0x34
#define HC_FM_REMAINING   0x38
#define HC_FM_NUMBER      0x3C
#define HC_PERIODIC_START 0x40
#define HC_RH_DESC_A      0x48
#define HC_RH_DESC_B      0x4C
#define HC_RH_STATUS      0x50
#define HC_RH_PORT_STAT   0x54

static void dump_ohci(const char *tag, uint32_t base)
{
    uint32_t rev   = mmio_r(base, HC_REVISION);
    uint32_t ctl   = mmio_r(base, HC_CONTROL);
    uint32_t cmds  = mmio_r(base, HC_CMDSTATUS);
    uint32_t ints  = mmio_r(base, HC_INT_STATUS);
    uint32_t inte  = mmio_r(base, HC_INT_ENABLE);
    uint32_t hcca  = mmio_r(base, HC_HCCA);
    uint32_t chead = mmio_r(base, HC_CTRL_HEAD_ED);
    uint32_t bhead = mmio_r(base, HC_BULK_HEAD_ED);
    uint32_t done  = mmio_r(base, HC_DONE_HEAD);
    uint32_t fmiv  = mmio_r(base, HC_FM_INTERVAL);
    uint32_t fmrem = mmio_r(base, HC_FM_REMAINING);
    uint32_t fmno  = mmio_r(base, HC_FM_NUMBER);
    uint32_t pst   = mmio_r(base, HC_PERIODIC_START);
    uint32_t rha   = mmio_r(base, HC_RH_DESC_A);
    uint32_t rhb   = mmio_r(base, HC_RH_DESC_B);
    uint32_t rhs   = mmio_r(base, HC_RH_STATUS);

    int hcfs = (ctl >> 6) & 3;
    static const char *hcfs_n[] = {"Reset", "Resume", "Operational", "Suspend"};

    DbgPrint("%s OHCI@0x%08X: rev=0x%08X\n", tag, base, rev);
    DbgPrint("%s   ctl=0x%08X  HCFS=%s PLE=%d IE=%d CLE=%d BLE=%d CBSR=%d\n",
             tag, ctl, hcfs_n[hcfs],
             (ctl >> 2) & 1, (ctl >> 3) & 1, (ctl >> 4) & 1, (ctl >> 5) & 1,
             ctl & 3);
    DbgPrint("%s   cmdsts=0x%08X intsts=0x%08X intenb=0x%08X\n",
             tag, cmds, ints, inte);
    DbgPrint("%s   hcca=0x%08X ctrl_head=0x%08X bulk_head=0x%08X done=0x%08X\n",
             tag, hcca, chead, bhead, done);
    DbgPrint("%s   fminterval=0x%08X fmremaining=0x%08X fmnumber=0x%04X periodic_start=0x%08X\n",
             tag, fmiv, fmrem, fmno, pst);
    DbgPrint("%s   rhDescA=0x%08X rhDescB=0x%08X rhStatus=0x%08X (ndp=%d)\n",
             tag, rha, rhb, rhs, rha & 0xFF);
    int ndp = rha & 0xFF;
    for (int p = 0; p < ndp; p++) {
        uint32_t ps = mmio_r(base, HC_RH_PORT_STAT + 4 * p);
        DbgPrint("%s   port%d=0x%08X%s%s%s%s%s%s\n",
                 tag, p + 1, ps,
                 (ps & 0x001) ? " CCS"  : "",
                 (ps & 0x002) ? " PES"  : "",
                 (ps & 0x100) ? " PPS"  : "",
                 (ps & 0x200) ? " LSDA" : "",
                 (ps & 0x10000) ? " CSC"  : "",
                 (ps & 0x100000) ? " PRSC" : "");
    }
}

int main(void)
{
    DbgPrint("=== ohci-compare ===\n");

    /* 1. PCI config -- what the kernel programmed */
    DbgPrint("--- PCI config ---\n");
    dump_pci("USB1", USB1_PCI_DEV);
    dump_pci("USB2", USB2_PCI_DEV);

    /* 2. IRQ vector -- what HalGetInterruptVector tells the title */
    DbgPrint("--- IRQ vectors ---\n");
    {
        KIRQL irql;
        ULONG v1 = HalGetInterruptVector(1, &irql);
        DbgPrint("  HalGetInterruptVector(1) -> vector=0x%X irql=%d\n",
                 (unsigned)v1, (int)irql);
        ULONG v9 = HalGetInterruptVector(9, &irql);
        DbgPrint("  HalGetInterruptVector(9) -> vector=0x%X irql=%d\n",
                 (unsigned)v9, (int)irql);
    }

    /* 3. OHCI state -- pre-init */
    DbgPrint("--- OHCI state BEFORE usbh_core_init ---\n");
    dump_ohci("USB1", USB1_MMIO_BASE);
    dump_ohci("USB2", USB2_MMIO_BASE);

    /* 4. Run libusbohci's init and dump again */
    DbgPrint("--- usbh_core_init() ---\n");
    usbh_core_init();
    {
        LARGE_INTEGER d;
        d.QuadPart = -2000000; /* 200 ms */
        KeDelayExecutionThread(KernelMode, FALSE, &d);
    }
    DbgPrint("--- OHCI state AFTER usbh_core_init ---\n");
    dump_ohci("USB1", USB1_MMIO_BASE);
    dump_ohci("USB2", USB2_MMIO_BASE);

    DbgPrint("=== ohci-compare done ===\n");
    return 0;
}
