/*
 * Minimal PE32 image loader -- just enough to copy xboxkrnl's sections from
 * the decompressed image into RAM at its preferred ImageBase.
 */

#ifndef NXLDR_PE_H
#define NXLDR_PE_H

#include "nxldr.h"

/* PE-load `Image` into RAM at PA = ImageBase + RVA - 0x80000000, section by
 * section.  Returns the entry-point VA (ImageBase + AddressOfEntryPoint) on
 * success, 0 on failure (and sets LoaderFailCode).  The size is taken from the
 * image's own PE header (SizeOfImage), which bounds the section copies.
 *
 * Caller is responsible for ensuring the destination PAs are free -- we do not
 * page-fault check because paging is still off when this runs. */
ULONG
PeLoad(const void *Image);

#endif /* NXLDR_PE_H */
