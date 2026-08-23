// mtrr-dump -- dump the x86 MTRRs (memory-type range registers) over the
// debug UART.  Run on the official Xbox kernel (a permitted black-box oracle)
// to capture the memory-type layout nxkrnl must reproduce.  Xbox titles
// run in ring 0, so RDMSR is allowed.

#include <xboxkrnl/xboxkrnl.h>

static unsigned long long rdmsr(unsigned int msr)
{
    unsigned int lo, hi;
    __asm__ __volatile__("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((unsigned long long)hi << 32) | lo;
}

static void show(const char *name, unsigned int msr)
{
    unsigned long long v = rdmsr(msr);
    DbgPrint("mtrr-dump: %s [0x%03X] = %08X:%08X\n", name, msr,
             (unsigned int)(v >> 32), (unsigned int)(v & 0xFFFFFFFFu));
}

int main(void)
{
    DbgPrint("mtrr-dump: ===== begin =====\n");

    show("MTRRcap",     0x0FE);
    show("MTRRdefType", 0x2FF);

    show("PhysBase0", 0x200); show("PhysMask0", 0x201);
    show("PhysBase1", 0x202); show("PhysMask1", 0x203);
    show("PhysBase2", 0x204); show("PhysMask2", 0x205);
    show("PhysBase3", 0x206); show("PhysMask3", 0x207);
    show("PhysBase4", 0x208); show("PhysMask4", 0x209);
    show("PhysBase5", 0x20A); show("PhysMask5", 0x20B);
    show("PhysBase6", 0x20C); show("PhysMask6", 0x20D);
    show("PhysBase7", 0x20E); show("PhysMask7", 0x20F);

    show("Fix64K_00000", 0x250);
    show("Fix16K_80000", 0x258);
    show("Fix16K_A0000", 0x259);
    show("Fix4K_C0000",  0x268);
    show("Fix4K_C8000",  0x269);
    show("Fix4K_D0000",  0x26A);
    show("Fix4K_D8000",  0x26B);
    show("Fix4K_E0000",  0x26C);
    show("Fix4K_E8000",  0x26D);
    show("Fix4K_F0000",  0x26E);
    show("Fix4K_F8000",  0x26F);

    /* Paging + cache control state. */
    {
        unsigned int cr0, cr3, cr4;
        __asm__ __volatile__("mov %%cr0,%0" : "=r"(cr0));
        __asm__ __volatile__("mov %%cr3,%0" : "=r"(cr3));
        __asm__ __volatile__("mov %%cr4,%0" : "=r"(cr4));
        DbgPrint("mtrr-dump: CR0 = %08X (PG=%u WP=%u CD=%u NW=%u)\n",
                 cr0, (cr0>>31)&1, (cr0>>16)&1, (cr0>>30)&1, (cr0>>29)&1);
        DbgPrint("mtrr-dump: CR3 = %08X (PD base PA)\n", cr3);
        DbgPrint("mtrr-dump: CR4 = %08X (PSE=%u PAE=%u PGE=%u)\n",
                 cr4, (cr4>>4)&1, (cr4>>5)&1, (cr4>>7)&1);
    }
    show("IA32_PAT", 0x277);

    DbgPrint("mtrr-dump: ===== end =====\n");
    return 0;
}
