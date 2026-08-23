/*
 * Xbox kernel boot setup.
 *
 * boot/nxldr decompresses + PE-maps the kernel, sets up paging, and jumps to
 * KiSystemStartup passing NOTHING.  KiSystemStartup's SARCH_XBOX prologue calls
 * the routines here -- before it reads the GDT/IDT/TR or the LoaderBlock -- to
 * install the NT-shaped CPU descriptors and synthesize the
 * LOADER_PARAMETER_BLOCK that the generic path then consumes.
 *
 * Everything is either a compile-time constant (the fixed Xbox physical layout,
 * boot-layout.h, shared with the loader) or readable from our own PE header
 * (SizeOfImage / entry RVA via __ImageBase) -- the loader has no dynamic state
 * to hand us.  Building the block here lets us use NT's real structures
 * (LOADER_PARAMETER_BLOCK, LDR_DATA_TABLE_ENTRY, MEMORY_ALLOCATION_DESCRIPTOR,
 * the ARC CONFIGURATION_COMPONENT_DATA tree) instead of the layout-mirrored
 * copies the loader used to carry.
 *
 * The block + its descriptors / ARC nodes / strings live in the fixed
 * LoaderBlock/Aux pages (PA 0x40000-0x42000, KSEG0-aliased), marked
 * LoaderOsloaderHeap so MmFreeLoaderBlock reclaims them after Phase 1 -- same
 * placement and lifetime the loader used.
 */

#include <ntoskrnl.h>
#include "nxldr-entry.h"
#include "../../boot/nxldr/boot-layout.h"

#define LB_VA  ((PUCHAR)NXBL_PA_TO_KSEG0(NXBL_LB_PA))
#define AUX_VA ((PUCHAR)NXBL_PA_TO_KSEG0(NXBL_AUX_PA))

extern UCHAR __ImageBase[];

/*
 * CPU descriptor bring-up -----------------------------------------------------
 *
 * boot/nxldr leaves us in flat 32-bit protected mode on its own bootstrap GDT
 * (paging on).  Before KiSystemStartup's body runs we populate the kernel's
 * own resident GDT (KiGdt) with the flat R0/R3 code+data selectors plus an
 * R0_PCR selector based at KIP0PCRADDRESS (the PCR page boot/nxldr mapped +
 * zeroed), and lgdt it -- so the early Ke386SetFs(KGDT_R0_PCR) resolves and
 * KiGetMachineBootPointers discovers KiGdt directly (no copy step).  The
 * kernel owns the IDT (KiIdt) and TSS (KiInitialTss) itself -- it lidt's /
 * ltr's them in its own init -- so we don't build either here.
 */

/* Selector values match NT's KGDT_* (i386/ke.h). */
#define SEL_R0_CODE 0x08
#define SEL_R0_DATA 0x10
#define SEL_R3_CODE 0x18
#define SEL_R3_DATA 0x20
#define SEL_R0_PCR  0x30

/* Build an 8-byte i386 segment descriptor at GDT[Selector]. */
static VOID
GdtSet(PUCHAR Gdt, ULONG Selector, ULONG Base, ULONG Limit, UCHAR Access,
       UCHAR Gran)
{
    PUCHAR D = Gdt + Selector;
    D[0] = (UCHAR)(Limit & 0xFF);
    D[1] = (UCHAR)((Limit >> 8) & 0xFF);
    D[2] = (UCHAR)(Base & 0xFF);
    D[3] = (UCHAR)((Base >> 8) & 0xFF);
    D[4] = (UCHAR)((Base >> 16) & 0xFF);
    D[5] = Access;
    D[6] = (UCHAR)((Gran & 0xF0) | ((Limit >> 16) & 0x0F));
    D[7] = (UCHAR)((Base >> 24) & 0xFF);
}

