/*
 * sdl-pad-probe: verify SDL2's game-controller pipeline sees a connected
 * xpad on the OHCI hub, and that button presses make it through SDL into
 * the title.
 *
 * Avoid SDL_GetTicks for deadlines -- on this kernel that wall-clock
 * isn't advancing the way SDL expects -- and iterate explicitly instead.
 *
 * Run interactively under xemu with a controller bound to port 1, focus
 * the window, and press buttons.  The serial log shows what reached the
 * title and at which layer.
 */
#include <xboxkrnl/xboxkrnl.h>
#include <SDL.h>
#include <stdio.h>
#include <string.h>

/* libusbohci ISR/DPC counters -- proves whether the title's interrupt
 * handlers ever run.  Defined in third_party/nxdk/lib/usb/libusbohci_xbox/
 * usbh_xbox.c. */
extern volatile uint32_t usbh_isr_count;
extern volatile uint32_t usbh_dpc_count;
extern volatile uint32_t usbh_isr_last_ip;

/* SDL xpad int-callback diagnostic counters (libSDL2). */
extern volatile uint32_t sdl_xbox_int_cb_count;
extern volatile uint32_t sdl_xbox_int_cb_early_exit;
extern volatile uint32_t sdl_xbox_int_cb_no_hwdata;
extern volatile uint32_t sdl_xbox_int_cb_resubmit_ok;
extern volatile int32_t  sdl_xbox_int_cb_last_status;
extern volatile uint32_t sdl_xbox_int_cb_last_resubmit_ret;

/* Raw OHCI MMIO accessors (USB1 is at 0xFED00000).  We don't go through
 * libusbohci's _ohci pointer because that's static-private to the .lib; the
 * standard OHCI register offsets are well known. */
#define OHCI1_BASE              0xFED00000u
#define OHCI_HcRevision         0x00u
#define OHCI_HcControl          0x04u
#define OHCI_HcCommandStatus    0x08u
#define OHCI_HcInterruptStatus  0x0Cu
#define OHCI_HcInterruptEnable  0x10u
#define OHCI_HcHCCA             0x18u
#define OHCI_HcPeriodCurrentED  0x1Cu
#define OHCI_HcControlHeadED    0x20u
#define OHCI_HcBulkHeadED       0x28u
#define OHCI_HcDoneHead         0x30u
#define OHCI_HcFmInterval       0x34u
#define OHCI_HcFmNumber         0x3Cu

/* HcControl bits */
#define OHCI_CTL_PLE            (1u << 2)
#define OHCI_CTL_IE             (1u << 3)
#define OHCI_CTL_CLE            (1u << 4)
#define OHCI_CTL_BLE            (1u << 5)
#define OHCI_CTL_HCFS_MASK      (3u << 6)
#define OHCI_CTL_HCFS_OPER      (2u << 6)

/* HcInterruptEnable bits */
#define OHCI_INTR_SO            (1u << 0)
#define OHCI_INTR_WDH           (1u << 1)
#define OHCI_INTR_SF            (1u << 2)
#define OHCI_INTR_MIE           (1u << 31)

static uint32_t ohci_rd(uint32_t off) {
    return *(volatile uint32_t *)(OHCI1_BASE + off);
}

/* x86 port I/O for PIC + EFLAGS introspection.  IRQ1 = OHCI USB1 on Xbox;
 * masking it at the PIC, or running with IF clear, would explain why the
 * title-side ISR never fires even though xemu is asserting the IRQ line. */
