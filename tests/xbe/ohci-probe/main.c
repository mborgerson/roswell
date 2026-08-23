/*
 * OHCI port-status probe + libusbohci enumeration test.  Brings up the
 * USB stack the same way the dashboard does, then dumps:
 *   1. OHCI MMIO registers (control state + per-port status)
 *   2. The libusbohci device list (parents, ports, vid/pid)
 *
 * If the OHCI shows a port with CCS but the device list is empty, the
 * USB stack failed to enumerate it.  If the device list shows the hub
 * but not the xpad behind it, hub-port enumeration failed.  Etc.
 */
#include <xboxkrnl/xboxkrnl.h>
#include <usb.h>
#include <usbh_lib.h>
#include <xid_driver.h>
#include <stdint.h>

#define USB1_BASE  0xFED00000u
#define USB2_BASE  0xFED08000u

#define HC_CONTROL        0x04
#define HC_INT_STATUS     0x0C
#define HC_INT_ENABLE     0x10
#define HC_FM_NUMBER      0x3C
#define HC_RH_DESC_A      0x48
#define HC_RH_STATUS      0x50
#define HC_RH_PORT_STAT   0x54

#define RH_PORTSTAT_CCS   0x00000001u
#define RH_PORTSTAT_PES   0x00000002u
#define RH_PORTSTAT_PPS   0x00000100u
#define RH_PORTSTAT_LSDA  0x00000200u
#define RH_PORTSTAT_CSC   0x00010000u
#define RH_PORTSTAT_PRSC  0x00100000u

static uint32_t mmio_r(uint32_t base, uint32_t off)
{
    return *(volatile uint32_t *)(base + off);
}

static void dump_one(const char *tag, uint32_t base)
{
    uint32_t ctl = mmio_r(base, HC_CONTROL);
    uint32_t fmnum = mmio_r(base, HC_FM_NUMBER);
    uint32_t rhdescA = mmio_r(base, HC_RH_DESC_A);
    uint32_t intsts = mmio_r(base, HC_INT_STATUS);
    uint32_t intenb = mmio_r(base, HC_INT_ENABLE);
    int ndp = rhdescA & 0xFF;
    int hcfs = (ctl >> 6) & 3;
    static const char *hcfs_names[] = {"Reset", "Resume", "Operational",
                                       "Suspend"};

    DbgPrint("%s ctl=0x%08X HCFS=%s fmnum=0x%04X ndp=%d intsts=0x%08X intenb=0x%08X\n",
             tag, ctl, hcfs_names[hcfs], fmnum, ndp, intsts, intenb);

    for (int p = 0; p < ndp; p++) {
        uint32_t ps = mmio_r(base, HC_RH_PORT_STAT + 4 * p);
        DbgPrint("%s port%d status=0x%08X%s%s%s%s%s%s\n",
                 tag, p + 1, ps,
                 (ps & RH_PORTSTAT_CCS)  ? " CCS"  : "",
                 (ps & RH_PORTSTAT_PES)  ? " PES"  : "",
                 (ps & RH_PORTSTAT_PPS)  ? " PPS"  : "",
                 (ps & RH_PORTSTAT_LSDA) ? " LSDA" : "",
                 (ps & RH_PORTSTAT_CSC)  ? " CSC"  : "",
                 (ps & RH_PORTSTAT_PRSC) ? " PRSC" : "");
    }
}

static void dump_both(const char *label)
{
    DbgPrint("--- %s ---\n", label);
    dump_one("USB1", USB1_BASE);
    dump_one("USB2", USB2_BASE);
}

static void delay_ms(int ms)
{
    LARGE_INTEGER d;
    d.QuadPart = -((LONGLONG)ms * 10000);
    KeDelayExecutionThread(KernelMode, FALSE, &d);
}

static void dump_dev_list(void)
{
    int n = 0;
    DbgPrint("usbh device list:\n");
    xid_dev_t *xid = usbh_xid_get_device_list();
    while (xid != NULL) {
        DbgPrint("  xid %d: vid=0x%04X pid=0x%04X type=%d iface=%p\n",
                 xid->uid, xid->idVendor, xid->idProduct,
                 xid->xid_desc.bType, xid->iface);
        xid = xid->next;
        n++;
    }
    DbgPrint("  -> %d xid device(s)\n", n);
}

int main(void)
{
    DbgPrint("ohci-probe: pre-init OHCI state\n");
    dump_both("pre-init");

    DbgPrint("ohci-probe: usbh_core_init()\n");
    usbh_core_init();
    DbgPrint("ohci-probe: usbh_xid_init()\n");
    usbh_xid_init();

    dump_both("after usbh_*_init");

    /* Poll hubs repeatedly to let enumeration converge. */
    for (int i = 0; i < 20; i++) {
        usbh_pooling_hubs();
        delay_ms(100);
        if (i == 4 || i == 9 || i == 19) {
            DbgPrint("ohci-probe: after %d hub-polls\n", i + 1);
            dump_both("during");
            dump_dev_list();
        }
    }

    DbgPrint("ohci-probe: done\n");
    return 0;
}
