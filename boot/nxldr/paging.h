/*
 * Page-table layout and paging setup for the loader.  See paging.c for the
 * full PA layout commentary.  The fixed Xbox physical placement of everything
 * the loader and kernel share lives in boot-layout.h (NXBL_*).
 */

#ifndef NXLDR_PAGING_H
#define NXLDR_PAGING_H

#include "nxldr.h"

/* Page directory pinned at the retail Xbox CR3. */
#define PD_PA 0x0000F000UL

/* Page tables, PCR backing, and the loader block all come from boot-layout.h
 * (NXBL_*).  GDT/IDT/TSS are the kernel's own resident tables, not low-PA. */

/* KSEG0: NT's high-half identity for low PA.  PA P maps to VA (KSEG0_BASE|P)
 * AND the kernel preserves that mapping permanently.  Our transient identity
 * map gets nuked in MmArmInitSystem (mm/ARM3/i386/init.c:283 RtlZeroMemory of
 * user-mode PDEs), so the KSEG0 alias is the only mapping of low PA that
 * survives past that point. */
#define KSEG0_BASE 0x80000000UL

/* High-reserve region: VA 0xFFDF0000 .. 0xFFE00000 (16 pages) covers
 * KI_USER_SHARED_DATA + the KPCR/KPRCB pages up to KIP0PCRADDRESS.
 * Backing PA is NXBL_PCR_REGION_PA (boot-layout.h). */
#define PCR_REGION_PAGES 16
#define PCR_REGION_VA 0xffdf0000UL

#define KIP0PCRADDRESS 0xffdff000UL /* matches NT's per-CPU PCR slot */

#define PTE_P 0x1UL
#define PTE_RW 0x2UL
#define PTE_FLAGS (PTE_P | PTE_RW)

#define PAGE_SHIFT 12
#define PAGE_SIZE (1UL << PAGE_SHIFT)

#define PD_INDEX(va) (((va) >> 22) & 0x3ff)
#define PT_INDEX(va) (((va) >> 12) & 0x3ff)

/* Fill PD + PT pages for the identity, KSEG0, and PCR mappings.  Must run
 * before PagingEnable(). */
void
PagingInit(void);

/* Load CR3, set CR4.PSE, flip CR0.PG. */
void
PagingEnable(void);

#endif /* NXLDR_PAGING_H */