static inline uint8_t pio_inb(uint16_t port) {
    uint8_t v;
    __asm__ __volatile__("inb %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}
static inline void pio_outb(uint16_t port, uint8_t v) {
    __asm__ __volatile__("outb %0, %1" : : "a"(v), "Nd"(port));
}
static inline uint32_t read_eflags(void) {
    uint32_t f;
    __asm__ __volatile__("pushfl; popl %0" : "=r"(f));
    return f;
}

/* KPCR.CurrentIrql lives at fs:0x24 on i386 in ReactOS.  ReactOS x86
 * implements IRQL via PIC IMR manipulation, so an unexpectedly elevated
 * IRQL would explain IRQs sitting in IRR forever -- they're masked at
 * the PIC by KeRaiseIrql until somebody lowers back to PASSIVE. */
static inline uint8_t read_kirql(void) {
    uint8_t v;
    __asm__ __volatile__("movb %%fs:0x24, %0" : "=r"(v));
    return v;
}

/* PIC1 (master): cmd 0x20, data 0x21.  PIC2 (slave): cmd 0xA0, data 0xA1.
 * OCW3 = 0x0A → read IRR, 0x0B → read ISR. */
static void dump_pic_state(const char *tag) {
    uint8_t imr1 = pio_inb(0x21);
    uint8_t imr2 = pio_inb(0xA1);
    pio_outb(0x20, 0x0A); uint8_t irr1 = pio_inb(0x20);
    pio_outb(0x20, 0x0B); uint8_t isr1 = pio_inb(0x20);
    pio_outb(0xA0, 0x0A); uint8_t irr2 = pio_inb(0xA0);
    pio_outb(0xA0, 0x0B); uint8_t isr2 = pio_inb(0xA0);
    uint32_t flags = read_eflags();
    uint8_t  irql  = read_kirql();
    DbgPrint("pic[%s]: PIC1 imr=0x%02X irr=0x%02X isr=0x%02X | "
             "PIC2 imr=0x%02X irr=0x%02X isr=0x%02X | EFLAGS.IF=%d IRQL=%u\n",
             tag, imr1, irr1, isr1, imr2, irr2, isr2,
             (flags >> 9) & 1, irql);
}

/* PCI config reads via the CF8/CFC mechanism.  Used to inspect the LPC's
 * INT-IRQ routing register at config 0x6C (retail value 0x0e065491 routes
 * USB0->IRQ1, USB1->IRQ9, etc.) and the OHCI device's interrupt pin/line. */
static uint32_t pci_cfg_rd(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t reg) {
    uint32_t addr = 0x80000000u | ((uint32_t)bus << 16) |
                    ((uint32_t)dev << 11) | ((uint32_t)fn << 8) | (reg & 0xFC);
    __asm__ __volatile__("outl %0, %1" : : "a"(addr), "Nd"((uint16_t)0xCF8));
    uint32_t v;
    __asm__ __volatile__("inl %1, %0" : "=a"(v) : "Nd"((uint16_t)0xCFC));
    return v;
}

static void dump_pci_state(const char *tag) {
    /* LPC (slot 1) routing for internal devices: each nibble in reg 0x6C
     * maps a pirq index to a PIC IRQ; nibble 0 = USB0, nibble 1 = USB1.
     * If nibble 0 == 0, OHCI USB0 IRQs are dropped at the LPC. */
    uint32_t lpc_6c = pci_cfg_rd(0, 1, 0, 0x6C);
    /* USB0 (slot 2) PCI config: 0x04 = cmd/status (BusMaster, MemSpace),
     * 0x3C = interrupt line (8b) + interrupt pin (8b). */
    uint32_t usb0_04 = pci_cfg_rd(0, 2, 0, 0x04);
    uint32_t usb0_3C = pci_cfg_rd(0, 2, 0, 0x3C);
    DbgPrint("pci[%s]: LPC.0x6C=0x%08X (USB0->IRQ%u USB1->IRQ%u NIC->IRQ%u APU->IRQ%u ACI->IRQ%u IDE->IRQ%u)\n",
             tag, lpc_6c,
             (unsigned)(lpc_6c >> 0) & 0xF, (unsigned)(lpc_6c >> 4) & 0xF,
             (unsigned)(lpc_6c >> 8) & 0xF, (unsigned)(lpc_6c >> 12) & 0xF,
             (unsigned)(lpc_6c >> 16) & 0xF, (unsigned)(lpc_6c >> 24) & 0xF);
    DbgPrint("pci[%s]: USB0.cmd=0x%04X status=0x%04X int_line=%u int_pin=%u\n",
             tag, usb0_04 & 0xFFFF, (usb0_04 >> 16) & 0xFFFF,
             usb0_3C & 0xFF, (usb0_3C >> 8) & 0xFF);
}

/* IDT[0x31] = OHCI vector (HalGetInterruptVector(1) = 0x31 per prior probe).
 * Dump the descriptor so we can see whether KeConnectInterrupt actually
 * pointed it at a real handler, or left a default trap stub there. */
static void dump_idt_vec(uint32_t vec) {
    struct __attribute__((packed)) { uint16_t limit; uint32_t base; } idtr;
    __asm__ __volatile__("sidt %0" : "=m"(idtr));
    if (vec * 8 + 7 > idtr.limit) {
        DbgPrint("idt[%u]: out of range (limit=0x%04X)\n", vec, idtr.limit);
        return;
    }
    volatile uint8_t *e = (volatile uint8_t *)(idtr.base + vec * 8);
    uint16_t off_lo = e[0] | (e[1] << 8);
    uint16_t sel    = e[2] | (e[3] << 8);
    uint8_t  ist    = e[4];
    uint8_t  attr   = e[5];
    uint16_t off_hi = e[6] | (e[7] << 8);
    uint32_t handler = ((uint32_t)off_hi << 16) | off_lo;
    DbgPrint("idt[%u]: base=0x%08X handler=0x%08X sel=0x%04X attr=0x%02X(P=%d DPL=%d type=%d)\n",
             vec, idtr.base, handler, sel, attr,
             (attr >> 7) & 1, (attr >> 5) & 3, attr & 0xF);
}

static void dump_ohci_state(const char *tag) {
    uint32_t ctl   = ohci_rd(OHCI_HcControl);
    uint32_t cmd   = ohci_rd(OHCI_HcCommandStatus);
    uint32_t isr   = ohci_rd(OHCI_HcInterruptStatus);
    uint32_t ier   = ohci_rd(OHCI_HcInterruptEnable);
    uint32_t hcca  = ohci_rd(OHCI_HcHCCA);
    uint32_t pcur  = ohci_rd(OHCI_HcPeriodCurrentED);
    uint32_t chead = ohci_rd(OHCI_HcControlHeadED);
    uint32_t bhead = ohci_rd(OHCI_HcBulkHeadED);
    uint32_t dhead = ohci_rd(OHCI_HcDoneHead);
    uint32_t fn1   = ohci_rd(OHCI_HcFmNumber);

    DbgPrint("ohci[%s]: ctl=0x%08X (%s%s%s%s HCFS=%u) cmd=0x%08X isr=0x%08X ier=0x%08X\n",
             tag, ctl,
             (ctl & OHCI_CTL_PLE) ? "PLE " : "",
             (ctl & OHCI_CTL_IE)  ? "IE "  : "",
             (ctl & OHCI_CTL_CLE) ? "CLE " : "",
             (ctl & OHCI_CTL_BLE) ? "BLE " : "",
             (ctl >> 6) & 3,
             cmd, isr, ier);
    DbgPrint("ohci[%s]:   ier_flags=%s%s%s%s\n", tag,
             (ier & OHCI_INTR_MIE) ? "MIE " : "",
             (ier & OHCI_INTR_WDH) ? "WDH " : "",
             (ier & OHCI_INTR_SF)  ? "SF "  : "",
             (ier & OHCI_INTR_SO)  ? "SO "  : "");
    DbgPrint("ohci[%s]:   hcca=0x%08X periodCurrED=0x%08X ctrlHead=0x%08X bulkHead=0x%08X done=0x%08X\n",
             tag, hcca, pcur, chead, bhead, dhead);

    /* Re-read fm number after a short delay to verify the controller is
     * actually ticking frames -- if it's stopped this stays the same. */
    for (volatile int i = 0; i < 100000; i++) ;
    uint32_t fn2 = ohci_rd(OHCI_HcFmNumber);
    DbgPrint("ohci[%s]:   fmNumber: %u -> %u (delta=%d)\n",
             tag, fn1, fn2, (int)(fn2 - fn1));

    /* HCCA is in contiguous memory below 64 MB; OR with 0x80000000 to map
     * the physical address back into the title's virtual space. */
    if (hcca) {
        volatile uint32_t *intr_table =
            (volatile uint32_t *)(hcca | 0x80000000u);
        /* hcca layout: intr[32] (0..127), frameno (128), done_head (132) */
        uint32_t frame    = intr_table[32];
        uint32_t hcca_done = intr_table[33];
        DbgPrint("ohci[%s]:   hcca.frame=0x%08X hcca.done=0x%08X\n",
                 tag, frame, hcca_done);

        /* int_table[0] is the head of the every-1ms interrupt list -- xpad
         * (4ms) chains off here too once linked. */
        for (int idx = 0; idx < 4; idx++) {
            uint32_t head = intr_table[idx];
            DbgPrint("ohci[%s]:   intr_table[%d] head=0x%08X", tag, idx, head);
            int hops = 0;
            uint32_t cur = head;
            while (cur && hops < 6) {
                volatile uint32_t *ed =
                    (volatile uint32_t *)((cur & ~0xFu) | 0x80000000u);
                uint32_t info  = ed[0];
                uint32_t tailp = ed[1];
                uint32_t headp = ed[2];
                uint32_t next  = ed[3];
                DbgPrint("    ED@0x%08X info=0x%08X (FA=%u EN=%u D=%u S=%u K=%u F=%u)"
                         " head=0x%08X tail=0x%08X next=0x%08X",
                         cur, info, info & 0x7F, (info>>7)&0xF, (info>>11)&0x3,
                         (info>>13)&1, (info>>14)&1, (info>>15)&1,
                         headp, tailp, next);
                cur = next;
                hops++;
            }
            DbgPrint("\n");
        }
    }
}

static const char *button_name(int btn)
{
    switch (btn) {
    case SDL_CONTROLLER_BUTTON_A:             return "A";
    case SDL_CONTROLLER_BUTTON_B:             return "B";
    case SDL_CONTROLLER_BUTTON_X:             return "X";
    case SDL_CONTROLLER_BUTTON_Y:             return "Y";
    case SDL_CONTROLLER_BUTTON_BACK:          return "BACK";
    case SDL_CONTROLLER_BUTTON_GUIDE:         return "GUIDE";
    case SDL_CONTROLLER_BUTTON_START:         return "START";
    case SDL_CONTROLLER_BUTTON_LEFTSTICK:     return "LSTICK";
    case SDL_CONTROLLER_BUTTON_RIGHTSTICK:    return "RSTICK";
    case SDL_CONTROLLER_BUTTON_LEFTSHOULDER:  return "LBUMPER";
    case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER: return "RBUMPER";
    case SDL_CONTROLLER_BUTTON_DPAD_UP:       return "DUP";
    case SDL_CONTROLLER_BUTTON_DPAD_DOWN:     return "DDOWN";
    case SDL_CONTROLLER_BUTTON_DPAD_LEFT:     return "DLEFT";
    case SDL_CONTROLLER_BUTTON_DPAD_RIGHT:    return "DRIGHT";
    default:                                  return "?";
    }
}

/* Event watcher: SDL calls this for every event pushed, before it
 * lands in the queue.  If we see the event here but never via
 * SDL_PollEvent, the event is being filtered or the queue is broken. */
static int total_watched = 0;
static int SDLCALL pad_event_watcher(void *userdata, SDL_Event *e)
{
    if (e->type == SDL_JOYBUTTONDOWN || e->type == SDL_JOYBUTTONUP ||
        e->type == SDL_JOYAXISMOTION || e->type == SDL_JOYHATMOTION ||
        e->type == SDL_CONTROLLERBUTTONDOWN ||
        e->type == SDL_CONTROLLERBUTTONUP ||
        e->type == SDL_CONTROLLERAXISMOTION) {
        DbgPrint("  WATCH type=0x%X timestamp=%u\n", e->type, e->common.timestamp);
        total_watched++;
    }
    return 1;
}

int main(void)
{
    /* Dump OHCI root-hub port status for both controllers, all ports.
     * Player N → root port port_map[N-1] where port_map = {3,4,1,2} -- so
     * looking at port 1 alone misses the actual gamepad attachment point. */
    {
        for (int p = 0; p < 4; p++) {
            uint32_t s1 = *(volatile uint32_t *)(0xFED00054u + p * 4);
            uint32_t s2 = *(volatile uint32_t *)(0xFED08054u + p * 4);
            DbgPrint("sdl-pad-probe: USB1 port%d=0x%08X  USB2 port%d=0x%08X\n",
                     p + 1, s1, p + 1, s2);
        }
    }

    DbgPrint("sdl-pad-probe: entering main\n");
    /* Poll the nxkrnl-side kith_count counter directly from kernel memory.
     * If this is non-zero, KiInterruptTemplateHandler IS being called.
     * Try the binary's expected address AND the relocated-by-0x130 alt. */
    {
        volatile uint32_t *kith = (volatile uint32_t *)0x80463240u;
        volatile uint32_t *any  = (volatile uint32_t *)0x80463244u;
        volatile uint32_t *kith_alt = (volatile uint32_t *)(0x80463240u - 0x130u);
        volatile uint32_t *any_alt  = (volatile uint32_t *)(0x80463244u - 0x130u);
        DbgPrint("sdl-pad-probe: kith=0x80463240=%u  any=0x80463244=%u  "
                 "kith_alt=0x80463110=%u  any_alt=0x80463114=%u\n",
                 *kith, *any, *kith_alt, *any_alt);
        /* Also check the LIVE template bytes at the relocated addr. */
        volatile uint8_t *live_tpl = (volatile uint8_t *)(0x80013C9Cu - 0x130u);
        char buf[200]; int bn=0;
        bn += sprintf(buf+bn, "  LIVE_template@0x%08X bytes:", (uint32_t)live_tpl);
        for (int i = 0; i < 16; i++) bn += sprintf(buf+bn, " %02X", live_tpl[i]);
        DbgPrint("%s\n", buf);
        /* Dump bytes at the trampoline's jump target 0x80186590 (LIVE KITH). */
        volatile uint8_t *live_kith = (volatile uint8_t *)0x80186590u;
        char buf2[200]; int bn2=0;
        bn2 += sprintf(buf2+bn2, "  LIVE_kith@0x80186590 bytes:");
        for (int i = 0; i < 32; i++) bn2 += sprintf(buf2+bn2, " %02X", live_kith[i]);
        DbgPrint("%s\n", buf2);
        /* Scan a wide range of kernel .text for KITH prologue pattern. */
        int found = 0;
        for (uint32_t scan = 0x80011000u; scan <= 0x80300000u && found < 5; scan += 1) {
            volatile uint8_t *p = (volatile uint8_t *)scan;
            if (p[0] == 0x55 && p[1] == 0x89 && p[2] == 0xe5 &&
                p[3] == 0x56 && p[4] == 0x89 && p[5] == 0xd6 &&
                p[6] == 0x53 && p[7] == 0x89 && p[8] == 0xcb &&
                p[9] == 0x83 && p[10] == 0xec && p[11] == 0x10) {
                DbgPrint("  Found KITH-pattern at LIVE 0x%08X\n", scan);
                found++;
            }
        }
        if (found == 0) DbgPrint("  KITH-pattern NOT found in [0x80011000..0x80300000]\n");
        /* Dump 32 bytes at each candidate location to verify it's KITH. */
        for (uint32_t cand_idx = 0; cand_idx < 2; cand_idx++) {
            uint32_t cands[2] = { 0x8004B7B0u, 0x801E7F00u };
            uint32_t addr = cands[cand_idx];
            volatile uint8_t *p = (volatile uint8_t *)addr;
            char buf[200]; int bn=0;
            bn += sprintf(buf+bn, "  CAND@0x%08X bytes:", addr);
            for (int i = 0; i < 32; i++) bn += sprintf(buf+bn, " %02X", p[i]);
            DbgPrint("%s\n", buf);
        }
        /* Dump IDT entries for all enabled hardware IRQ vectors so we can
         * see which ones have valid trampolines installed and which
         * point at garbage. */
        struct __attribute__((packed)) { uint16_t limit; uint32_t base; } idtr;
        __asm__ __volatile__("sidt %0" : "=m"(idtr));
        for (uint32_t v = 0x30; v <= 0x3F; v++) {
            volatile uint8_t *e = (volatile uint8_t *)(idtr.base + v * 8);
            uint16_t off_lo = e[0] | (e[1] << 8);
            uint16_t off_hi = e[6] | (e[7] << 8);
            uint32_t handler = ((uint32_t)off_hi << 16) | off_lo;
            volatile uint8_t *h = (volatile uint8_t *)handler;
            char buf[100]; int n=0;
            n += sprintf(buf+n, "  IDT[0x%02X]=0x%08X bytes:", v, handler);
            for (int i = 0; i < 8; i++) n += sprintf(buf+n, " %02X", h[i]);
            DbgPrint("%s\n", buf);
        }
        /* Also peek the first 32 bytes of the IDT[0x31] handler so we can
         * tell whether it's the KiInterruptTemplate trampoline (mov/jmp
         * pattern) or some other handler. */
        {
            volatile uint8_t *h = (volatile uint8_t *)0x8046B180u;
            DbgPrint("  IDT[0x31] handler bytes:");
            for (int i = 0; i < 32; i++) {
                DbgPrint(" %02X", h[i]);
            }
            DbgPrint("\n");
        }
        /* Compare to KiInterruptTemplate at 0x80013c9c -- our expected
         * template source. */
        {
            volatile uint8_t *t = (volatile uint8_t *)0x80013C9Cu;
            char buf[200]; int n=0;
            n += sprintf(buf+n, "  KiInterruptTemplate bytes:");
            for (int i = 0; i < 32; i++) n += sprintf(buf+n, " %02X", t[i]);
            DbgPrint("%s\n", buf);
        }
        /* Also peek where NxbeInterruptShadows[0]'s DispatchCode should be
         * (.bss 0x1F140 + 4 offset for XboxInterrupt + 0x3C for KINTERRUPT
         * DispatchCode = 0x8046C180). */
        {
            volatile uint8_t *s = (volatile uint8_t *)0x8046C180u;
            char buf[200]; int n=0;
            n += sprintf(buf+n, "  Shadow[0].DispatchCode@0x8046C180:");
            for (int i = 0; i < 32; i++) n += sprintf(buf+n, " %02X", s[i]);
            DbgPrint("%s\n", buf);
        }
        /* And the bytes 0x1000 below (where IDT[0x31] actually points). */
        {
            volatile uint8_t *s = (volatile uint8_t *)0x8046B180u;
            char buf[200]; int n=0;
            n += sprintf(buf+n, "  IDT-handler-loc@0x8046B180:");
            for (int i = 0; i < 32; i++) n += sprintf(buf+n, " %02X", s[i]);
            DbgPrint("%s\n", buf);
        }
    }
    dump_ohci_state("pre-Init");

    /* Sanity check: write a known sentinel to HcInterruptEnable directly
     * from the title, then read it back.  If xemu sees a different value
     * than what we wrote, the MMIO routing or value translation is broken.
     * Sentinel = 0x80000006 = MIE | WDH | SF (same as libusbohci's init). */
    DbgPrint("sdl-pad-probe: sentinel write 0x80000006 to HcIntEnable\n");
    *(volatile uint32_t *)(OHCI1_BASE + OHCI_HcInterruptEnable) = 0x80000006u;
    uint32_t back = *(volatile uint32_t *)(OHCI1_BASE + OHCI_HcInterruptEnable);
    DbgPrint("sdl-pad-probe: read back HcIntEnable=0x%08X\n", back);
    /* Clear it again before SDL_Init to avoid spurious IRQs. */
    *(volatile uint32_t *)(OHCI1_BASE + 0x14u /* HcIntDisable */) = 0xFFFFFFFFu;

    /* Hypothesis: SDL_PrivateJoystickShouldIgnoreEvent returns TRUE for
     * PRESSED state because something thinks we have a window without
     * focus.  Setting this hint disables that filter unconditionally. */
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    DbgPrint("sdl-pad-probe: ALLOW_BACKGROUND_EVENTS hint set\n");

    DbgPrint("sdl-pad-probe: about to SDL_Init(GAMECONTROLLER)\n");

    if (SDL_Init(SDL_INIT_GAMECONTROLLER) < 0) {
        DbgPrint("SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }

    DbgPrint("sdl-pad-probe: SDL_Init returned OK\n");

    /* Dump current event-state for everything we care about, so we know
     * whether something is filtering joystick events before they reach
     * the queue. */
    DbgPrint("  event_state: JOYBUTTONDOWN=%d JOYBUTTONUP=%d JOYAXISMOTION=%d\n",
             SDL_EventState(SDL_JOYBUTTONDOWN, SDL_QUERY),
             SDL_EventState(SDL_JOYBUTTONUP, SDL_QUERY),
             SDL_EventState(SDL_JOYAXISMOTION, SDL_QUERY));
    DbgPrint("  event_state: CONTROLLERBUTTONDOWN=%d CONTROLLERBUTTONUP=%d CONTROLLERAXISMOTION=%d\n",
             SDL_EventState(SDL_CONTROLLERBUTTONDOWN, SDL_QUERY),
             SDL_EventState(SDL_CONTROLLERBUTTONUP, SDL_QUERY),
             SDL_EventState(SDL_CONTROLLERAXISMOTION, SDL_QUERY));
    {
        SDL_EventFilter f;
        void *fd;
        SDL_GetEventFilter(&f, &fd);
        DbgPrint("  SDL_GetEventFilter -> filter=%p userdata=%p\n",
                 (void *)f, fd);
    }

    /* Install a watcher so we see every pushed event regardless of
     * whether SDL_PollEvent returns it. */
    SDL_AddEventWatch(pad_event_watcher, NULL);
    DbgPrint("sdl-pad-probe: event watcher installed\n");

    /* Phase 1: drain events.  Keep going until we see a controller or
     * 10 s elapse, with progress dumps every 1 s so we can see whether
     * enumeration is moving. */
    DbgPrint("sdl-pad-probe: phase 1: drain events for up to 10 s\n");
    int joystick_added = 0, controller_added = 0;
    SDL_GameController *ctrl = NULL;
    SDL_Event e;
    for (int sec = 0; sec < 10; sec++) {
        for (int i = 0; i < 10; i++) {
            while (SDL_PollEvent(&e)) {
                if (e.type == SDL_JOYDEVICEADDED) {
                    joystick_added++;
                    DbgPrint("  SDL_JOYDEVICEADDED idx=%d\n", e.jdevice.which);
                } else if (e.type == SDL_CONTROLLERDEVICEADDED) {
                    controller_added++;
                    DbgPrint("  SDL_CONTROLLERDEVICEADDED idx=%d\n",
                             e.cdevice.which);
                    if (ctrl == NULL) {
                        ctrl = SDL_GameControllerOpen(e.cdevice.which);
                        DbgPrint("  SDL_GameControllerOpen -> %p\n", ctrl);
                        if (ctrl == NULL) {
                            DbgPrint("    error: %s\n", SDL_GetError());
                        } else {
                            const char *name = SDL_GameControllerName(ctrl);
                            DbgPrint("    name: '%s'\n", name ? name : "(null)");
                        }
                    }
                }
            }
            SDL_Delay(100);
        }
        volatile uint32_t *usb1_port1 = (volatile uint32_t *)0xFED00054u;
        DbgPrint("  [%ds] USB1 port1=0x%08X NumJoysticks=%d added=%d\n",
                 sec + 1, *usb1_port1, SDL_NumJoysticks(), joystick_added);
        if (ctrl) {
            break;
        }
    }

    /* Phase 2: directly query the joystick count, in case events weren't
     * dispatched yet (this is the dashboard's pattern of relying on
     * events; if events never fire we want to know). */
    int njoy = SDL_NumJoysticks();
    DbgPrint("sdl-pad-probe: SDL_NumJoysticks=%d  joystick_added=%d  controller_added=%d\n",
             njoy, joystick_added, controller_added);
    for (int i = 0; i < njoy; i++) {
        SDL_JoystickGUID g = SDL_JoystickGetDeviceGUID(i);
        char guid_str[40];
        SDL_JoystickGetGUIDString(g, guid_str, sizeof(guid_str));
        const char *name = SDL_JoystickNameForIndex(i);
        SDL_bool is_ctrl = SDL_IsGameController(i);
        DbgPrint("  joy[%d]: name='%s' guid=%s is_game_controller=%d\n",
                 i, name ? name : "(null)", guid_str, (int)is_ctrl);

        if (ctrl == NULL && is_ctrl) {
            ctrl = SDL_GameControllerOpen(i);
            DbgPrint("  (fallback) SDL_GameControllerOpen(%d) -> %p\n",
                     i, ctrl);
            if (ctrl == NULL) {
                DbgPrint("    error: %s\n", SDL_GetError());
            }
        }
    }

    if (ctrl == NULL) {
        DbgPrint("sdl-pad-probe: no controller available -- did you bind one to port 1?\n");
        dump_ohci_state("no-ctrl");
        dump_idt_vec(0x31);
        dump_pic_state("no-ctrl");
        DbgPrint("sdl-pad-probe: done\n");
        SDL_Quit();
        return 1;
    }

    /* Snapshot the OHCI / periodic schedule state right after open: this
     * is the moment SDL has called usbh_xid_read, which should have linked
     * the xpad's interrupt ED into the HCCA int_table and set HcControl.PLE.
     * If we see PLE clear or the intr_table[N] still empty, the int xfer
     * never made it into the schedule. */
    dump_ohci_state("post-open");
    dump_idt_vec(0x31);
    /* POST-OPEN: dump the FULL trampoline at IDT[0x31] handler and the
     * jump-target it references.  The template has 'mov $imm32, %eax;
     * jmp *%eax' at offset 0x96 in our build (the immediate is the
     * absolute address of KiInterruptTemplateHandler). */
    {
        struct __attribute__((packed)) { uint16_t limit; uint32_t base; } idtr;
        __asm__ __volatile__("sidt %0" : "=m"(idtr));
        volatile uint8_t *e = (volatile uint8_t *)(idtr.base + 0x31 * 8);
        uint16_t off_lo = e[0] | (e[1] << 8);
        uint16_t off_hi = e[6] | (e[7] << 8);
        uint32_t handler = ((uint32_t)off_hi << 16) | off_lo;
        volatile uint8_t *h = (volatile uint8_t *)handler;
        char tpl_dump[2000]; int td = 0;
        td += sprintf(tpl_dump+td, "  POST-OPEN trampoline@0x%08X:", handler);
        for (int i = 0; i < 160; i++) td += sprintf(tpl_dump+td, " %02X", h[i]);
        DbgPrint("%s\n", tpl_dump);
        /* KITH counter check */
        volatile uint32_t *k = (volatile uint32_t *)0x80463240u;
        volatile uint32_t *a = (volatile uint32_t *)0x80463244u;
        DbgPrint("  post-open kith_count=%u any_count=%u\n", *k, *a);
        /* Read 4 bytes at template+0x97 (where the KITH-jump immediate
         * is supposed to be) and 4 bytes at template+0x97-0x130 in case
         * the kernel got PE-relocated. */
        {
            volatile uint32_t *imm_a = (volatile uint32_t *)(0x80013C9Cu + 0x97u);
            volatile uint32_t *imm_b = (volatile uint32_t *)((0x80013C9Cu - 0x130u) + 0x97u);
            DbgPrint("  template-imm @0x%08X = 0x%08X; alt-imm @0x%08X = 0x%08X\n",
                     (uint32_t)imm_a, *imm_a, (uint32_t)imm_b, *imm_b);
        }
        /* The kernel appears to have been relocated by -0x130 at load
         * time.  Read the LIVE addresses (binary - 0x130) for the
         * template start and kith_count. */
        {
            volatile uint8_t *live_tpl = (volatile uint8_t *)(0x80013C9Cu - 0x130u);
            DbgPrint("  LIVE template@0x%08X bytes (first 32):",
                     (uint32_t)live_tpl);
            char buf[200]; int bn=0;
            for (int i = 0; i < 32; i++) bn += sprintf(buf+bn, " %02X", live_tpl[i]);
            DbgPrint("%s\n", buf);
            volatile uint32_t *live_kith = (volatile uint32_t *)(0x80463240u - 0x130u);
            volatile uint32_t *live_any  = (volatile uint32_t *)(0x80463244u - 0x130u);
            DbgPrint("  LIVE kith@0x%08X=%u  any@0x%08X=%u\n",
                     (uint32_t)live_kith, *live_kith,
                     (uint32_t)live_any, *live_any);
        }
    }
    dump_pic_state("post-open");
    dump_pci_state("post-open");

    /* Watch HcInterruptStatus over a few seconds: if WDH (bit 1) stays set
     * without being cleared, the OHCI controller is asserting IRQ1 but the
     * title's ISR is never running.  We expect the ISR/DPC pair to clear
     * WDH each time it processes the done queue, so a stuck WDH is the
     * smoking gun for an interrupt-delivery failure. */
    /* Tight observation: watch usbh_isr_count and HcInterruptStatus while
     * the title is in a pure busy-spin (NO SDL_Delay, NO KeDelayExecutionThread).
     * If isr_n keeps incrementing here, IRQ delivery is fine and the freeze
     * is caused by SDL_Delay/KeDelay perturbing the dispatcher.  If isr_n
     * still freezes here, the bug is internal to the IRQ chain. */
    DbgPrint("sdl-pad-probe: busy-spin watch phase (4s, no SDL_Delay)\n");
    {
        uint32_t base_isr_n  = usbh_isr_count;
        uint32_t base_fmn    = ohci_rd(OHCI_HcFmNumber);
        uint32_t last_isr_n  = base_isr_n;
        uint32_t freeze_fmn  = 0;
        int freeze_detected  = 0;
        /* OHCI frame ticks every 1ms.  Run for ~4000 frames = 4s.  Print
         * a status sample every 250 frames (250ms).  Also dump the moment
         * isr_n stops advancing for >= 500 frames (500ms quiet). */
        for (uint32_t target = 1; target <= 4000; target++) {
            /* spin until HcFmNumber advances by `target` frames from base */
            while ((uint32_t)(ohci_rd(OHCI_HcFmNumber) - base_fmn) < target)
                ;
            uint32_t cur_isr_n = usbh_isr_count;
            uint32_t cur_isr   = ohci_rd(OHCI_HcInterruptStatus);
            if (cur_isr_n != last_isr_n) {
                last_isr_n  = cur_isr_n;
                freeze_fmn  = base_fmn + target;
            } else if (!freeze_detected &&
                       (base_fmn + target) - freeze_fmn > 500) {
                /* No isr_n change in 500ms -- log the freeze moment ONCE. */
                freeze_detected = 1;
                DbgPrint("sdl-pad-probe: FREEZE detected at fmn=%u (last "
                         "isr_n change at fmn=%u, no change for 500ms)\n",
                         base_fmn + target, freeze_fmn);
                DbgPrint("  isr_n=%u dpc_n=%u OHCI_isr=0x%08X\n",
                         cur_isr_n, (unsigned)usbh_dpc_count, cur_isr);
                DbgPrint("  int_cb: count=%u early_exit=%u no_hwdata=%u "
                         "resubmit_ok=%u last_status=%d last_ret=0x%08X\n",
                         (unsigned)sdl_xbox_int_cb_count,
                         (unsigned)sdl_xbox_int_cb_early_exit,
                         (unsigned)sdl_xbox_int_cb_no_hwdata,
                         (unsigned)sdl_xbox_int_cb_resubmit_ok,
                         (int)sdl_xbox_int_cb_last_status,
                         (unsigned)sdl_xbox_int_cb_last_resubmit_ret);
                dump_pic_state("freeze");
                dump_idt_vec(0x31);
                /* Dump the bytes at the IDT[0x31] handler and at the
                 * Shadow[0].DispatchCode address.  We expect them to be
                 * the KiInterruptTemplate trampoline (~83 EC 68 ...).
                 * If they're zero or random, the trampoline never got
                 * installed correctly. */
                {
                    volatile uint8_t *h = (volatile uint8_t *)0x8046B180u;
                    char buf[300]; int bn = 0;
                    bn += sprintf(buf+bn, "  IDT@0x8046B180 bytes:");
                    for (int i = 0; i < 64; i++) bn += sprintf(buf+bn, " %02X", h[i]);
                    DbgPrint("%s\n", buf);
                }
                {
                    volatile uint8_t *s = (volatile uint8_t *)0x8046C180u;
                    char buf[300]; int bn = 0;
                    bn += sprintf(buf+bn, "  Shadow0@0x8046C180 bytes:");
                    for (int i = 0; i < 64; i++) bn += sprintf(buf+bn, " %02X", s[i]);
                    DbgPrint("%s\n", buf);
                }
                /* Also poll kith_count -- if non-zero, the trampoline IS
                 * being entered and our DPC theory is wrong. */
                {
                    volatile uint32_t *k = (volatile uint32_t *)0x80463240u;
                    DbgPrint("  kith_count@0x80463240 = %u\n", *k);
                }
                dump_ohci_state("freeze");
                dump_pci_state("freeze");
                /* Also read USB0's PCI_STATUS bit 3 (INTERRUPT_STATUS):
                 * 1 = device is currently asserting INTx, 0 = not.  This is
                 * pci_update_irq_status' truth: if it's 1 here, the OHCI
                 * side is correctly asserting and routing must be broken;
                 * if it's 0, the OHCI's intr_update logic is broken. */
                {
                    uint32_t s = pci_cfg_rd(0, 2, 0, 0x04);
                    DbgPrint("  USB0.PCI_STATUS=0x%04X (INT.status bit3=%u)\n",
                             (unsigned)((s >> 16) & 0xFFFF),
                             (unsigned)((s >> 19) & 1));
                }

                /* Try a manual non-specific EOI on PIC1 -- if some IRQ is
                 * stuck in-service, this clears it and a queued IRQ1
                 * should fire as soon as we resample.  OCW2 = 0x20. */
                DbgPrint("  attempting manual PIC1 EOI (OCW2=0x20)...\n");
                uint32_t pre_isr_n = usbh_isr_count;
                pio_outb(0x20, 0x20);
                /* Burn 100k cycles to give any pending IRQ time to fire. */
                for (volatile int s = 0; s < 100000; s++) ;
                DbgPrint("  post-EOI: isr_n=%u (was %u)\n",
                         (unsigned)usbh_isr_count, pre_isr_n);
                dump_pic_state("post-eoi");

                /* Also try toggling MIE off/on by hand to provoke a new
                 * 0->1 edge on the PCI INTx line. */
                DbgPrint("  toggling MIE off/on by hand...\n");
                pre_isr_n = usbh_isr_count;
                *(volatile uint32_t *)(OHCI1_BASE + 0x14) = 0x80000000u;
                for (volatile int s = 0; s < 100000; s++) ;
                *(volatile uint32_t *)(OHCI1_BASE + 0x10) = 0x80000000u;
                for (volatile int s = 0; s < 100000; s++) ;
                DbgPrint("  post-MIE-toggle: isr_n=%u (was %u)\n",
                         (unsigned)usbh_isr_count, pre_isr_n);
                dump_pic_state("post-mie-toggle");
                {
                    uint32_t s = pci_cfg_rd(0, 2, 0, 0x04);
                    DbgPrint("  post-mie USB0.PCI_STATUS=0x%04X (INT.status=%u)\n",
                             (unsigned)((s >> 16) & 0xFFFF),
                             (unsigned)((s >> 19) & 1));
                }
            }
            if (target % 250 == 0) {
                DbgPrint("  [fmn+%u/%u ms] isr_n=%u dpc_n=%u OHCI_isr=0x%08X\n",
                         target, target,
                         cur_isr_n, (unsigned)usbh_dpc_count, cur_isr);
            }
        }
        DbgPrint("sdl-pad-probe: busy-spin done. base=%u final=%u  delta=%u\n",
                 base_isr_n, last_isr_n, last_isr_n - base_isr_n);
    }

    /* Phase 3: poll for button transitions / axis motion for ~30 s of
     * wall time (300 iterations x 100 ms).  Focus the xemu window and
     * press buttons. */
    DbgPrint("sdl-pad-probe: controller open -- press buttons for ~30 s\n");
    Uint8 prev_buttons[SDL_CONTROLLER_BUTTON_MAX] = {0};
    int total_events = 0, total_state_transitions = 0;

    for (int i = 0; i < 300; i++) {
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_CONTROLLERBUTTONDOWN) {
                DbgPrint("  CONTROLLERBUTTONDOWN  %s\n",
                         button_name(e.cbutton.button));
                total_events++;
            } else if (e.type == SDL_CONTROLLERBUTTONUP) {
                DbgPrint("  CONTROLLERBUTTONUP    %s\n",
                         button_name(e.cbutton.button));
                total_events++;
            } else if (e.type == SDL_CONTROLLERAXISMOTION) {
                if (e.caxis.value < -8000 || e.caxis.value > 8000) {
                    DbgPrint("  CONTROLLERAXISMOTION  axis=%d value=%d\n",
                             e.caxis.axis, e.caxis.value);
                    total_events++;
                }
            } else if (e.type == SDL_CONTROLLERDEVICEREMOVED) {
                DbgPrint("  CONTROLLERDEVICEREMOVED\n");
            } else if (e.type == SDL_JOYBUTTONDOWN) {
                DbgPrint("  JOYBUTTONDOWN  button=%d\n", e.jbutton.button);
            } else if (e.type == SDL_JOYBUTTONUP) {
                DbgPrint("  JOYBUTTONUP    button=%d\n", e.jbutton.button);
            } else if (e.type == SDL_JOYAXISMOTION) {
                if (e.jaxis.value < -8000 || e.jaxis.value > 8000) {
                    DbgPrint("  JOYAXISMOTION  axis=%d value=%d\n",
                             e.jaxis.axis, e.jaxis.value);
                }
            } else if (e.type == SDL_JOYHATMOTION) {
                DbgPrint("  JOYHATMOTION   hat=%d value=%d\n",
                         e.jhat.hat, e.jhat.value);
            } else {
                /* Unmatched event type -- useful to see if SDL is
                 * spewing events we don't care about. */
                DbgPrint("  (other event type=0x%X)\n", e.type);
            }
        }

        /* Direct state polling as a fallback -- if SDL events are
         * silently dropped but the underlying state is updated, this
         * still shows transitions. */
        for (int b = 0; b < SDL_CONTROLLER_BUTTON_MAX; b++) {
            Uint8 cur = SDL_GameControllerGetButton(ctrl, b);
            if (cur != prev_buttons[b]) {
                DbgPrint("  (poll) %s %s\n",
                         cur ? "down" : "up  ", button_name(b));
                prev_buttons[b] = cur;
                total_state_transitions++;
            }
        }

        SDL_Delay(100);
    }

    DbgPrint("sdl-pad-probe: done.  events=%d state_transitions=%d watched=%d\n",
             total_events, total_state_transitions, total_watched);
    dump_ohci_state("at-exit");
    SDL_GameControllerClose(ctrl);
    SDL_Quit();
    return 0;
}
