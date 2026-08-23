/*
 * stackpos: discover where the kernel positions its stacks, on BOTH nxkrnl
 * and the retail kernel (boot xemu with bios.bin).  Retail keeps no static
 * stack data in its image -- this finds the VA/PA region it allocates them
 * from so our boot/#DF/idle stacks can mimic it.
 *
 * All reads are black-box observation of the running kernel: exported APIs
 * (MmCreateKernelStack / MmGetPhysicalAddress) plus the live GDT/TSS read
 * with str/sgdt.  Never disassembly of bios.bin.  The current-thread read
 * uses the Xbox KTHREAD layout and is only meaningful on retail.
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

#define RD8(p)  (*(volatile uint8_t  *)(uintptr_t)(p))
#define RD16(p) (*(volatile uint16_t *)(uintptr_t)(p))
#define RD32(p) (*(volatile uint32_t *)(uintptr_t)(p))

static uint32_t pa_of(uint32_t va)
{
    return (uint32_t)MmGetPhysicalAddress((PVOID)(uintptr_t)va);
}

/* Read a dword from the KPCR through FS (FS base = KPCR on both kernels), so
 * we don't assume where the KPCR is flat-mapped. */
static uint32_t fs_rd(uint32_t off)
{
    uint32_t v;
    __asm__ __volatile__("movl %%fs:(%1), %0" : "=r"(v) : "r"(off));
    return v;
}

int main(void)
{
    say("stackpos: begin\n");

    /* 1. The kernel-stack allocator's region (layout-independent: exported). */
    {
        PVOID base = MmCreateKernelStack(0x3000, FALSE);   /* 12 KB, KERNEL_STACK_SIZE */
        if (!base)
        {
            say("MmCreateKernelStack(0x3000) -> NULL\n");
        }
        else
        {
            uint32_t vbase = (uint32_t)(uintptr_t)base;          /* high end (grows down) */
            uint32_t inside = vbase - 0x100;                     /* a committed byte */
            say("MmCreateKernelStack(0x3000): base(VA)=%08x  PA(base-0x100)=%08x\n",
                vbase, pa_of(inside));
            MmDeleteKernelStack(base, (PVOID)(uintptr_t)(vbase - 0x3000));
        }
    }

    /* 2. Current thread's kernel stack (Xbox KTHREAD: StackBase +0x1C,
     *    StackLimit +0x20, KernelStack +0x24).  Retail-layout only. */
    {
        uint32_t t = (uint32_t)(uintptr_t)KeGetCurrentThread();
        uint32_t sb = RD32(t + 0x1C), sl = RD32(t + 0x20), ks = RD32(t + 0x24);
        say("CurThread@%08x StackBase=%08x StackLimit=%08x KernelStack=%08x  PA(base-0x100)=%08x\n",
            t, sb, sl, ks, pa_of(sb - 0x100));
    }

    /* Read GDTR/IDTR/TR once for the descriptor-table sections below. */
    uint16_t tr;
    struct { uint16_t limit; uint32_t base; } __attribute__((packed)) gdtr, idtr;
    __asm__ __volatile__("str %0"  : "=m"(tr));
    __asm__ __volatile__("sgdt %0" : "=m"(gdtr));
    __asm__ __volatile__("sidt %0" : "=m"(idtr));

    /* Descriptor-table sizes.  limit = bytes - 1; IDT/GDT entries are 8 bytes. */
    say("GDT base=%08x limit=%u (%u entries)   IDT base=%08x limit=%u (%u entries)\n",
        gdtr.base, gdtr.limit, (gdtr.limit + 1) / 8,
        idtr.base, idtr.limit, (idtr.limit + 1) / 8);

    /* gdt_tss(selector) -> TSS linear base (GDT descriptor parse). */
    #define TSS_BASE(sel) ( RD16(gdtr.base + ((sel) & ~7u) + 2) \
        | ((uint32_t)RD8(gdtr.base + ((sel) & ~7u) + 4) << 16) \
        | ((uint32_t)RD8(gdtr.base + ((sel) & ~7u) + 7) << 24) )

    /* 3. Live main TSS Esp0 = the ring-0 transition stack (hardware). */
    {
        uint32_t tssbase = TSS_BASE(tr);
        uint32_t esp0 = RD32(tssbase + 4);
        say("MAIN GDT@%08x TR=%04x TSS@%08x Esp0=%08x  PA(esp0-0x100)=%08x\n",
            gdtr.base, tr, tssbase, esp0, pa_of(esp0 - 0x100));
    }

    /* 4. Idle thread stack via the KPCR through FS (no flat-VA assumption):
     *    Xbox NtTib.StackBase +0x04, IdleThread +0x30 (PrcbData @ KPCR+0x28).
     *    Retail-layout only. */
    {
        uint32_t tibBase = fs_rd(0x04), tibLimit = fs_rd(0x08);
        uint32_t idle = fs_rd(0x30);
        say("KPCR(fs) NtTib.StackBase=%08x StackLimit=%08x  IdleThread=%08x\n",
            tibBase, tibLimit, idle);
        if (idle >= 0x80000000u)
        {
            uint32_t sb = RD32(idle + 0x1C), sl = RD32(idle + 0x20), ks = RD32(idle + 0x24);
            say("IdleThread StackBase=%08x StackLimit=%08x KernelStack=%08x  PA(base-0x100)=%08x\n",
                sb, sl, ks, pa_of(sb - 0x100));
        }
    }

    /* 5. The #DF (double-fault) stack via the IDT[8] task gate -> DF-TSS
     *    (hardware, layout-independent).  This is the direct analog of our
     *    KiDoubleFaultStack carve. */
    {
        uint32_t gate = idtr.base + 8 * 8;       /* IDT vector 8, 8 bytes each */
        uint16_t sel = RD16(gate + 2);
        uint8_t  type = RD8(gate + 5);
        if ((type & 0x1F) == 0x05)               /* task gate */
        {
            uint32_t dftss = TSS_BASE(sel);
            uint32_t esp = RD32(dftss + 0x38), eip = RD32(dftss + 0x20);
            say("#DF IDT[8] taskgate sel=%04x DFTSS@%08x ESP=%08x EIP=%08x  PA(esp-0x100)=%08x\n",
                sel, dftss, esp, eip, pa_of(esp - 0x100));
        }
        else
        {
            say("#DF IDT[8] type=%02x (not a task gate) sel=%04x -- runs on Esp0\n",
                type, sel);
        }
    }

    /* 6. The NMI stack via the IDT[2] task gate -> NMI-TSS, same as #DF. */
    {
        uint32_t gate = idtr.base + 2 * 8;       /* IDT vector 2 */
        uint16_t sel = RD16(gate + 2);
        uint8_t  type = RD8(gate + 5);
        if ((type & 0x1F) == 0x05)               /* task gate */
        {
            uint32_t nmitss = TSS_BASE(sel);
            uint32_t esp = RD32(nmitss + 0x38), eip = RD32(nmitss + 0x20);
            say("NMI IDT[2] taskgate sel=%04x NMITSS@%08x ESP=%08x EIP=%08x  PA(esp-0x100)=%08x\n",
                sel, nmitss, esp, eip, pa_of(esp - 0x100));
        }
        else
        {
            say("NMI IDT[2] type=%02x (not a task gate) sel=%04x -- runs on Esp0\n",
                type, sel);
        }
    }

    say("stackpos: done\n");
    return 0;
}
