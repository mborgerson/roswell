/*
 * boot/nxldr -- C entry point.  Decompresses the XZ-packed xboxkrnl payload
 * from flash, PE-maps it to its imagebase, sets up paging, and jumps to
 * KiSystemStartup.  The kernel's SARCH_XBOX prologue installs the CPU
 * descriptors and builds the LOADER_PARAMETER_BLOCK itself
 * (ntoskrnl/xb/nxldr-entry.c) -- we pass nothing.
 *
 * Built freestanding (no libc); the loader is split across:
 *   nxldr.h / log.h  - shared types + DBG-gated logger
 *   mem.c            - libc mem* shims
 *   serial.c         - COM1 dprintf (DBG only)
 *   pe.c             - PE32 image loader
 *   paging.c         - page-table setup + PagingEnable
 *   loader.c         - LoaderMain entry + XZ decompression
 *   startup.S        - reset path + .low_rom prologue
 */

#include "boot-layout.h"
#include "log.h"
#include "nxldr.h"
#include "paging.h"
#include "pe.h"
#include "xz/xz.h"

/* GDB-visible sentinels. */
volatile ULONG LoaderPhase = 0;
volatile ULONG LoaderKernelEntry = 0; /* set after PeLoad */
volatile ULONG LoaderKernelSize = 0;  /* set after PeLoad */
volatile ULONG LoaderFailCode = 0;    /* set on error */

/*
 * The XZ decoder wants a one-shot allocator for its ~32 KB of state.  The arena
 * lives in .bss (RAM only); nothing is ever freed -- one decompression per boot,
 * then the loader hands off to the kernel.  Hooked via xz_config.h's
 * kmalloc/vmalloc macros.
 */
#define XZ_ARENA_SIZE (48 * 1024)
static UCHAR XzArena[XZ_ARENA_SIZE];
static ULONG XzArenaUsed;

void *
xz_arena_alloc(unsigned long Size)
{
    UCHAR *p;

    Size = (Size + 7) & ~7UL;
    if (Size > XZ_ARENA_SIZE - XzArenaUsed)
        return NULL;
    p = XzArena + XzArenaUsed;
    XzArenaUsed += Size;
    return p;
}

/* Flash-MMIO address of the XZ-compressed kernel payload; placed + exported by
 * nxldr.ld (it follows the loader's .data source). */
extern const UCHAR KernelPayloadVa[];

/*
 * Decompress the kernel payload from flash to a scratch RAM area and return its
 * address, or NULL (with LoaderFailCode set) on any failure.  The payload is
 * always XZ-compressed -- a header magic mismatch is a hard error.
 */
static const void *
DecompressKernel(void)
{
    const NXKRNL_XZ_HEADER *Header = (const NXKRNL_XZ_HEADER *)KernelPayloadVa;
    /* Scratch buffer at PA 0x02000000 (32 MiB) -- clear of the kernel PA and
     * above nxldr's own footprint (nxldr is at PA 0x00100000, stack tops at
     * 0x001ffff0). */
    UCHAR *Scratch = (UCHAR *)0x02000000;
    struct xz_dec *Xz;
    struct xz_buf Buf;
    enum xz_ret Ret;

    if (Header->Magic != NXKRNL_XZ_MAGIC)
    {
        LoaderFailCode = 0x585a; /* 'XZ' */
        dprintf("bad payload magic %#x\n", Header->Magic);
        return NULL;
    }

    LoaderPhase = 0x22;
    dprintf("xz_dec_run csize=%u dsize=%u...\n", Header->CompressedSize, Header->DecompressedSize);
    xz_crc32_init();
    Xz = xz_dec_init(XZ_SINGLE, 0);
    if (Xz == NULL)
    {
        LoaderFailCode = 0x585a; /* 'XZ' */
        dprintf("xz_dec_init failed (arena)\n");
        return NULL;
    }

    Buf.in = (const UCHAR *)(Header + 1);
    Buf.in_pos = 0;
    Buf.in_size = Header->CompressedSize;
    Buf.out = Scratch;
    Buf.out_pos = 0;
    Buf.out_size = Header->DecompressedSize;
    Ret = xz_dec_run(Xz, &Buf);
    if (Ret != XZ_STREAM_END || Buf.out_pos != Header->DecompressedSize)
    {
        LoaderFailCode = 0x585a; /* 'XZ' */
        dprintf("xz decode failed ret=%d out=%u\n", (int)Ret, (unsigned)Buf.out_pos);
        return NULL;
    }
    return Scratch;
}

__attribute__((noreturn)) void
LoaderMain(void)
{
    const void *KernelImage;
    ULONG EntryVa;

    DBG_SERIAL_INIT();
    dprintf("\nLoaderMain entered\n");

    LoaderPhase = 0x21; /* entered LoaderMain */

    KernelImage = DecompressKernel();
    if (KernelImage == NULL)
        goto halt;

    LoaderPhase = 0x23; /* about to call PeLoad */
    dprintf("PeLoad...\n");
    EntryVa = PeLoad(KernelImage);

    LoaderKernelEntry = EntryVa;
    dprintf("kernel_entry=%#x size=%#x fail=%u\n", EntryVa, LoaderKernelSize, LoaderFailCode);

    if (LoaderFailCode != 0)
        goto halt;

    LoaderPhase = 0x24;
    dprintf("PagingInit...\n");
    PagingInit();

    LoaderPhase = 0x25;
    dprintf("PagingEnable...\n");
    PagingEnable();

    LoaderPhase = 0x26;
    dprintf("paging on; reading via VA\n");

    /* Read the kernel via its VA (must come through paging now). */
    dprintf(
        "VA base=%#x entry=%#x pcr=%#x kuser=%#x\n", *(volatile ULONG *)NXBL_IMAGE_BASE,
        *(volatile ULONG *)EntryVa, *(volatile ULONG *)KIP0PCRADDRESS, *(volatile ULONG *)PCR_REGION_VA);

    /* Hand off.  We run on our flat bootstrap GDT with paging on; the kernel
     * does the rest.  KiSystemStartup's SARCH_XBOX prologue installs the
     * NT-shaped GDT/IDT/TSS/PCR and synthesizes the LOADER_PARAMETER_BLOCK
     * itself (ntoskrnl/xb/nxldr-entry.c) -- the loader passes nothing.
     * KiSystemStartup is noreturn, so 'call' (a dead return address) is fine;
     * it reads its LoaderBlock arg as garbage and overwrites it before use. */
    LoaderPhase = 0x2d;
    dprintf("jumping to KiSystemStartup\n");
    DBG_SERIAL_DRAIN();
    __asm__ __volatile__("call *%0\n\t" ::"r"((void *)EntryVa) : "memory");
    /* Should never return. */

halt:;
    for (;;)
    {
        __asm__ __volatile__("cli; hlt");
    }
}
