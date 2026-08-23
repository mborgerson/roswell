/*
 * Shared Xbox boot layout -- the fixed physical placement of everything the
 * loader sets up and the kernel's boot setup reads back.  Pure #defines so
 * both the freestanding loader (boot/nxldr) and the kernel-side setup
 * (ntoskrnl/xb/nxldr-entry.c) can include it; the loader has no NT types and
 * the kernel has no loader types, but neither needs any here.
 *
 * There is one layout: a single kernel image at the 64 MiB mark
 * (PA 0x4000000) so the title owns the low 64 MB of 128 MB RAM, the PCR
 * backing + LoaderBlock pages at PA 0x30000-0x42000, and the page-table
 * cluster at the top of RAM.  (GDT/IDT/TSS are the kernel's resident KiGdt/
 * KiIdt/KiInitialTss, not low-PA tables.)
 */

#ifndef NXBL_BOOT_LAYOUT_H
#define NXBL_BOOT_LAYOUT_H

#define NXBL_KSEG0_BASE 0x80000000UL
#define NXBL_PA_TO_KSEG0(pa) ((pa) | NXBL_KSEG0_BASE)

#define NXBL_IMAGE_BASE 0x84000000UL    /* PE ImageBase (PA 0x04000000)   */
#define NXBL_PT_LOW_PA 0x07F00000UL     /* 4 PTs, identity map of low 16 MB */
#define NXBL_PT_KSEG0_PA 0x07F04000UL   /* NXBL_PT_KSEG0_COUNT PTs, KSEG0  */
#define NXBL_PT_KSEG0_COUNT 17          /* PA 0..0x4400000: KSEG0 + image  */
#define NXBL_PT_PCR_PA 0x07F15000UL     /* 1 PT for the PCR window         */
#define NXBL_PCR_REGION_PA 0x00030000UL /* 16 pages backing 0xFFDF0000..   */
#define NXBL_LB_PA 0x00040000UL         /* LoaderBlock page                */
#define NXBL_AUX_PA 0x00041000UL        /* extension + LDR entry + descs   */
#define NXBL_PAGES_SPANNED 0x4400       /* Extension->LoaderPagesSpanned   */

#endif /* NXBL_BOOT_LAYOUT_H */
