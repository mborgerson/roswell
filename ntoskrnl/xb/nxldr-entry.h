/*
 * Xbox kernel boot setup (ntoskrnl/xb/nxldr-entry.c), called from
 * KiSystemStartup's SARCH_XBOX prologue.  boot/nxldr hands control to
 * KiSystemStartup passing nothing; these install the NT-shaped CPU descriptors
 * and synthesize the LOADER_PARAMETER_BLOCK before the generic path reads them.
 */

#ifndef NXLDR_ENTRY_H
#define NXLDR_ENTRY_H

/* Build the NT-shaped GDT/IDT/TSS/PCR descriptors at their fixed PAs. */
VOID NxldrInitDescriptors(VOID);

/* lgdt/lidt those tables and reload CS/SS/DS/ES/FS/GS + TR onto them. */
VOID NxldrLoadDescriptors(VOID);

/* Synthesize the LOADER_PARAMETER_BLOCK in the fixed LoaderBlock/Aux pages. */
PLOADER_PARAMETER_BLOCK NxldrBuildLoaderBlock(VOID);

#endif /* NXLDR_ENTRY_H */
