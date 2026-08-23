/*
 * usb-state-dump -- READ-ONLY OHCI/HCCA inspector.
 *
 * The purpose: launched as a regular Xbox title, let the underlying
 * kernel (retail xboxkrnl OR our nxkrnl) bring USB up however it
 * wants, then snapshot the hardware state so we can compare:
 *
 *   - HC control registers (PLE/CLE/BLE bits, HCFS state, ...)
 *   - HCCA layout (32-entry periodic ED list, frame number, done head)
 *   - Every endpoint descriptor reachable from hcca.intr[]
 *   - Every transfer descriptor queued on those EDs
 *   - The control / bulk ED chains via HcControlHeadED / HcBulkHeadED
 *
 * We do NOT call usbh_init / xid_init / anything that would mutate
 * the controller -- we want to see what the KERNEL has set up on
 * behalf of the dashboard / XAPI USBD before our XBE got control.
 *
 * Two snapshots taken: one immediately at startup (T0), one after
 * a delay (T1) so we can spot whether the periodic-list TD queue
 * drains over time the way it should during live polling.
 *
 * Output goes through DbgPrint (LPC UART under xemu).  Run via
 *   tools/run-xemu --official --dvd build/usb-state-dump.iso \
 *                  --serial file:/tmp/usb-state-retail.log
 * then again with --flash (nxkrnl) to compare.
 */

#include <xboxkrnl/xboxkrnl.h>
#include <stdint.h>

/* PCI BAR0 of the two Xbox OHCI controllers.  Retail and nxkrnl
 * both put them here; see hal/halx86/xbox/nxkrnl-pci.c. */
#define USB1_BASE  0xFED00000u
#define USB2_BASE  0xFED08000u

/* OHCI register offsets (OHCI 1.0a spec) */
#define HC_REVISION          0x00
#define HC_CONTROL           0x04
#define HC_COMMAND_STATUS    0x08
#define HC_INTERRUPT_STATUS  0x0C
#define HC_INTERRUPT_ENABLE  0x10
#define HC_INTERRUPT_DISABLE 0x14
#define HC_HCCA              0x18
#define HC_PERIOD_CURRENT_ED 0x1C
#define HC_CONTROL_HEAD_ED   0x20
#define HC_CONTROL_CURRENT_ED 0x24
#define HC_BULK_HEAD_ED      0x28
#define HC_BULK_CURRENT_ED   0x2C
#define HC_DONE_HEAD         0x30
#define HC_FM_INTERVAL       0x34
#define HC_FM_REMAINING      0x38
#define HC_FM_NUMBER         0x3C
#define HC_PERIODIC_START    0x40
#define HC_LS_THRESHOLD      0x44
#define HC_RH_DESCRIPTOR_A   0x48
#define HC_RH_DESCRIPTOR_B   0x4C
#define HC_RH_STATUS         0x50
#define HC_RH_PORT_STATUS_0  0x54

/* HcControl bits */
#define CTL_CBSR_MASK   0x00000003u
#define CTL_PLE         0x00000004u
#define CTL_IE          0x00000008u  /* IsoEnable */
#define CTL_CLE         0x00000010u
#define CTL_BLE         0x00000020u
#define CTL_HCFS_MASK   0x000000C0u
#define CTL_HCFS_SHIFT  6
#define CTL_IR          0x00000100u
#define CTL_RWC         0x00000200u
#define CTL_RWE         0x00000400u

/* HcInterruptStatus / Enable / Disable bits */
#define INT_SO          0x00000001u
#define INT_WDH         0x00000002u
#define INT_SF          0x00000004u
#define INT_RD          0x00000008u
#define INT_UE          0x00000010u
#define INT_FNO         0x00000020u
#define INT_RHSC        0x00000040u
#define INT_OC          0x40000000u
#define INT_MIE         0x80000000u

/* Root-hub port-status bits */
#define PS_CCS          0x00000001u
#define PS_PES          0x00000002u
#define PS_PSS          0x00000004u
#define PS_POCI         0x00000008u
#define PS_PRS          0x00000010u
#define PS_PPS          0x00000100u
#define PS_LSDA         0x00000200u
#define PS_CSC          0x00010000u
#define PS_PESC         0x00020000u
#define PS_PSSC         0x00040000u
#define PS_OCIC         0x00080000u
#define PS_PRSC         0x00100000u

