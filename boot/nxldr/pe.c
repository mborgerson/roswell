/*
 * PE32 image loader for xboxkrnl.  Walks the PE header of the decompressed
 * kernel image and copies its sections to PA = ImageBase + RVA - 0x80000000
 * (the KSEG0 identity).  The kernel is a single contiguous image, so the
 * sections pack from the base with no RVA holes.  Field offsets match
 * Microsoft's "PE Format" spec; we avoid pulling in Windows headers to keep
 * the loader self-contained.
 */

#include "pe.h"
#include "boot-layout.h"
#include "nxldr.h"

#define PE_DOS_MAGIC 0x5A4D        /* "MZ" */
#define PE_NT_SIGNATURE 0x00004550 /* "PE\0\0" */
#define PE_OPT_MAGIC_PE32 0x010b

#define DOS_E_LFANEW_OFF 0x3c

/* File-header field offsets (from start of PE signature). */
#define FH_NUM_SECTIONS_OFF (4 + 2)
#define FH_SIZE_OPT_HEADER_OFF (4 + 16)
#define FH_SIZE (4 + 20) /* signature + IMAGE_FILE_HEADER */

/* Optional-header field offsets (from start of optional header). */
#define OH_MAGIC_OFF 0x00
#define OH_ADDR_OF_ENTRY_OFF 0x10
#define OH_IMAGE_BASE_OFF 0x1c
#define OH_SIZE_OF_HEADERS_OFF 0x3c
#define OH_SIZE_OF_IMAGE_OFF 0x38

/* IMAGE_SECTION_HEADER offsets, 40 bytes per entry. */
#define SH_VIRTUAL_SIZE_OFF 0x08
#define SH_VIRTUAL_ADDR_OFF 0x0c
#define SH_SIZE_OF_RAW_DATA_OFF 0x10
#define SH_PTR_TO_RAW_DATA_OFF 0x14
#define SH_SIZE 40

static USHORT
Read16(const UCHAR *p)
{
    return (USHORT)p[0] | ((USHORT)p[1] << 8);
}

static ULONG
Read32(const UCHAR *p)
{
    return (ULONG)p[0] | ((ULONG)p[1] << 8) | ((ULONG)p[2] << 16) | ((ULONG)p[3] << 24);
}

#define PAGE_MASK_4K 0xfffUL
#define ROUND_PAGE_4K(x) (((x) + PAGE_MASK_4K) & ~PAGE_MASK_4K)

ULONG
PeLoad(const void *Image)
{
    const UCHAR *Bytes = (const UCHAR *)Image;
    ULONG ELfanew, Ph, Oh, Sh, ImageBase, EntryRva, SizeOfImage;
    ULONG PaBase;
    USHORT NumSections, SizeOpt;
    int i;

    /* Trusted image (XZ-crc32-verified): validate via the PE magic/signature
     * chain and bound the section copies against the image's own SizeOfImage
     * -- no externally-supplied size needed. */
    if (Read16(Bytes) != PE_DOS_MAGIC)
    {
        LoaderFailCode = 2;
        return 0;
    }

    ELfanew = Read32(Bytes + DOS_E_LFANEW_OFF);
    Ph = ELfanew;
    if (Read32(Bytes + Ph) != PE_NT_SIGNATURE)
    {
        LoaderFailCode = 4;
        return 0;
    }

    NumSections = Read16(Bytes + Ph + FH_NUM_SECTIONS_OFF);
    SizeOpt = Read16(Bytes + Ph + FH_SIZE_OPT_HEADER_OFF);
    Oh = Ph + FH_SIZE;
    if (Read16(Bytes + Oh + OH_MAGIC_OFF) != PE_OPT_MAGIC_PE32)
    {
        LoaderFailCode = 6;
        return 0;
    }

    ImageBase = Read32(Bytes + Oh + OH_IMAGE_BASE_OFF);
    EntryRva = Read32(Bytes + Oh + OH_ADDR_OF_ENTRY_OFF);
    SizeOfImage = Read32(Bytes + Oh + OH_SIZE_OF_IMAGE_OFF);

    if (ImageBase != NXBL_IMAGE_BASE)
    {
        /* Any other base would need relocations; refuse rather than silently
         * corrupt. */
        LoaderFailCode = 7;
        return 0;
    }
    PaBase = ImageBase - NXBL_KSEG0_BASE;

    Sh = Oh + SizeOpt;
    if (Sh + (ULONG)NumSections * SH_SIZE > SizeOfImage)
    {
        LoaderFailCode = 8;
        return 0;
    }

    LoaderPhase = 0x31; /* PeLoad: about to place sections */

    /* Headers (PE-loaded VA: ImageBase + 0 .. ImageBase + SizeOfHeaders).
     * The kernel's MmInit reads its own PE headers via KeLoaderBlock's
     * LDR entry DllBase, so they need to be present at the imagebase. */
    {
        ULONG SizeOfHeaders = Read32(Bytes + Oh + OH_SIZE_OF_HEADERS_OFF);
        if (SizeOfHeaders > SizeOfImage)
        {
            LoaderFailCode = 9;
            return 0;
        }
        memset((void *)PaBase, 0, ROUND_PAGE_4K(SizeOfHeaders));
        memcpy((void *)PaBase, Bytes, SizeOfHeaders);
    }
    LoaderPhase = 0x32; /* headers placed */

    /* Walk section headers; zero each section's page-rounded span (so
     * .bss tails and alignment padding read zero), then copy the file
     * bytes to (PaBase + VirtualAddress).  .bss-style sections
     * (raw_off == 0 || raw_size == 0) are zero-only. */
    for (i = 0; i < NumSections; i++)
    {
        const UCHAR *s = Bytes + Sh + i * SH_SIZE;
        ULONG VAddr = Read32(s + SH_VIRTUAL_ADDR_OFF);
        ULONG VSize = Read32(s + SH_VIRTUAL_SIZE_OFF);
        ULONG RawSize = Read32(s + SH_SIZE_OF_RAW_DATA_OFF);
        ULONG RawOff = Read32(s + SH_PTR_TO_RAW_DATA_OFF);
        ULONG CopySize;

        if (VAddr + VSize > SizeOfImage)
        {
            LoaderFailCode = 10;
            return 0;
        }

        memset((UCHAR *)PaBase + VAddr, 0, ROUND_PAGE_4K(VSize));

        if (RawSize == 0 || RawOff == 0)
            continue;
        if (RawOff + RawSize > SizeOfImage)
        {
            LoaderFailCode = 11;
            return 0;
        }

        CopySize = RawSize < VSize ? RawSize : VSize;
        memcpy((UCHAR *)PaBase + VAddr, Bytes + RawOff, CopySize);
    }

    LoaderKernelSize = SizeOfImage;
    return ImageBase + EntryRva;
}