VOID
NxldrInitDescriptors(VOID)
{
    PUCHAR Gdt = (PUCHAR)KiGdt; /* zero-initialised KGDTENTRY KiGdt[16] */

    /* Flat ring-0 / ring-3 code+data (G=1, D=1, 4 GiB). */
    GdtSet(Gdt, SEL_R0_CODE, 0, 0xFFFFF, 0x9A, 0xCF);
    GdtSet(Gdt, SEL_R0_DATA, 0, 0xFFFFF, 0x92, 0xCF);
    GdtSet(Gdt, SEL_R3_CODE, 0, 0xFFFFF, 0xFA, 0xCF);
    GdtSet(Gdt, SEL_R3_DATA, 0, 0xFFFFF, 0xF2, 0xCF);
    /* R0_PCR: base = KIP0PCRADDRESS, limit = 1 page. */
    GdtSet(Gdt, SEL_R0_PCR, KIP0PCRADDRESS, PAGE_SIZE - 1, 0x92, 0xCF);
    /* TSS / per-thread TEB / LDT slots stay zero -- Ki386InitializeTss and the
     * scheduler fill them in the kernel's own init. */

    KiGdtDescriptor.Limit = (USHORT)(sizeof(KGDTENTRY) * 16 - 1); /* Base = KiGdt */
}

/* lgdt KiGdt and reload the segment registers onto it: CS=R0_CODE,
 * SS/DS/ES=R0_DATA, FS=R0_PCR, GS=0.  One asm block so no segment-dependent C
 * access happens between the lgdt and the reloads.  The kernel lidt's KiIdt /
 * ltr's KiInitialTss later; interrupts are masked until then. */
VOID
NxldrLoadDescriptors(VOID)
{
    __asm__ __volatile__(
        "lgdt %0\n\t"
        "ljmp $0x08, $1f\n\t"   /* reload CS = R0_CODE */
        "1:\n\t"
        "movw $0x10, %%ax\n\t"  /* SS/DS/ES = R0_DATA */
        "movw %%ax, %%ss\n\t"
        "movw %%ax, %%ds\n\t"
        "movw %%ax, %%es\n\t"
        "movw $0x30, %%ax\n\t"  /* FS = R0_PCR */
        "movw %%ax, %%fs\n\t"
        "xorw %%ax, %%ax\n\t"   /* GS = 0 */
        "movw %%ax, %%gs\n\t"
        :
        : "m"(KiGdtDescriptor.Limit)
        : "ax", "memory");
}

/* Bump-allocate LEN bytes from the aux cursor, copy SRC in, return the
 * (writable) copy -- for the LDR/ARC strings the kernel stores as pointers. */
static PVOID
AuxDup(PUCHAR *Cursor, const VOID *Src, ULONG Len)
{
    PVOID Dst = *Cursor;
    *Cursor += Len;
    RtlCopyMemory(Dst, Src, Len);
    return Dst;
}

/* Carve a memory descriptor from the aux cursor, fill it, link it onto the
 * block's descriptor list. */
static VOID
AddMd(PLOADER_PARAMETER_BLOCK Lb, PUCHAR *Cursor, TYPE_OF_MEMORY Type,
      PFN_NUMBER Base, PFN_NUMBER Count)
{
    PMEMORY_ALLOCATION_DESCRIPTOR Md = (PMEMORY_ALLOCATION_DESCRIPTOR)*Cursor;
    *Cursor += sizeof(*Md);
    Md->MemoryType = Type;
    Md->BasePage = Base;
    Md->PageCount = Count;
    InsertTailList(&Lb->MemoryDescriptorListHead, &Md->ListEntry);
}

/* Carve an ARC component node under PARENT, fill the common fields, return it
 * so the caller can chain siblings. */
static PCONFIGURATION_COMPONENT_DATA
AddArcChild(PUCHAR *Cursor, PCONFIGURATION_COMPONENT_DATA Parent,
            CONFIGURATION_CLASS Class, CONFIGURATION_TYPE Type,
            PCHAR Id, ULONG IdLen, PVOID Cfg, ULONG CfgLen)
{
    PCONFIGURATION_COMPONENT_DATA C = (PCONFIGURATION_COMPONENT_DATA)*Cursor;
    *Cursor += sizeof(*C);
    RtlZeroMemory(C, sizeof(*C));
    C->Parent = Parent;
    C->ComponentEntry.Class = Class;
    C->ComponentEntry.Type = Type;
    C->ComponentEntry.AffinityMask = 0xFFFFFFFF;
    C->ComponentEntry.IdentifierLength = IdLen;
    C->ComponentEntry.Identifier = Id;
    C->ComponentEntry.ConfigurationDataLength = CfgLen;
    C->ConfigurationData = Cfg;
    return C;
}