/* HCCA layout (OHCI 1.0a sec 4.4) */
typedef struct {
    uint32_t intr[32];      /* periodic ED list head pointers (PA) */
    uint16_t frame_number;
    uint16_t pad1;
    uint32_t done_head;     /* PA of completed-TD chain head */
    uint8_t  reserved[116];
} HCCA;

/* Endpoint descriptor (OHCI 1.0a sec 4.2) */
typedef struct {
    uint32_t control;       /* MPS|F|K|S|D|EN|FA */
    uint32_t tail_p;        /* TD queue tail (PA) */
    uint32_t head_p;        /* TD queue head (PA), low bits: H(halt)|C(toggle)|R */
    uint32_t next_ed;       /* PA of next ED in list */
} ED;

/* General transfer descriptor (OHCI 1.0a sec 4.3.1) */
typedef struct {
    uint32_t control;       /* CC|EC|T|DI|DP|R|BUFRND */
    uint32_t cbp;           /* current buffer pointer (PA) */
    uint32_t next_td;       /* PA of next TD */
    uint32_t be;            /* buffer end (PA) */
} TD;

/* The Xbox KSEG0 alias: any contiguously-allocated PA is also visible
 * at VA = 0x80000000 | PA.  HCCA / EDs / TDs all live in that range
 * (MmAllocateContiguousMemory). */
#define KSEG0(pa) ((void *)((uintptr_t)(pa) | 0x80000000u))

static uint32_t mmio_r(uint32_t base, uint32_t off)
{
    return *(volatile uint32_t *)(uintptr_t)(base + off);
}

static void delay_ms(uint32_t ms)
{
    LARGE_INTEGER d;
    d.QuadPart = -((LONGLONG)ms * 10000);
    KeDelayExecutionThread(KernelMode, FALSE, &d);
}

static const char *hcfs_name(uint32_t ctl)
{
    static const char *names[] = {"Reset", "Resume", "Operational", "Suspend"};
    return names[(ctl & CTL_HCFS_MASK) >> CTL_HCFS_SHIFT];
}

static void dump_int_flags(const char *prefix, uint32_t v)
{
    DbgPrint("%s 0x%08X%s%s%s%s%s%s%s%s%s\n", prefix, v,
             (v & INT_SO)   ? " SO"   : "",
             (v & INT_WDH)  ? " WDH"  : "",
             (v & INT_SF)   ? " SF"   : "",
             (v & INT_RD)   ? " RD"   : "",
             (v & INT_UE)   ? " UE"   : "",
             (v & INT_FNO)  ? " FNO"  : "",
             (v & INT_RHSC) ? " RHSC" : "",
             (v & INT_OC)   ? " OC"   : "",
             (v & INT_MIE)  ? " MIE"  : "");
}

static void dump_port_status(const char *tag, int port, uint32_t v)
{
    DbgPrint("%s port%d 0x%08X%s%s%s%s%s%s LSDA=%d%s%s%s%s%s\n",
             tag, port, v,
             (v & PS_CCS)  ? " CCS"  : "",
             (v & PS_PES)  ? " PES"  : "",
             (v & PS_PSS)  ? " PSS"  : "",
             (v & PS_POCI) ? " POCI" : "",
             (v & PS_PRS)  ? " PRS"  : "",
             (v & PS_PPS)  ? " PPS"  : "",
             !!(v & PS_LSDA),
             (v & PS_CSC)  ? " CSC"  : "",
             (v & PS_PESC) ? " PESC" : "",
             (v & PS_PSSC) ? " PSSC" : "",
             (v & PS_OCIC) ? " OCIC" : "",
             (v & PS_PRSC) ? " PRSC" : "");
}

