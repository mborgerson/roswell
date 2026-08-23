/*
 * Shared definitions for the boot/nxldr loader: integer typedefs, NULL, the
 * DBG flag, the compressed-payload framing, and the GDB-visible sentinel
 * globals.  Included by every TU.
 */

#ifndef NXLDR_H
#define NXLDR_H

typedef unsigned char UCHAR;
typedef unsigned short USHORT;
typedef unsigned int ULONG;
typedef unsigned long long ULONGLONG;
typedef unsigned long ULONG_PTR;
typedef void VOID;
typedef void *PVOID;
typedef UCHAR *PUCHAR;
typedef ULONG *PULONG;

#ifndef NULL
#define NULL ((PVOID)0)
#endif

#ifndef DBG
#define DBG 0
#endif

/* Sentinels the GDB watchpoint / live debug can match against. */
extern volatile ULONG LoaderPhase;
extern volatile ULONG LoaderKernelEntry;
extern volatile ULONG LoaderKernelSize;
extern volatile ULONG LoaderFailCode;

/* libc mem* shims; see mem.c for the rationale. */
void *
memcpy(void *Dest, const void *Src, ULONG Count);
void *
memset(void *Dest, int Value, ULONG Count);
void *
memmove(void *Dest, const void *Src, ULONG Count);
int
memcmp(const void *Buffer1, const void *Buffer2, ULONG Count);

/* Compressed-kernel framing written by gen-flash-bin.py into the payload slot
 * (KernelPayloadVa), an xz container produced with
 * `xz --x86 --lzma2=preset=9e --check=crc32`. */
#define NXKRNL_XZ_MAGIC 0x5a4b584eUL /* 'NXKZ' */
typedef struct _NXKRNL_XZ_HEADER
{
    ULONG Magic;
    ULONG CompressedSize;
    ULONG DecompressedSize;
} NXKRNL_XZ_HEADER, *PNXKRNL_XZ_HEADER;

#endif /* NXLDR_H */