PLOADER_PARAMETER_BLOCK
NxldrBuildLoaderBlock(VOID)
{
    PLOADER_PARAMETER_BLOCK Lb = (PLOADER_PARAMETER_BLOCK)LB_VA;
    PUCHAR Cursor = AUX_VA;
    PLOADER_PARAMETER_EXTENSION Ext;
    PLDR_DATA_TABLE_ENTRY Ldr;
    PIMAGE_DOS_HEADER Dos;
    PIMAGE_NT_HEADERS Nt;
    ULONG KernelSize, KernelPages, KernelEnd, HighBase, HighCount;

    /* Kernel size / entry come from our own PE header. */
    Dos = (PIMAGE_DOS_HEADER)__ImageBase;
    Nt = (PIMAGE_NT_HEADERS)(__ImageBase + Dos->e_lfanew);
    KernelSize = Nt->OptionalHeader.SizeOfImage;

    RtlZeroMemory(Lb, PAGE_SIZE);
    RtlZeroMemory(Cursor, PAGE_SIZE);

    Ext = (PLOADER_PARAMETER_EXTENSION)Cursor;
    Cursor += sizeof(*Ext);
    Ldr = (PLDR_DATA_TABLE_ENTRY)Cursor;
    Cursor += sizeof(*Ldr);

    InitializeListHead(&Lb->LoadOrderListHead);
    InitializeListHead(&Lb->MemoryDescriptorListHead);
    InitializeListHead(&Lb->BootDriverListHead);

    /* Strings.  ldr names are wide; ARC names + load options are ASCII.
     * MININT/INRAM puts the kernel in RAM-resident mode (no boot-volume
     * mount); the ARC paths are synthetic but must be non-NULL or
     * CmpSetSystemValues bugchecks. */
    {
        static const WCHAR wfull[] = L"\\SystemRoot\\system32\\ntoskrnl.exe";
        static const WCHAR wbase[] = L"xboxkrnl.exe";
        static const CHAR load_opts[] =
            "/MININT /INRAM /DEBUG /DEBUGPORT=COM1 /BAUDRATE=115200";
        static const CHAR arc_boot[] = "multi(0)disk(0)rdisk(0)partition(1)";
        static const CHAR arc_hal[] = "multi(0)disk(0)rdisk(0)partition(1)";
        static const CHAR nt_boot[] = "\\nxkrnl\\";
        static const CHAR nt_hal[] = "\\";
        static const CHAR arc_root_id[] = "Original Xbox";
        static const CHAR pci_bus_id[] = "PCI";
        static const CHAR isa_bus_id[] = "ISA";
        static const CHAR pci_bios_id[] = "PCI BIOS";

        ULONG arc_id_sz = sizeof(arc_root_id);
        ULONG pci_id_sz = sizeof(pci_bus_id);
        ULONG isa_id_sz = sizeof(isa_bus_id);
        ULONG pci_bios_id_sz = sizeof(pci_bios_id);

        PCHAR arc_id_buf = AuxDup(&Cursor, arc_root_id, arc_id_sz);
        PCHAR pci_id_buf = AuxDup(&Cursor, pci_bus_id, pci_id_sz);
        PCHAR isa_id_buf = AuxDup(&Cursor, isa_bus_id, isa_id_sz);
        PCHAR pci_bios_id_buf = AuxDup(&Cursor, pci_bios_id, pci_bios_id_sz);
        PWCHAR full_buf = AuxDup(&Cursor, wfull, sizeof(wfull));
        PWCHAR base_buf = AuxDup(&Cursor, wbase, sizeof(wbase));

        RtlInitUnicodeString(&Ldr->FullDllName, full_buf);
        RtlInitUnicodeString(&Ldr->BaseDllName, base_buf);
        Lb->LoadOptions = AuxDup(&Cursor, load_opts, sizeof(load_opts));
        Lb->ArcBootDeviceName = AuxDup(&Cursor, arc_boot, sizeof(arc_boot));
        Lb->ArcHalDeviceName = AuxDup(&Cursor, arc_hal, sizeof(arc_hal));
        Lb->NtBootPathName = AuxDup(&Cursor, nt_boot, sizeof(nt_boot));
        Lb->NtHalPathName = AuxDup(&Cursor, nt_hal, sizeof(nt_hal));

        /* Minimal ARC hardware tree: a SystemClass root plus a PCI BIOS
         * marker, PCI bus 0 (carrying PCI_REGISTRY_INFO), PCI bus 1 (the
         * bridge to NV2A), and ISA.  The per-bus "PCI" MultifunctionAdapter
         * entries are what make the PnP enumerator start the PCI driver --
         * without them the IDE controller never attaches and the CDROM never
         * mounts (the XBE loader then gets c000003a on default.xbe). */
        {
            enum { ADAPTER = AdapterClass, MULTIFN = MultiFunctionAdapter };
            PCONFIGURATION_COMPONENT_DATA Root, PciBios, Pci0, Pci1, Isa;
            PUCHAR Cfg0, CfgEmpty, CfgBios;

            Root = (PCONFIGURATION_COMPONENT_DATA)Cursor;
            Cursor += sizeof(*Root);
            RtlZeroMemory(Root, sizeof(*Root));
            Root->ComponentEntry.Class = SystemClass;
            Root->ComponentEntry.Type = ArcSystem;
            Root->ComponentEntry.AffinityMask = 0xFFFFFFFF;
            Root->ComponentEntry.IdentifierLength = arc_id_sz;
            Root->ComponentEntry.Identifier = arc_id_buf;
            Lb->ConfigurationRoot = Root;

            /* PCI bus 0 ConfigurationData (28 bytes, pshpack4):
             *   CM_PARTIAL_RESOURCE_LIST { Version=1, Revision=1, Count=1 }
             *   + CM_PARTIAL_RESOURCE_DESCRIPTOR (Type=5 DeviceSpecific) with
             *     DeviceSpecificData { DataSize=4 }
             *   + PCI_REGISTRY_INFO { Major=1, Minor=0, NoBuses=2, HwMech=1 }
             * NoBuses=2 = PCI bus 0 + the bridge to NV2A (bus 1);
             * HardwareMechanism=1 = type-1 config access via 0xCF8. */
            Cfg0 = Cursor;
            Cursor += 28;
            RtlZeroMemory(Cfg0, 28);
            *(USHORT *)(Cfg0 + 0) = 1;   /* Version */
            *(USHORT *)(Cfg0 + 2) = 1;   /* Revision */
            *(ULONG *)(Cfg0 + 4) = 1;    /* Count */
            Cfg0[8] = 5;                 /* CmResourceTypeDeviceSpecific */
            *(ULONG *)(Cfg0 + 12) = 4;   /* DeviceSpecificData.DataSize */
            Cfg0[24] = 1;                /* PCI_REGISTRY_INFO.MajorRevision */
            Cfg0[26] = 2;                /* NoBuses */
            Cfg0[27] = 1;                /* HardwareMechanism */

            /* Empty resource list (Version=1, Revision=1, Count=0) shared by
             * the bus-1 and ISA markers; the PCI BIOS marker takes an
             * all-zero one. */
            CfgEmpty = Cursor;
            Cursor += 8;
            RtlZeroMemory(CfgEmpty, 8);
            *(USHORT *)(CfgEmpty + 0) = 1;
            *(USHORT *)(CfgEmpty + 2) = 1;
            CfgBios = Cursor;
            Cursor += 8;
            RtlZeroMemory(CfgBios, 8);

            /* Siblings under the root, in freeldr's order:
             *   PCI BIOS -> PCI bus 0 -> PCI bus 1 -> ISA. */
            PciBios = AddArcChild(&Cursor, Root, ADAPTER, MULTIFN,
                                  pci_bios_id_buf, pci_bios_id_sz, CfgBios, 8);
            Pci0 = AddArcChild(&Cursor, Root, ADAPTER, MULTIFN,
                               pci_id_buf, pci_id_sz, Cfg0, 28);
            Pci1 = AddArcChild(&Cursor, Root, ADAPTER, MULTIFN,
                               pci_id_buf, pci_id_sz, CfgEmpty, 8);
            Isa = AddArcChild(&Cursor, Root, ADAPTER, MULTIFN,
                              isa_id_buf, isa_id_sz, CfgEmpty, 8);

            Root->Child = PciBios;
            PciBios->Sibling = Pci0;
            Pci0->Sibling = Pci1;
            Pci1->Sibling = Isa;
        }

        /* Empty ARC_DISK_INFORMATION -- IopCreateArcNames dereferences it
         * unconditionally; an empty list head is enough (no real ARC disks). */
        {
            PLIST_ENTRY Adi = (PLIST_ENTRY)Cursor;
            Cursor += sizeof(*Adi);
            InitializeListHead(Adi);
            Lb->ArcDiskInformation = (PARC_DISK_INFORMATION)Adi;
        }
    }

    /* The kernel's own LDR entry. */
    Ldr->DllBase = __ImageBase;
    Ldr->EntryPoint = __ImageBase + Nt->OptionalHeader.AddressOfEntryPoint;
    Ldr->SizeOfImage = KernelSize;
    Ldr->LoadCount = 1;
    InsertTailList(&Lb->LoadOrderListHead, &Ldr->InLoadOrderLinks);

    /* Memory descriptors (devkit layout, 128 MB).  Keep the low-PA arena
     * mostly LoaderFree for the title heap; pin the live structures (PCR
     * backing, page tables) as LoaderMemoryData.  See boot-layout.h. */
    KernelPages = (KernelSize + PAGE_SIZE - 1) >> PAGE_SHIFT;
    KernelEnd = 0x4000 + KernelPages;
    HighBase = NXBL_PT_LOW_PA >> PAGE_SHIFT;
    HighCount = 4 + NXBL_PT_KSEG0_COUNT + 1;

    AddMd(Lb, &Cursor, LoaderMemoryData, 0x000, 0x001);          /* IVT/BDA: page 0 reserved */
    AddMd(Lb, &Cursor, LoaderFree, 0x001, 0x00E - 0x001);
    AddMd(Lb, &Cursor, LoaderMemoryData, 0x00E, 0x011 - 0x00E);  /* persist + PD + hdr */
    AddMd(Lb, &Cursor, LoaderFree, 0x011, 0x030 - 0x011);        /* freed PT slots + GDT/IDT/TSS */
    AddMd(Lb, &Cursor, LoaderMemoryData, NXBL_PCR_REGION_PA >> PAGE_SHIFT, 16); /* PCR backing */
    AddMd(Lb, &Cursor, LoaderOsloaderHeap, 0x040, 2);            /* LoaderBlock + Aux */
    AddMd(Lb, &Cursor, LoaderFree, 0x042, 0x100 - 0x042);
    AddMd(Lb, &Cursor, LoaderFirmwareTemporary, 0x100, 0x100);   /* loader code+stack */
    AddMd(Lb, &Cursor, LoaderFree, 0x200, 0x3C00 - 0x200);       /* title arena */
    AddMd(Lb, &Cursor, LoaderFirmwarePermanent, 0x3C00, 0x400);  /* FB + NV2A reserve */
    AddMd(Lb, &Cursor, LoaderSystemCode, 0x4000, KernelPages);
    AddMd(Lb, &Cursor, LoaderFree, KernelEnd, HighBase - KernelEnd);
    AddMd(Lb, &Cursor, LoaderMemoryData, HighBase, HighCount);   /* page tables */
    AddMd(Lb, &Cursor, LoaderFree, HighBase + HighCount, 0x8000 - (HighBase + HighCount));

    /* Extension.  Mm reads LoaderPagesSpanned to floor MmSystemPteSpaceStart
     * above everything we mapped at KSEG0 (16 MB KSEG0 + the kernel image
     * PT); see boot-layout.h NXBL_PAGES_SPANNED. */
    Ext->Size = sizeof(*Ext);
    Ext->MajorVersion = 5;
    Ext->MinorVersion = 2;
    Ext->LoaderPagesSpanned = NXBL_PAGES_SPANNED;
    Lb->Extension = Ext;

    /* I386 union: MACHINE_TYPE_ISA = 0. */
    Lb->u.I386.CommonDataArea = NULL;
    Lb->u.I386.MachineType = 0;
    Lb->u.I386.VirtualBias = 0;

    /* (No FirmwareInformation: the kernel compiles below NTDDI_LONGHORN, so
     * its LOADER_PARAMETER_BLOCK has no such field -- the old loader wrote a
     * phantom trailing field the kernel never read.) */

    return Lb;
}