static void dump_ed(const char *tag, uint32_t pa, int depth)
{
    if (pa == 0) return;
    if (depth > 8) {
        DbgPrint("%s   ... (depth cap)\n", tag);
        return;
    }

    ED *ed = (ED *)KSEG0(pa & ~0xFu);
    uint32_t ctl = ed->control;
    uint32_t addr = ctl & 0x7F;
    uint32_t en   = (ctl >> 7) & 0xF;
    uint32_t dir  = (ctl >> 11) & 0x3;
    uint32_t spd  = (ctl >> 13) & 0x1;
    uint32_t skip = (ctl >> 14) & 0x1;
    uint32_t fmt  = (ctl >> 15) & 0x1;
    uint32_t mps  = (ctl >> 16) & 0x7FF;
    int halt = (ed->head_p & 1);
    int toggle_carry = (ed->head_p & 2) >> 1;

    static const char *dir_str[] = {"FromTD", "OUT", "IN", "FromTD"};

    DbgPrint("%s ED@%08X ctl=%08X addr=%d ep=%d dir=%s%s%s%s mps=%d "
             "tail=%08X head=%08X%s%s next=%08X\n",
             tag, pa, ctl, addr, en, dir_str[dir],
             spd  ? "/LS"  : "/FS",
             skip ? "/SKIP" : "",
             fmt  ? "/ISO" : "",
             mps, ed->tail_p, ed->head_p & ~0xFu,
             halt ? " HALT" : "",
             toggle_carry ? " C" : "",
             ed->next_ed);

    /* Walk TD list if it has anything queued. */
    uint32_t head = ed->head_p & ~0xFu;
    uint32_t tail = ed->tail_p & ~0xFu;
    if (head != tail) {
        uint32_t td_pa = head;
        int td_idx = 0;
        while (td_pa != 0 && td_pa != tail && td_idx < 16) {
            TD *td = (TD *)KSEG0(td_pa);
            uint32_t tctl = td->control;
            uint32_t cc = (tctl >> 28) & 0xF;
            uint32_t ec = (tctl >> 26) & 0x3;
            uint32_t t  = (tctl >> 24) & 0x3;
            uint32_t di = (tctl >> 21) & 0x7;
            uint32_t dp = (tctl >> 19) & 0x3;
            uint32_t r  = (tctl >> 18) & 0x1;
            static const char *dp_str[] = {"SETUP", "OUT", "IN", "RSV"};
            DbgPrint("%s     TD@%08X ctl=%08X CC=%d EC=%d T=%d DI=%d DP=%s%s "
                     "cbp=%08X be=%08X next=%08X\n",
                     tag, td_pa, tctl, cc, ec, t, di, dp_str[dp],
                     r ? "/R" : "",
                     td->cbp, td->be, td->next_td);
            td_pa = td->next_td;
            td_idx++;
        }
        if (td_pa != 0 && td_pa != tail)
            DbgPrint("%s     ... (TD list cap)\n", tag);
    } else {
        DbgPrint("%s   (no TDs queued; head==tail)\n", tag);
    }

    /* Recurse into next ED on this list (sibling). */
    if (ed->next_ed != 0)
        dump_ed(tag, ed->next_ed, depth + 1);
}

static void dump_chain(const char *tag, uint32_t head_pa)
{
    if (head_pa == 0) {
        DbgPrint("%s (empty)\n", tag);
        return;
    }
    dump_ed(tag, head_pa, 0);
}

static void dump_hcca(const char *tag, uint32_t hcca_pa)
{
    if (hcca_pa == 0) {
        DbgPrint("%s HCCA: NULL (controller not yet wired up)\n", tag);
        return;
    }
    HCCA *hcca = (HCCA *)KSEG0(hcca_pa);
    DbgPrint("%s HCCA@%08X frame=%u done_head=%08X\n",
             tag, hcca_pa, hcca->frame_number, hcca->done_head);

    /* Print the 32 periodic-list head pointers, collapsing runs of NULLs. */
    int last_set = -1;
    for (int i = 0; i < 32; i++) {
        if (hcca->intr[i] != 0) {
            DbgPrint("%s   intr[%2d] head=%08X%s\n",
                     tag, i, hcca->intr[i],
                     (last_set >= 0 && i - last_set > 1) ? " (after NULLs)" : "");
            last_set = i;
        }
    }
    if (last_set < 0)
        DbgPrint("%s   intr[*] all NULL\n", tag);

    /* Walk the distinct EDs reachable from intr[].  Each EDs may appear at
     * multiple intr[] slots (OHCI periodic-list balancing), but the ED+TD
     * walk is idempotent so duplicate prints are fine. */
    for (int i = 0; i < 32; i++) {
        if (hcca->intr[i] == 0) continue;
        char heading[16];
        /* DbgPrint formats only %d; use a short manual itoa instead of %d */
        heading[0] = tag[0]; heading[1] = tag[1]; heading[2] = tag[2];
        heading[3] = tag[3]; heading[4] = ' '; heading[5] = 'i';
        heading[6] = '0' + (i / 10);
        heading[7] = '0' + (i % 10);
        heading[8] = 0;
        dump_ed(heading, hcca->intr[i], 0);
    }
}

static void dump_one(const char *tag, uint32_t base)
{
    uint32_t rev    = mmio_r(base, HC_REVISION);
    uint32_t ctl    = mmio_r(base, HC_CONTROL);
    uint32_t cs     = mmio_r(base, HC_COMMAND_STATUS);
    uint32_t intsts = mmio_r(base, HC_INTERRUPT_STATUS);
    uint32_t intenb = mmio_r(base, HC_INTERRUPT_ENABLE);
    uint32_t hcca_pa = mmio_r(base, HC_HCCA);
    uint32_t pced   = mmio_r(base, HC_PERIOD_CURRENT_ED);
    uint32_t ched   = mmio_r(base, HC_CONTROL_HEAD_ED);
    uint32_t cced   = mmio_r(base, HC_CONTROL_CURRENT_ED);
    uint32_t bhed   = mmio_r(base, HC_BULK_HEAD_ED);
    uint32_t bced   = mmio_r(base, HC_BULK_CURRENT_ED);
    uint32_t dhead  = mmio_r(base, HC_DONE_HEAD);
    uint32_t fmint  = mmio_r(base, HC_FM_INTERVAL);
    uint32_t fmrem  = mmio_r(base, HC_FM_REMAINING);
    uint32_t fmnum  = mmio_r(base, HC_FM_NUMBER);
    uint32_t perst  = mmio_r(base, HC_PERIODIC_START);
    uint32_t lst    = mmio_r(base, HC_LS_THRESHOLD);
    uint32_t rhdA   = mmio_r(base, HC_RH_DESCRIPTOR_A);
    uint32_t rhdB   = mmio_r(base, HC_RH_DESCRIPTOR_B);
    uint32_t rhst   = mmio_r(base, HC_RH_STATUS);

    DbgPrint("%s rev=0x%04X ctl=0x%08X HCFS=%s PLE=%d CLE=%d BLE=%d IE=%d\n",
             tag, rev & 0xFF, ctl, hcfs_name(ctl),
             !!(ctl & CTL_PLE), !!(ctl & CTL_CLE),
             !!(ctl & CTL_BLE), !!(ctl & CTL_IE));
    DbgPrint("%s cs=0x%08X fmint=0x%08X fmnum=%u fmrem=0x%08X perstart=0x%08X lst=0x%08X\n",
             tag, cs, fmint, fmnum, fmrem, perst, lst);
    dump_int_flags(tag, intsts);
    DbgPrint("%s intenb=", tag);
    dump_int_flags("", intenb);
    DbgPrint("%s pced=%08X ched=%08X cced=%08X bhed=%08X bced=%08X dhead=%08X\n",
             tag, pced, ched, cced, bhed, bced, dhead);
    DbgPrint("%s rhdA=0x%08X rhdB=0x%08X rhst=0x%08X ndp=%d\n",
             tag, rhdA, rhdB, rhst, rhdA & 0xFF);

    int ndp = rhdA & 0xFF;
    for (int p = 0; p < ndp && p < 8; p++) {
        uint32_t ps = mmio_r(base, HC_RH_PORT_STATUS_0 + 4 * p);
        dump_port_status(tag, p + 1, ps);
    }

    dump_hcca(tag, hcca_pa);

    DbgPrint("%s control-list chain (from HcControlHeadED=%08X):\n", tag, ched);
    dump_chain(tag, ched);
    DbgPrint("%s bulk-list chain (from HcBulkHeadED=%08X):\n", tag, bhed);
    dump_chain(tag, bhed);
}

static void snapshot(const char *label)
{
    DbgPrint("==================== %s ====================\n", label);
    DbgPrint("USB1 @ 0x%08X:\n", USB1_BASE);
    dump_one("USB1", USB1_BASE);
    DbgPrint("\n");
    DbgPrint("USB2 @ 0x%08X:\n", USB2_BASE);
    dump_one("USB2", USB2_BASE);
    DbgPrint("==================== end %s ====================\n\n", label);
}

void main(void)
{
    /* Give the kernel a beat to finish whatever it does post-launch.
     * Retail's XAPI USBD device thread runs for ~500 ms after
     * XInitDevices to bring everything up (see cxbx-reloaded
     * Xapi.cpp).  Wait a bit longer than that. */
    delay_ms(1500);
    snapshot("T0  (1.5s after launch)");

    /* Wait a bit and re-snapshot so we can see the periodic list
     * actually drain/refill if polling is healthy. */
    delay_ms(2500);
    snapshot("T1  (4.0s after launch)");

    /* Final wait + dump to catch settled steady-state. */
    delay_ms(4000);
    snapshot("T2  (8.0s after launch)");

    /* Stay alive so the bootloader doesn't reboot. */
    for (;;) {
        delay_ms(60000);
    }
}
