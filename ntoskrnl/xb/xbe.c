/*
 * PROJECT:     nxkrnl -- a free kernel for the original Xbox
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     XBE loader.
 *
 * Parse XBEH, map the sections at the image base, de-XOR the entry point and
 * the kernel thunk table, resolve thunks against the kernel ordinal export
 * directory, set up TLS, call the entry point.  Called from Phase 1 init in
 * place of launching smss.
 *
 * Clean-room from the public XBE format (xboxdevwiki, nxdk's cxbe) and the
 * public xboxkrnl ordinal list (nxdk).  The Xbox kernel was forked from
 * Windows 2000, so most ordinals are direct NT analogs that the spec forwards
 * straight to a ReactOS symbol; the Xbox-specific ones (Av / Hal / Mm-contig /
 * Phy / Xc and the threading model) are shimmed here or in sibling files.
 */

#include <ntifs.h>
#include <ntimage.h>
#include <intrin.h>
#include <xb-debug.h>
#include <xb-event-cache.h>


/* Need the full KTHREAD layout (XeXboxShadow / XeXboxFs4 sit at the end). */
#include <ndk/ketypes.h>

#include "strace.h"
#include "object-types.h"
#include "obcreate.h"
#include "mm/mm.h"

/* ExMutantObjectType + MUTANT_ALL_ACCESS. */
#include <ndk/extypes.h>

/* OBJECT_TO_OBJECT_HEADER. */
#include <ndk/obtypes.h>

/* HalReturnToFirmware. */
#include <ndk/halfuncs.h>

/* HAL SMBus primitives -- forward-declared (defined in hal/halx86/xbox/smbus.c)
 * to avoid pulling HAL-private halxbox.h into a kernel TU. */
NTSTATUS HalpXboxSmBusReadByte(_In_ UCHAR Address, _In_ UCHAR Register,
                               _Out_ PUCHAR Value);
NTSTATUS HalpXboxSmBusWriteByte(_In_ UCHAR Address, _In_ UCHAR Register,
                                _In_ UCHAR Value);
NTSTATUS HalpXboxSmBusReadWord(_In_ UCHAR Address, _In_ UCHAR Register,
                               _Out_ PUSHORT Value);
NTSTATUS HalpXboxSmBusWriteWord(_In_ UCHAR Address, _In_ UCHAR Register,
                                _In_ USHORT Value);

/* Title-facing VM ordinals (defined below); the loader and the section
 * loader allocate through these so the whole title address space is
 * managed by one VM, whichever backend is configured. */
NTSTATUS NTAPI XeNtAllocateVirtualMemory(PVOID *Base, ULONG_PTR ZeroBits,
                                         PSIZE_T Size, ULONG Type,
                                         ULONG Protect);
NTSTATUS NTAPI XeNtFreeVirtualMemory(PVOID *Base, PSIZE_T Size,
                                     ULONG FreeType);

/* --- XBE format (public; xboxdevwiki / nxdk cxbe) ------------------------- */

#include <pshpack1.h>
typedef struct _XBE_HEADER
{
    ULONG Magic;                    /* 0x000 'XBEH' */
    UCHAR Signature[256];           /* 0x004 */
    ULONG BaseAddr;                 /* 0x104 */
    ULONG SizeOfHeaders;            /* 0x108 */
    ULONG SizeOfImage;              /* 0x10C */
    ULONG SizeOfImageHeader;        /* 0x110 */
    ULONG TimeDate;                 /* 0x114 */
    ULONG CertificateAddr;          /* 0x118 */
    ULONG Sections;                 /* 0x11C */
    ULONG SectionHeadersAddr;       /* 0x120 */
    ULONG InitFlags;                /* 0x124 */
    ULONG EntryAddr;                /* 0x128 -- XOR-encoded */
    ULONG TlsAddr;                  /* 0x12C */
    ULONG PeStackCommit;            /* 0x130 */
    ULONG PeHeapReserve;            /* 0x134 */
    ULONG PeHeapCommit;             /* 0x138 */
    ULONG PeBaseAddr;               /* 0x13C */
    ULONG PeSizeOfImage;            /* 0x140 */
    ULONG PeChecksum;               /* 0x144 */
    ULONG PeTimeDate;               /* 0x148 */
    ULONG DebugPathnameAddr;        /* 0x14C */
    ULONG DebugFilenameAddr;        /* 0x150 */
    ULONG DebugUnicodeFilenameAddr; /* 0x154 */
    ULONG KernelThunkAddr;          /* 0x158 -- XOR-encoded */
    ULONG NonKernelImportDirAddr;   /* 0x15C */
    ULONG LibraryVersions;          /* 0x160 */
    ULONG LibraryVersionsAddr;      /* 0x164 */
    ULONG KernelLibVersionAddr;     /* 0x168 */
    ULONG XapiLibVersionAddr;       /* 0x16C */
    ULONG LogoBitmapAddr;           /* 0x170 */
    ULONG SizeOfLogoBitmap;         /* 0x174 */
} XBE_HEADER, *PXBE_HEADER;

typedef struct _XBE_SECTION
{
    ULONG Flags;
    ULONG VirtualAddr;
    ULONG VirtualSize;
    ULONG RawAddr;
    ULONG SizeOfRaw;
    ULONG SectionNameAddr;
    ULONG SectionRefCount;
    ULONG HeadSharedRefCountAddr;
    ULONG TailSharedRefCountAddr;
    UCHAR Digest[20];
} XBE_SECTION, *PXBE_SECTION;

/* XBE TLS directory -- the layout of IMAGE_TLS_DIRECTORY_32 (nxdk lib/.../tls.c). */
typedef struct _XBE_TLS
{
    ULONG StartAddressOfRawData;
    ULONG EndAddressOfRawData;
    ULONG AddressOfIndex;
    ULONG AddressOfCallbacks;
    ULONG SizeOfZeroFill;
    ULONG Characteristics;
} XBE_TLS, *PXBE_TLS;
#include <poppack.h>

C_ASSERT(sizeof(XBE_HEADER) == 0x178);
C_ASSERT(sizeof(XBE_SECTION) == 0x38);
C_ASSERT(sizeof(XBE_TLS) == 0x18);

#define XBE_MAGIC       'HEBX'      /* 'XBEH' little-endian */

#define XBE_SECTION_FLAG_WRITABLE   0x00000001
#define XBE_SECTION_FLAG_PRELOAD    0x00000002
#define XOR_EP_DEBUG    0x94859D4B
#define XOR_EP_RETAIL   0xA8FC57AB
#define XOR_KT_DEBUG    0xEFB1F152
#define XOR_KT_RETAIL   0x5B6D40B6
#define XBE_THUNK_ORDINAL 0x80000000

#define XBE_TAG        'ebxN'

/* Retail search order: disc first (D:), dashboard on C: as fallback.  Xbox
 * partition naming is Partition1=E, Partition2=C, Partition3=X,
 * Partition4=Y, Partition5=Z. */
static PCWSTR const XeTitleSearchPaths[] = {
    L"\\Device\\CdRom0\\default.xbe",
    L"\\Device\\Harddisk0\\Partition2\\xboxdash.xbe",
};

/* --- Xbox thread + TLS shim ----------------------------------------------- *
 *
 * PsCreateSystemThreadEx -- the Xbox-flavour PsCreateSystemThread -- takes a
 * TLS data size.  The kernel allocates the block and exposes it via
 * KTHREAD.TlsData at offset 0x28 of the Xbox KTHREAD; the title's per-thread
 * startup reads it, and compiler-generated __thread loads read fs:[0x04].
 *
 * NT's KTHREAD has neither field.  XeXboxShadow (inline KTHREAD field, with
 * TlsData at offset 0x28 within it) and XeXboxFs4 carry the Xbox-shaped view.
 * fs:[0x04] is KPCR.NtTib.StackBase, which NT rewrites on every context
 * switch -- KiSwapContextExit calls XeRestoreTlsFsBase after KiSetTebBase to
 * push our value back.  Unregistered threads keep XeXboxFs4 = 0 so the hook
 * is a no-op for them.
 */

#define XBE_KTHREAD_TLSDATA  0x28      /* Xbox KTHREAD.TlsData offset */
#define XBE_TIB_STACKBASE    0x04      /* fs:[] offset for the TLS pointer */

typedef VOID (NTAPI *PKSTART_ROUTINE_X)(PVOID StartContext);

/* Stamp the current thread with TlsData + Fs4 so it appears as an Xbox-shaped
 * KTHREAD.  Returns the shadow address (== TlsData base). */
static PVOID
XeRegisterThread(_In_opt_ PVOID TlsData, _In_ ULONG_PTR Fs4)
{
    PKTHREAD self = KeGetCurrentThread();

    RtlZeroMemory(self->XeXboxShadow, sizeof(self->XeXboxShadow));
    *(PVOID *)(self->XeXboxShadow + XBE_KTHREAD_TLSDATA) = TlsData;
    self->XeXboxFs4 = Fs4;
    /* SEH chain head before any title frame: the int3 dispatch baseline. */
    self->XeBaseSeh = (PVOID)__readfsdword(0);
    return self->XeXboxShadow;
}

/*
 * Push the new thread's Xbox-shaped fs:[0x04] and fs:[0x28] back into the PCR
 * after KiSetTebBase clobbered them.  Hooked from KiSwapContextExit.  Xbox
 * places CurrentThread inline at PCR+0x28 (NT keeps it in PrcbData at
 * PCR+0x124); titles read fs:[0x28] for their KTHREAD shadow and fs:[0x04]
 * for the TLS-pointer table.  HAL's soft-IRR pending masks moved off
 * Pcr->IRR/IrrActive to module-scope globals so this write doesn't fight
 * them.
 */
VOID
XeRestoreTlsFsBase(_In_ PKTHREAD NewThread)
{
    if (NewThread->XeXboxFs4 != 0)
        __writefsdword(XBE_TIB_STACKBASE, (ULONG)NewThread->XeXboxFs4);

    __writefsdword(0x28, (ULONG)(ULONG_PTR)NewThread->XeXboxShadow);

    /* PRCB+0x24C/0x250 are Xbox-side "optional per-thread debug/FP setup"
     * pointers; titles read them and expect NULL on retail.  In our NT
     * layout these offsets land inside KPRCB.ProcessorState (the saved
     * boot FXSAVE image), which kernel activity between thread starts can
     * silently re-populate.  Zero them on every context switch into a
     * title thread; KiEoiHelper also zeroes on every trap return to a
     * title thread, so interrupts during title execution can't leave the
     * slot non-zero. */
    if (NewThread->XeXboxFs4 != 0)
    {
        PULONG prcb = (PULONG)__readfsdword(0x20);
        if (prcb != NULL)
        {
            prcb[0x24C / 4] = 0;
            prcb[0x250 / 4] = 0;
        }
    }
}

PVOID NTAPI
XeKeGetCurrentThread(VOID)
{
    return KeGetCurrentThread()->XeXboxShadow;
}

/*
 * Allocate a per-thread TLS block.  nxdk's WinapiThreadStartup uses block+4
 * as the usable area and asserts ((block+4) & 15) == 0, so place block at
 * (base == 12 mod 16).  ExAllocatePoolWithTag is only 8-byte aligned, so
 * over-allocate by 16 and offset.  The raw pointer is not retained (TLS lives
 * as long as the title).
 */
static PVOID
XeAllocTls(_In_ SIZE_T Size)
{
    ULONG_PTR raw = (ULONG_PTR)ExAllocatePoolWithTag(NonPagedPool, Size + 16,
                                                     XBE_TAG);
    if (raw == 0)
        return NULL;
    return (PVOID)(((raw + 4 + 15) & ~(ULONG_PTR)15) - 4);
}

/* Trampoline for PsCreateSystemThreadEx: set up TLS, then enter the title. */
typedef struct _XBE_THREAD_CTX
{
    PKSYSTEM_ROUTINE  SystemRoutine;
    PKSTART_ROUTINE_X StartRoutine;
    PVOID             StartContext;
    PVOID             TlsData;
    SIZE_T            TlsSize;
} XBE_THREAD_CTX, *PXBE_THREAD_CTX;

static VOID NTAPI
XeThreadTrampoline(_In_ PVOID Context)
{
    XBE_THREAD_CTX ctx = *(PXBE_THREAD_CTX)Context;
    ExFreePoolWithTag(Context, XBE_TAG);

    /* fs:[0x04] must point at the *end* of the TLS block: the title's
     * compiled __thread access is `mov $_tls_index, %eax; mov %fs:0x4, %ecx;
     * mov (%ecx,%eax,4), %eax`, and nxdk's crt0 stamps
     * `_tls_index = -tlssize/4` into *_tls_used.AddressOfIndex
     * (third_party/nxdk/lib/pdclib/platform/xbox/crt0.c).  END + index*4
     * = END - tlssize = the block base, where the self-reference lives.
     *
     * The registered Fs4 is what our KiSwapContextExit hook will reload on
     * every context switch (XeRestoreTlsFsBase). */
    ULONG_PTR fs4 = (ctx.TlsData != NULL)
        ? ((ULONG_PTR)ctx.TlsData + (ULONG_PTR)ctx.TlsSize)
        : 0;
    XeRegisterThread(ctx.TlsData, fs4);
    if (fs4 != 0)
        __writefsdword(XBE_TIB_STACKBASE, (ULONG)fs4);

    /* Titles read fs:[0x28] to find their KTHREAD shadow on every entry point;
     * the KiSwapContextExit hook only fires on context switches *into* this
     * thread, not on the first run.  Stamp the slot here so the title's first
     * `mov eax, fs:[0x28]` after we hand control to it sees a valid pointer. */
    __writefsdword(0x28, (ULONG)(ULONG_PTR)KeGetCurrentThread()->XeXboxShadow);

    /* Titles probe per-processor pointers at fs:[0x20]+N expecting the Xbox
     * KPRCB layout, where they are optional structs that are NULL on retail.
     * Those offsets land inside NT's KPRCB saved boot ProcessorState (FXSAVE
     * context image), so they hold stale non-zero bytes and the title's
     * NULL-check fails:
     *   +0x24C  debug-monitor block (validated by [+4] == 0x58424436)
     *   +0x250  per-thread FP-setup struct */
    {
        PULONG prcb = (PULONG)__readfsdword(0x20);
        if (prcb != NULL)
        {
            prcb[0x24C / 4] = 0;
            prcb[0x250 / 4] = 0;
        }
    }

    XbDbg("title thread up (tls=%p, start=%p, fs:[0x28]=0x%x)\n",
             ctx.TlsData, ctx.StartRoutine, __readfsdword(0x28));
    XbStraceValidatePool("trampoline-pre");

    if (ctx.SystemRoutine != NULL)
        ctx.SystemRoutine((PVOID)ctx.StartRoutine, ctx.StartContext);
    else if (ctx.StartRoutine != NULL)
        ctx.StartRoutine(ctx.StartContext);

    XbDbg("title thread returned\n");
}

/* PsCreateSystemThread with an explicit kernel-stack size (ntoskrnl/ps/thread.c). */
extern NTSTATUS NTAPI NxkPsCreateSystemThread(
    OUT PHANDLE ThreadHandle, IN ACCESS_MASK DesiredAccess,
    IN POBJECT_ATTRIBUTES ObjectAttributes, IN HANDLE ProcessHandle,
    IN PCLIENT_ID ClientId, IN PKSTART_ROUTINE StartRoutine,
    IN PVOID StartContext, IN SIZE_T StackSize, IN BOOLEAN CreateSuspended);

NTSTATUS NTAPI
XePsCreateSystemThreadEx(
    _Out_ PHANDLE ThreadHandle,
    _In_ SIZE_T ThreadExtensionSize,
    _In_ SIZE_T KernelStackSize,
    _In_ SIZE_T TlsDataSize,
    _Out_opt_ PHANDLE ThreadId,
    _In_ PVOID StartRoutine,
    _In_ PVOID StartContext,
    _In_ BOOLEAN CreateSuspended,
    _In_ BOOLEAN DebuggerThread,
    _In_opt_ PVOID SystemRoutine)
{
    PXBE_THREAD_CTX ctx;
    NTSTATUS status;
    CLIENT_ID cid;
    HANDLE handle;

    UNREFERENCED_PARAMETER(ThreadExtensionSize);
    UNREFERENCED_PARAMETER(DebuggerThread);

    ctx = ExAllocatePoolWithTag(NonPagedPool, sizeof(*ctx), XBE_TAG);
    if (ctx == NULL)
        return STATUS_INSUFFICIENT_RESOURCES;

    ctx->SystemRoutine = (PKSYSTEM_ROUTINE)SystemRoutine;
    ctx->StartRoutine = (PKSTART_ROUTINE_X)StartRoutine;
    ctx->StartContext = StartContext;
    ctx->TlsData = NULL;
    ctx->TlsSize = TlsDataSize;
    if (TlsDataSize != 0)
    {
        ctx->TlsData = XeAllocTls(TlsDataSize);
        if (ctx->TlsData != NULL)
            RtlZeroMemory(ctx->TlsData, TlsDataSize);
    }

    /* Honor the caller's stack-size request.  Title main threads ask via the
     * XBE header (PeStackCommit), often 256-512 KB; workers spawned from
     * there pass their own.  The 48 KB ReactOS default overflows on deep
     * title call chains -- bugcheck 0x50 in memcpy below StackLimit. */
    /* CreateSuspended is part of the ordinal contract: XAPI's
     * CreateThread(CREATE_SUSPENDED) parks the thread until
     * NtResumeThread so the creator can set priority/affinity first. */
    status = NxkPsCreateSystemThread(&handle, THREAD_ALL_ACCESS, NULL, NULL,
                                     &cid, XeThreadTrampoline, ctx,
                                     KernelStackSize, CreateSuspended);
    if (!NT_SUCCESS(status))
    {
        ExFreePoolWithTag(ctx, XBE_TAG);   /* TLS block leaks on this path */
        XbDbg("PsCreateSystemThreadEx failed (%08lx)\n", status);
        return status;
    }

    *ThreadHandle = handle;
    if (ThreadId != NULL)
        *ThreadId = cid.UniqueThread;
    return STATUS_SUCCESS;
}

/* Xbox PsCreateSystemThread is the 5-arg variant: no Object
 * attributes, no process handle, no client-id out-param, no ACCESS_MASK.
 * Routes to NT's PsCreateSystemThread with the system process and the
 * default kernel stack -- titles that need a custom stack call the Ex
 * form above. */
NTSTATUS NTAPI
XePsCreateSystemThread(_Out_ PHANDLE ThreadHandle,
                        _Out_opt_ PHANDLE ThreadId,
                        _In_ PKSTART_ROUTINE StartRoutine,
                        _In_ PVOID StartContext,
                        _In_ BOOLEAN DebuggerThread)
{
    NTSTATUS status;
    CLIENT_ID cid;

    UNREFERENCED_PARAMETER(DebuggerThread);

    status = PsCreateSystemThread(ThreadHandle, THREAD_ALL_ACCESS, NULL,
                                  NULL, &cid, StartRoutine, StartContext);
    if (NT_SUCCESS(status) && ThreadId != NULL)
        *ThreadId = cid.UniqueThread;
    return status;
}

/* Xbox FIRMWARE_REENTRY (nxdk hal/halfuncs.h).  Distinct enum from NT's
 * FIRMWARE_REENTRY (ndk/haltypes.h): only value 0 (HalHaltRoutine) is shared;
 * Xbox's HalRebootRoutine=1 collides with NT's HalPowerDownRoutine=1, etc.
 * Named XeFw* to keep the two enums from colliding in the same TU. */
typedef enum _XBE_FIRMWARE_REENTRY
{
    XeFwHalt                = 0,  /* power off                              */
    XeFwReboot              = 1,  /* SMC reset (warm reboot)                */
    XeFwQuickReboot         = 2,  /* SMC reset, skip dashboard animation    */
    XeFwKdReboot            = 3,  /* reboot expecting to enter kd           */
    XeFwFatalErrorReboot    = 4   /* reboot after a fatal title error       */
} XBE_FIRMWARE_REENTRY;

/* The chainload protocol (XLaunchXBEEx in nxdk hal/xbox.c, mirroring retail):
 * the title MmAllocateContiguousMemory's a 4 KB LAUNCH_DATA_PAGE, fills the
 * Header with LDT_TITLE + szLaunchPath, stores the page in LaunchDataPage,
 * MmPersistContiguousMemory's it across reboot, and HalReturnToFirmware's
 * HalQuickRebootRoutine.  On the other side of the "reboot" the kernel reads
 * LaunchDataPage and loads szLaunchPath in place of the dashboard. */
typedef struct _LAUNCH_DATA_HEADER
{
    ULONG dwLaunchDataType;
    ULONG dwTitleId;
    CHAR  szLaunchPath[520];
    ULONG dwFlags;
} LAUNCH_DATA_HEADER, *PLAUNCH_DATA_HEADER;

typedef struct _LAUNCH_DATA_PAGE
{
    LAUNCH_DATA_HEADER Header;
    UCHAR Pad[492];
    UCHAR LaunchData[3072];
} LAUNCH_DATA_PAGE, *PLAUNCH_DATA_PAGE;

#define LDT_TITLE             0
#define LDT_LAUNCH_DASHBOARD  1
#define LDT_FROM_DASHBOARD    2
#define LDT_NONE              0xFFFFFFFF

extern PLAUNCH_DATA_PAGE LaunchDataPage;        /* defined below w/ other DATA */

/*
 * Persistent chainload slot at a fixed low-PA page.  Retail Xbox does a warm
 * platform reset (SMC reset, RAM preserved, kernel re-initialised from scratch)
 * to chainload; on our side, RAM persists across qemu's device-reset, so a
 * fixed page reserved out of the contig allocator survives the SMC bounce and
 * carries the launch info to the next kernel boot.
 *
 * PA 0xE000 sits in the low reserved block (below the page directory at
 * PA 0xF000, above the BIOS data area) on BOTH boot layouts: never free
 * to any allocator, never zeroed by the loader, and outside the kernel
 * image -- on the retail split layout the old 0x12000 slot landed inside
 * .text, so the write scribbled kernel code and the reloaded kernel saw
 * no magic, costing titles their launch data across a QuickReboot.
 * KSEG0-mapped (VA = PA | 0x80000000). */
#define XB_PERSIST_PA      0x0000E000UL
#define XB_PERSIST_VA      (XB_PERSIST_PA | 0x80000000UL)
#define XB_PERSIST_MAGIC   0x6E78436CUL          /* 'lCxn' little-endian */

typedef struct _XB_PERSIST_SLOT
{
    ULONG              Magic;       /* XB_PERSIST_MAGIC iff valid          */
    ULONG              HadLaunchData; /* title supplied a LaunchDataPage   */
    LAUNCH_DATA_HEADER Header;      /* copied from the title's LaunchDataPage */
    UCHAR              LaunchData[3072]; /* the payload titles hand across */
} XB_PERSIST_SLOT, *PXB_PERSIST_SLOT;

/* The disc/HDD path of the XBE currently running, recorded at launch so a
 * warm reset with LDT_TITLE and an EMPTY szLaunchPath -- the retail
 * XLaunchNewImage(NULL, data) idiom -- can relaunch the same image. */
static CHAR XeRunningTitlePathA[520];

/*
 * Stash the title's reboot intent into the persistent slot so the next
 * kernel boot can route correctly after the SMC reset.  Three cases:
 *
 *   - LaunchDataPage = LDT_TITLE + szLaunchPath: explicit chainload to
 *     a named XBE on disc/HDD.
 *   - LaunchDataPage = anything else, OR NULL: title is asking to return
 *     to firmware -- on retail this means "boot the dashboard".  Stash
 *     LDT_LAUNCH_DASHBOARD so consume routes to C:\xboxdash.xbe instead
 *     of re-launching the disc title.
 *
 * Magic is always set on the way out; that flags "warm reset from a
 * title" so consume can distinguish from cold boot (where the slot is
 * still zero / garbage).
 */
static VOID
XeStashRebootIntent(VOID)
{
    PLAUNCH_DATA_PAGE p = LaunchDataPage;
    volatile PXB_PERSIST_SLOT slot = (PXB_PERSIST_SLOT)XB_PERSIST_VA;

    RtlZeroMemory((PVOID)&slot->Header, sizeof(slot->Header));
    RtlZeroMemory((PVOID)slot->LaunchData, sizeof(slot->LaunchData));
    slot->HadLaunchData = (p != NULL);

    if (p == NULL || p->Header.dwLaunchDataType == LDT_NONE)
    {
        /* No launch data: plain return to firmware = boot the dashboard. */
        XbDbg("return-to-dashboard requested\n");
        slot->Header.dwLaunchDataType = LDT_LAUNCH_DASHBOARD;
    }
    else
    {
        /* Preserve the page across the reset like retail RAM does: the
         * relaunched image reads the type and payload via XGetLaunchInfo. */
        slot->Header = p->Header;
        RtlCopyMemory((PVOID)slot->LaunchData, p->LaunchData,
                      sizeof(slot->LaunchData));

        if (p->Header.dwLaunchDataType == LDT_TITLE &&
            p->Header.szLaunchPath[0] == '\0')
        {
            /* XLaunchNewImage(NULL, data): relaunch the CURRENT title. */
            XbDbg("relaunch-current requested (%s)\n",
                  XeRunningTitlePathA);
            RtlCopyMemory(slot->Header.szLaunchPath, XeRunningTitlePathA,
                          sizeof(slot->Header.szLaunchPath));
        }
        else if (p->Header.dwLaunchDataType == LDT_TITLE)
        {
            XbDbg("chainload requested -> %s\n",
                  p->Header.szLaunchPath);
        }
        else
        {
            /* Dashboard-bound launch data (LDT_FROM_DASHBOARD etc.):
             * route to the dashboard but keep the payload for it. */
            XbDbg("return-to-dashboard with launch data (type %lu)\n",
                  p->Header.dwLaunchDataType);
        }
    }
    slot->Magic = XB_PERSIST_MAGIC;
}

/* exit() reaches HalReturnToFirmware(routine).  Translate the Xbox enum to
 * the NT one the HAL's HalReturnToFirmware expects.  Xbox SMC has only three
 * power actions (shutdown / cycle / reset); the "flavours" of reboot are
 * software conventions (EEPROM flags, dashboard hint bits) we don't model
 * yet, so all reboot variants collapse to SMC reset.
 *
 * HalQuickRebootRoutine is the chainload entry point on Xbox: a title sets
 * LaunchDataPage to a 4 KB page whose header names the next XBE to run, then
 * uses HalReturnToFirmware to "reboot" into it.  We mirror that on our side
 * by writing the launch header into a persistent low-PA page (survives the
 * qemu device-reset) and then driving the real SMC reset; the next kernel
 * boot reads the slot in XeRunInitialTitle. */
VOID NTAPI
XeHalReturnToFirmware(_In_ XBE_FIRMWARE_REENTRY Routine)
{
    FIRMWARE_REENTRY Action;

    /* Walk EBP frames to identify the title-side call site that decided to
     * reboot.  Recording the first few return addresses pins down which
     * path fired without needing a debugger. */
    {
        ULONG_PTR frames[6] = {0};
        ULONG_PTR *fp = (ULONG_PTR *)__builtin_frame_address(0);
        ULONG i;
        for (i = 0; i < RTL_NUMBER_OF(frames) && fp != NULL; i++)
        {
            /* Caller's return addr lives at [ebp+4]; previous ebp at [ebp]. */
            frames[i] = fp[1];
            fp = (ULONG_PTR *)fp[0];
            /* Stop if the chain leaves user-readable memory or self-loops. */
            if ((ULONG_PTR)fp < 0x10000 || (ULONG_PTR)fp == (ULONG_PTR)&frames[i])
                break;
        }
        XbDbg("HalReturnToFirmware(%lu) backtrace: "
              "%08lx %08lx %08lx %08lx %08lx %08lx\n",
              (ULONG)Routine, frames[0], frames[1], frames[2],
              frames[3], frames[4], frames[5]);
    }
    XbDbg("HalReturnToFirmware(%lu) -- title exited\n", (ULONG)Routine);

    if (Routine == XeFwQuickReboot)
        XeStashRebootIntent();

    switch (Routine)
    {
        case XeFwHalt:                Action = HalHaltRoutine;   break;
        case XeFwReboot:
        case XeFwQuickReboot:
        case XeFwKdReboot:
        case XeFwFatalErrorReboot:    Action = HalRebootRoutine; break;
        default:
            XbDbg("unknown FIRMWARE_REENTRY %lu, rebooting\n", (ULONG)Routine);
            Action = HalRebootRoutine;
            break;
    }

    HalReturnToFirmware(Action);
}

/*
 * XeLoadSection / XeUnloadSection -- on-demand XBE section loading, the
 * title-side memory-pressure valve.  Titles flag sections as not preloaded
 * and call XeLoadSection to bring the raw data in at runtime; the last
 * XeUnloadSection decommits the pages back to the free list.  State lives
 * in the title-visible section header itself: SectionRefCount counts
 * loads, and the head/tail shared-page counters (a USHORT array in the
 * XBE headers; adjacent sections share the boundary counter) decide when
 * a page straddling two sections may actually be freed.  Raw data
 * re-reads go through the image file handle the boot loader keeps open.
 * Section digests are not verified.
 */
static HANDLE XepImageFileHandle;
static KMUTEX XepSectionMutex;

/* Image VA range, recorded at load; MmQueryStatistics counts the
 * committed pages inside it as ImagePagesCommitted. */
ULONG_PTR XeImageRangeStart;
ULONG_PTR XeImageRangeEnd;

/* Commit + read + zero-fill one section.  Caller holds XepSectionMutex
 * and has checked SectionRefCount == 0.  Shared-page counters are bumped
 * only after the data is in place. */
static NTSTATUS
XepLoadSectionData(_Inout_ PXBE_SECTION Section)
{
    PVOID commitBase;
    SIZE_T commitSize;
    IO_STATUS_BLOCK iosb;
    LARGE_INTEGER off;
    NTSTATUS status;

    if (Section->VirtualSize == 0)
        return STATUS_SUCCESS;

    /* MEM_COMMIT rounds the base down / the end up to page boundaries;
     * re-committing a shared boundary page a neighbor already holds is a
     * contents-preserving no-op. */
    commitBase = (PVOID)(ULONG_PTR)Section->VirtualAddr;
    commitSize = Section->VirtualSize;
    status = XeNtAllocateVirtualMemory(&commitBase, 0, &commitSize,
                                       MEM_COMMIT, PAGE_EXECUTE_READWRITE);
    if (!NT_SUCCESS(status))
        return status;

    if (Section->SizeOfRaw != 0)
    {
        off.QuadPart = Section->RawAddr;
        status = ZwReadFile(XepImageFileHandle, NULL, NULL, NULL, &iosb,
                            (PVOID)(ULONG_PTR)Section->VirtualAddr,
                            Section->SizeOfRaw, &off, NULL);
        if (!NT_SUCCESS(status))
            return status;
    }
    if (Section->VirtualSize > Section->SizeOfRaw)
        RtlZeroMemory((PUCHAR)(ULONG_PTR)Section->VirtualAddr +
                          Section->SizeOfRaw,
                      Section->VirtualSize - Section->SizeOfRaw);

    (*(volatile USHORT *)(ULONG_PTR)Section->HeadSharedRefCountAddr)++;
    if (((Section->VirtualAddr + Section->VirtualSize - 1) & ~(PAGE_SIZE - 1)) !=
        (Section->VirtualAddr & ~(PAGE_SIZE - 1)))
        (*(volatile USHORT *)(ULONG_PTR)Section->TailSharedRefCountAddr)++;

    return STATUS_SUCCESS;
}

NTSYSAPI
NTSTATUS
NTAPI
NtFreeVirtualMemory(
    _In_ HANDLE ProcessHandle,
    _Inout_ PVOID *BaseAddress,
    _Inout_ PSIZE_T RegionSize,
    _In_ ULONG FreeType);

static VOID
XepDecommitRange(_In_ ULONG_PTR Start, _In_ ULONG_PTR End)
{
    PVOID base = (PVOID)Start;
    SIZE_T size = End - Start;

    if (Start < End)
        XeNtFreeVirtualMemory(&base, &size, MEM_DECOMMIT);
}

NTSTATUS NTAPI XeLoadSection(_Inout_ PXBE_SECTION Section)
{
    NTSTATUS status = STATUS_SUCCESS;

    if (Section == NULL)
        return STATUS_INVALID_PARAMETER;

    KeWaitForSingleObject(&XepSectionMutex, Executive, KernelMode,
                          FALSE, NULL);
    if (Section->SectionRefCount == 0)
        status = XepLoadSectionData(Section);
    if (NT_SUCCESS(status))
        Section->SectionRefCount++;
    KeReleaseMutex(&XepSectionMutex, FALSE);
    return status;
}

NTSTATUS NTAPI XeUnloadSection(_Inout_ PXBE_SECTION Section)
{
    NTSTATUS status = STATUS_SUCCESS;

    if (Section == NULL)
        return STATUS_INVALID_PARAMETER;

    KeWaitForSingleObject(&XepSectionMutex, Executive, KernelMode,
                          FALSE, NULL);
    if (Section->SectionRefCount == 0)
    {
        status = STATUS_INVALID_PARAMETER;
    }
    else if (--Section->SectionRefCount == 0 && Section->VirtualSize != 0)
    {
        ULONG_PTR headPage = Section->VirtualAddr & ~(ULONG_PTR)(PAGE_SIZE - 1);
        ULONG_PTR tailPage = (Section->VirtualAddr + Section->VirtualSize - 1)
                             & ~(ULONG_PTR)(PAGE_SIZE - 1);

        if (--(*(volatile USHORT *)(ULONG_PTR)Section->HeadSharedRefCountAddr) == 0)
            XepDecommitRange(headPage, headPage + PAGE_SIZE);
        if (tailPage != headPage &&
            --(*(volatile USHORT *)(ULONG_PTR)Section->TailSharedRefCountAddr) == 0)
            XepDecommitRange(tailPage, tailPage + PAGE_SIZE);
        XepDecommitRange(headPage + PAGE_SIZE, tailPage);
    }
    KeReleaseMutex(&XepSectionMutex, FALSE);
    return status;
}

/* --- Io* adapters: Xbox sigs differ from NT (ANSI names, fewer args) ---- */

/*
 * The console's file query is a much narrower routine than NT's: it
 * answers three classes and refuses everything else -- classes the file
 * system handles perfectly well through NtQueryInformationFile included.
 * The answer is always the class's own length, whatever the caller
 * claimed, so the class length is what goes to the file system: passing
 * a short one on would have a file system that checks refuse a request
 * the console completes.
 */
NTSTATUS NTAPI
XeIoQueryFileInformation(PFILE_OBJECT FileObject,
                           FILE_INFORMATION_CLASS FileInformationClass,
                           ULONG Length,
                           PVOID FileInformation,
                           PULONG ReturnedLength)
{
    ULONG ClassLength;

    UNREFERENCED_PARAMETER(Length);

    switch (FileInformationClass)
    {
        case FileInternalInformation:
            ClassLength = sizeof(FILE_INTERNAL_INFORMATION);
            break;
        case FilePositionInformation:
            ClassLength = sizeof(FILE_POSITION_INFORMATION);
            break;
        case FileNetworkOpenInformation:
            ClassLength = sizeof(FILE_NETWORK_OPEN_INFORMATION);
            break;
        default:
            return STATUS_INVALID_PARAMETER;
    }

    return IoQueryFileInformation(FileObject, FileInformationClass,
                                  ClassLength, FileInformation,
                                  ReturnedLength);
}

/* The volume query is the same shape with a set of its own.  The length
 * does reach the file system here -- the attribute class truncates
 * against it -- so it is passed on untouched. */
NTSTATUS NTAPI
XeIoQueryVolumeInformation(PFILE_OBJECT FileObject,
                             FS_INFORMATION_CLASS FsInformationClass,
                             ULONG Length,
                             PVOID FsInformation,
                             PULONG ReturnedLength)
{
    switch (FsInformationClass)
    {
        case FileFsVolumeInformation:
        case FileFsSizeInformation:
        case FileFsDeviceInformation:
        case FileFsAttributeInformation:
            break;
        default:
            return STATUS_INVALID_PARAMETER;
    }

    return IoQueryVolumeInformation(FileObject, FsInformationClass, Length,
                                    FsInformation, ReturnedLength);
}

/* IoCreateDevice, plus the provenance of the driver object. */
extern NTSTATUS NTAPI IopCreateDevice(
    _In_ PDRIVER_OBJECT DriverObject,
    _In_ ULONG DeviceExtensionSize,
    _In_opt_ PUNICODE_STRING DeviceName,
    _In_ DEVICE_TYPE DeviceType,
    _In_ ULONG DeviceCharacteristics,
    _In_ BOOLEAN Exclusive,
    _In_ BOOLEAN TitleOwnedDriver,
    _Out_ PDEVICE_OBJECT *DeviceObject);

/* DEVICE_OBJECT already carries the console's field order, so only the
 * ANSI device name needs converting. */
NTSTATUS NTAPI
XeIoCreateDevice(_In_ PVOID DriverObject,
                   _In_ ULONG DeviceExtensionSize,
                   _In_opt_ PSTRING DeviceName,
                   _In_ DEVICE_TYPE DeviceType,
                   _In_ BOOLEAN Exclusive,
                   _Out_ PVOID *DeviceObject)
{
    UNICODE_STRING uname = {0};
    PUNICODE_STRING pname = NULL;
    NTSTATUS s;
    PDEVICE_OBJECT dev;

    if (DeviceName != NULL && DeviceName->Buffer != NULL)
    {
        ANSI_STRING aname = { DeviceName->Length, DeviceName->MaximumLength,
                              DeviceName->Buffer };
        if (NT_SUCCESS(RtlAnsiStringToUnicodeString(&uname, &aname, TRUE)))
            pname = &uname;
    }
    /* The driver object belongs to the title. */
    s = IopCreateDevice((PDRIVER_OBJECT)DriverObject, DeviceExtensionSize,
                        pname, DeviceType, /*Characteristics*/ 0, Exclusive,
                        /*TitleOwnedDriver*/ TRUE, &dev);
    if (uname.Buffer != NULL) RtlFreeUnicodeString(&uname);
    if (NT_SUCCESS(s) && dev != NULL)
        *DeviceObject = dev;
    return s;
}

NTSTATUS NTAPI
IoInvalidDeviceRequest(_In_ PVOID DeviceObject, _In_ PVOID Irp)
{
    UNREFERENCED_PARAMETER(DeviceObject);
    UNREFERENCED_PARAMETER(Irp);
    return STATUS_INVALID_DEVICE_REQUEST;
}

/* Xbox dropped NT's Cancelable flag; default to FALSE. */
VOID NTAPI
XeIoStartNextPacket(_In_ PVOID DeviceObject)
{
    IoStartNextPacket((PDEVICE_OBJECT)DeviceObject, FALSE);
}

/* Same drop of the cancelable flag, for the by-key form. */
VOID NTAPI
XeIoStartNextPacketByKey(_In_ PVOID DeviceObject, _In_ ULONG Key)
{
    IoStartNextPacketByKey((PDEVICE_OBJECT)DeviceObject, FALSE, Key);
}

/* Xbox has no quota to charge; the packet always comes from the pool. */
PVOID NTAPI
XeIoAllocateIrp(_In_ CCHAR StackSize)
{
    return IoAllocateIrp(StackSize, FALSE);
}

/* Xbox dropped NT's cancel-routine arg; pass NULL. */
VOID NTAPI
XeIoStartPacket(_In_ PVOID DeviceObject, _In_ PVOID Irp,
                  _In_opt_ PULONG Key)
{
    IoStartPacket((PDEVICE_OBJECT)DeviceObject, (PIRP)Irp, Key, NULL);
}

/* The console offers a request builder paired with its own dispatch and
 * wait, so a caller that just wants the result never handles the packet
 * itself.  Both forms own the event and the status block, and both
 * report the completion status rather than the dispatch return. */
static NTSTATUS
XbIoAwaitRequest(_In_ PDEVICE_OBJECT DeviceObject,
                 _In_ PIRP Irp,
                 _In_ PKEVENT Event,
                 _In_ PIO_STATUS_BLOCK IoStatusBlock)
{
    NTSTATUS s = IofCallDriver(DeviceObject, Irp);

    if (s == STATUS_PENDING)
    {
        KeWaitForSingleObject(Event, Executive, KernelMode, FALSE, NULL);
        s = IoStatusBlock->Status;
    }
    return s;
}

NTSTATUS NTAPI
XeIoSynchronousFsdRequest(_In_ ULONG MajorFunction,
                          _In_ PVOID DeviceObject,
                          _Inout_opt_ PVOID Buffer,
                          _In_ ULONG Length,
                          _In_opt_ PLARGE_INTEGER StartingOffset)
{
    PDEVICE_OBJECT dev = (PDEVICE_OBJECT)DeviceObject;
    IO_STATUS_BLOCK iosb;
    KEVENT event;
    PIRP irp;

    KeInitializeEvent(&event, NotificationEvent, FALSE);
    irp = IoBuildSynchronousFsdRequest(MajorFunction, dev, Buffer, Length,
                                       StartingOffset, &event, &iosb);
    if (irp == NULL)
        return STATUS_INSUFFICIENT_RESOURCES;

    return XbIoAwaitRequest(dev, irp, &event, &iosb);
}

NTSTATUS NTAPI
XeIoSynchronousDeviceIoControlRequest(_In_ ULONG IoControlCode,
                                      _In_ PVOID DeviceObject,
                                      _In_opt_ PVOID InputBuffer,
                                      _In_ ULONG InputBufferLength,
                                      _Out_opt_ PVOID OutputBuffer,
                                      _In_ ULONG OutputBufferLength,
                                      _Out_opt_ PULONG ReturnedOutputBufferLength,
                                      _In_ BOOLEAN InternalDeviceIoControl)
{
    PDEVICE_OBJECT dev = (PDEVICE_OBJECT)DeviceObject;
    IO_STATUS_BLOCK iosb;
    KEVENT event;
    PIRP irp;
    NTSTATUS s;

    KeInitializeEvent(&event, NotificationEvent, FALSE);
    irp = IoBuildDeviceIoControlRequest(IoControlCode, dev, InputBuffer,
                                        InputBufferLength, OutputBuffer,
                                        OutputBufferLength,
                                        InternalDeviceIoControl,
                                        &event, &iosb);
    if (irp == NULL)
        return STATUS_INSUFFICIENT_RESOURCES;

    s = XbIoAwaitRequest(dev, irp, &event, &iosb);

    /* The transferred length is reported separately from the status. */
    if (ReturnedOutputBufferLength != NULL)
        *ReturnedOutputBufferLength = (ULONG)iosb.Information;
    return s;
}

/* --- KeQueryInterruptTime / Performance counter -- ULONGLONG returns ---- */
ULONGLONG NTAPI XeKeQueryInterruptTime(VOID)
{
    /* The live interrupt-time counter the clock advances (1ms ticks); the
     * same value the exported KeInterruptTime datum carries. */
    return KeQueryInterruptTime();
}
/* The Xbox performance counter is the ACPI PM timer, a 24-bit free-running
 * counter at PMBASE+0x08 (PMBASE = 0x8000 on retail, set by the BIOS via
 * the LPC PMBASE config register).  Its frequency is 3,375,000 Hz on Xbox
 * (vs. 3,579,545 Hz on a PC -- the Xbox part divides the 14.318 MHz NTSC
 * subcarrier by 4.25 instead of 4).  Retail KeQueryPerformanceFrequency
 * returns 0x00337F98 = 3,374,488 Hz; the 512 Hz drift from the canonical
 * 3,375,000 is a one-time calibration applied at boot on real silicon --
 * we match the published value exactly so any title that hardcodes it
 * compares equal.
 *
 * The counter is only 24 bits wide so it wraps every ~4.97 s.  Extend to
 * 64 bits by detecting wrap-around relative to the last sample. */
#define XB_PMT_PORT       ((PULONG)0x8008)
#define XB_PMT_MASK       0x00FFFFFFUL
#define XB_PMT_FREQUENCY  3374488ULL

static volatile ULONGLONG NxkPmtHigh = 0;
static volatile ULONG     NxkPmtLastLow = 0;

static ULONGLONG NxkReadPmt(VOID)
{
    KIRQL OldIrql;
    ULONG cur, last;
    ULONGLONG result;

    /* Serialize the read+wrap-detect against other CPUs / interrupts.
     * Single-CPU box, so DISPATCH_LEVEL keeps DPCs and lower-IRQL code out. */
    KeRaiseIrql(DISPATCH_LEVEL, &OldIrql);
    cur  = READ_PORT_ULONG(XB_PMT_PORT) & XB_PMT_MASK;
    last = NxkPmtLastLow;
    if (cur < last)
        NxkPmtHigh += (XB_PMT_MASK + 1ULL);
    NxkPmtLastLow = cur;
    result = NxkPmtHigh | cur;
    KeLowerIrql(OldIrql);
    return result;
}

ULONGLONG NTAPI XeKeQueryPerformanceCounter(VOID)
{
    return NxkReadPmt();
}

ULONGLONG NTAPI KeQueryPerformanceFrequency(VOID)
{
    return XB_PMT_FREQUENCY;
}

/* Xbox ExQueryPoolBlockSize dropped NT's PoolType out-param. */
ULONG NTAPI XeExQueryPoolBlockSize(_In_ PVOID Block)
{
    POOL_TYPE pt = NonPagedPool;
    return ExQueryPoolBlockSize(Block, (PBOOLEAN)&pt);
}

VOID NTAPI IoMarkIrpMustComplete(_In_ PVOID Irp) { UNREFERENCED_PARAMETER(Irp); }

/* --- NtUserIoApcDispatcher (Xbox async-IO completion APC) --------------- *
 *
 * Async NtReadFile/NtWriteFile completion is delivered via a user APC whose
 * routine is this dispatcher; ApcContext is the title's Win32-style overlapped
 * completion routine and IosbPtr doubles as the title's OVERLAPPED.  Convert
 * NTSTATUS/Information to (dwErrorCode, dwBytesTransferred) and invoke.
 *
 * Titles run ring 0, so RequestorMode = KernelMode and the completion comes
 * as a kernel-mode APC -- it runs but does not set UserApcPending, so an
 * alertable UserMode wait sleeps its full timeout instead of waking with
 * STATUS_USER_APC.  Flag the pending user APC here so KiCheckAlertability
 * returns STATUS_USER_APC from the wait. */
NTKERNELAPI ULONG NTAPI RtlNtStatusToDosError(IN NTSTATUS Status);
NTKERNELAPI VOID NTAPI KiSetCurrentThreadUserApcPending(VOID);
VOID NTAPI NtUserIoApcDispatcher(_In_ PVOID ApcContext, _In_ PVOID IosbPtr,
                                     _In_ ULONG Reserved)
{
    PIO_STATUS_BLOCK Iosb = (PIO_STATUS_BLOCK)IosbPtr;
    ULONG dwError = 0, dwTransferred = 0;
    VOID (NTAPI *CompletionRoutine)(ULONG, ULONG, PVOID) =
        (VOID (NTAPI *)(ULONG, ULONG, PVOID))ApcContext;

    UNREFERENCED_PARAMETER(Reserved);
    if (CompletionRoutine == NULL || Iosb == NULL)
        return;

    if (NT_SUCCESS(Iosb->Status))
        dwTransferred = (ULONG)Iosb->Information;
    else
        dwError = RtlNtStatusToDosError(Iosb->Status);

    /* lpOverlapped == the IoStatusBlock the title handed to Nt{Read,Write}File. */
    CompletionRoutine(dwError, dwTransferred, (PVOID)Iosb);

    KiSetCurrentThreadUserApcPending();
}

/* --- kernel ordinal export ------------------------------------------------ *
 *
 * The XBE imports kernel routines by ordinal through its thunk table; this
 * file (with help from the rest of xb/) resolves that table against the
 * kernel's PE export directory.  Most ordinals map straight to a ReactOS
 * ntoskrnl export.  Where the Xbox signature differs -- dropped parameters
 * (process handle, access mask, EA buffers, file key) or the *Ex waits'
 * extra WaitMode -- the export points instead at an XeNt/XeOb adapter
 * below: each takes the Xbox signature and calls ntoskrnl with the arguments
 * NT expects.  Adapter arity matches the Xbox @N so the title's
 * callee-cleanup pop stays balanced.  An adapter's ntoskrnl target is
 * declared here with a real prototype (ros_) and its exact decorated name.
 */

extern NTSTATUS NTAPI ros_NtAllocateVirtualMemory(
    HANDLE, PVOID *, ULONG_PTR, PSIZE_T, ULONG, ULONG)
    __asm__("_NtAllocateVirtualMemory@24");
extern NTSTATUS NTAPI ros_NtFreeVirtualMemory(
    HANDLE, PVOID *, PSIZE_T, ULONG)
    __asm__("_NtFreeVirtualMemory@16");
extern NTSTATUS NTAPI ros_NtProtectVirtualMemory(
    HANDLE, PVOID *, PSIZE_T, ULONG, PULONG)
    __asm__("_NtProtectVirtualMemory@20");
extern NTSTATUS NTAPI ros_NtQueryVirtualMemory(
    HANDLE, PVOID, ULONG /* MEMORY_INFORMATION_CLASS */,
    PVOID, SIZE_T, PSIZE_T)
    __asm__("_NtQueryVirtualMemory@24");
extern NTSTATUS NTAPI ros_NtCreateEvent(
    PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES, EVENT_TYPE, BOOLEAN)
    __asm__("_NtCreateEvent@20");
extern NTSTATUS NTAPI ros_NtWaitForSingleObject(
    HANDLE, BOOLEAN, PLARGE_INTEGER)
    __asm__("_NtWaitForSingleObject@12");
/* Internal ntoskrnl helper (ob/obwait.c) -- waits honoring an explicit WaitMode. */
extern NTSTATUS NTAPI NxkWaitForSingleObjectMode(
    HANDLE, KPROCESSOR_MODE, BOOLEAN, PLARGE_INTEGER);
extern NTSTATUS NTAPI NxkWaitForMultipleObjectsMode(
    ULONG, PHANDLE, WAIT_TYPE, KPROCESSOR_MODE, BOOLEAN, PLARGE_INTEGER);
extern NTSTATUS NTAPI NxkSignalAndWaitForSingleObjectMode(
    HANDLE, HANDLE, KPROCESSOR_MODE, BOOLEAN, PLARGE_INTEGER);
extern NTSTATUS NTAPI ros_NtQuerySymbolicLinkObject(
    HANDLE, PUNICODE_STRING, PULONG)
    __asm__("_NtQuerySymbolicLinkObject@12");
extern NTSTATUS NTAPI ros_NtDuplicateObject(
    HANDLE, HANDLE, HANDLE, PHANDLE, ACCESS_MASK, ULONG, ULONG)
    __asm__("_NtDuplicateObject@28");
/* Internal object-directory type (ob/obdir.c). */
extern POBJECT_TYPE ObpDirectoryObjectType;
extern POBJECT_TYPE IoCompletionType;
extern NTSTATUS NTAPI ObOpenObjectByName(
    POBJECT_ATTRIBUTES, POBJECT_TYPE, KPROCESSOR_MODE, PACCESS_STATE,
    ACCESS_MASK, PVOID, PHANDLE);
extern NTSTATUS NTAPI ros_NtWaitForMultipleObjects(
    ULONG, PHANDLE, WAIT_TYPE, BOOLEAN, PLARGE_INTEGER)
    __asm__("_NtWaitForMultipleObjects@20");
extern NTSTATUS NTAPI ros_NtCreateDirectoryObject(
    PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES)
    __asm__("_NtCreateDirectoryObject@12");
extern NTSTATUS NTAPI ros_NtOpenDirectoryObject(
    PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES)
    __asm__("_NtOpenDirectoryObject@12");
extern NTSTATUS NTAPI ros_NtOpenSymbolicLinkObject(
    PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES)
    __asm__("_NtOpenSymbolicLinkObject@12");
extern NTSTATUS NTAPI ros_NtQueryDirectoryObject(
    HANDLE, PVOID, ULONG, BOOLEAN, BOOLEAN, PULONG, PULONG)
    __asm__("_NtQueryDirectoryObject@28");
extern NTSTATUS NTAPI ros_NtQueryFullAttributesFile(
    POBJECT_ATTRIBUTES, PVOID)
    __asm__("_NtQueryFullAttributesFile@8");
extern NTSTATUS NTAPI ros_NtDeleteFile(POBJECT_ATTRIBUTES)
    __asm__("_NtDeleteFile@4");

/* --- object attributes ---------------------------------------------------- *
 * The Xbox OBJECT_ATTRIBUTES is a different struct from NT's: three fields,
 * with an *ANSI* object name -- where NT's is larger and UNICODE.  Comparing
 * argument *bytes* never caught this (a POBJECT_ATTRIBUTES and a
 * PUNICODE_STRING are both 4-byte pointers); the parameter *type* is what
 * differs.  Every ordinal that takes a POBJECT_ATTRIBUTES is translated. */

typedef struct _XBE_OBJECT_ATTRIBUTES
{
    HANDLE       RootDirectory;
    PANSI_STRING ObjectName;
    ULONG        Attributes;
} XBE_OBJECT_ATTRIBUTES, *PXBE_OBJECT_ATTRIBUTES;

/*
 * Translate a Xbox OBJECT_ATTRIBUTES into NT form in caller storage.  Returns
 * the OA pointer to hand to ntoskrnl (NULL if the Xbox OA was NULL).  If
 * Name->Buffer is non-NULL afterwards the caller must RtlFreeUnicodeString it.
 */
/* Retail roots unrooted object names ("mutex_3") in a flat kernel
 * namespace shared by every creator/opener; NT rejects them with
 * STATUS_OBJECT_PATH_SYNTAX_BAD.  Root them under a private \Xbox
 * directory instead so two creates of the same name share one object
 * (XAPI passes OBJ_OPENIF for the Win32 create-or-open semantics). */
static HANDLE
XeNamedObjectRoot(VOID)
{
    static HANDLE Dir;
    OBJECT_ATTRIBUTES oa;
    UNICODE_STRING name = RTL_CONSTANT_STRING(L"\\Xbox");
    HANDLE h;

    if (Dir != NULL)
        return Dir;

    InitializeObjectAttributes(&oa, &name,
                               OBJ_PERMANENT | OBJ_KERNEL_HANDLE | OBJ_OPENIF,
                               NULL, NULL);
    {
        NTSTATUS s = ZwCreateDirectoryObject(&h, DIRECTORY_ALL_ACCESS, &oa);
        XbDbg("\\Xbox dir create -> %08lx\n", s);
        if (NT_SUCCESS(s))
            Dir = h;
    }
    return Dir;
}

static POBJECT_ATTRIBUTES
XeTranslateOa(_In_opt_ PXBE_OBJECT_ATTRIBUTES Xbox,
                _Out_ POBJECT_ATTRIBUTES Nt, _Out_ PUNICODE_STRING Name)
{
    HANDLE RootDirectory;

    Name->Buffer = NULL;
    Name->Length = Name->MaximumLength = 0;
    if (Xbox == NULL)
        return NULL;

    RootDirectory = Xbox->RootDirectory;

    /* Xbox pseudo root-directory handles (retail ob.h): (HANDLE)-3 is
     * the \?? DOS-devices directory and (HANDLE)-4 the Win32 named
     * object namespace (XAPI passes it for CreateMutex/Event/Semaphore
     * names).  NT's ObReferenceObjectByHandle rejects both with
     * STATUS_INVALID_HANDLE, which used to surface as failed named
     * creates.  Map -4 to our \Xbox directory; -3 falls through to the
     * name-prefix logic below with a NULL root. */
    if (RootDirectory == (HANDLE)(LONG_PTR)-4)
        RootDirectory = XeNamedObjectRoot();

    if (Xbox->ObjectName != NULL)
    {
        PANSI_STRING Ansi = Xbox->ObjectName;
        CHAR Drive = (Ansi->Length >= 2) ? Ansi->Buffer[0] : 0;

        /* Xbox titles hand DOS drive-letter paths ("Z:\cache003.map")
         * straight to NtCreateFile/NtOpenFile: the Xbox kernel resolves the
         * drive letter against its \??\ DOS-device links and ignores any
         * RootDirectory.  NT's object manager does neither -- a non-"\"-rooted
         * name with a NULL root is STATUS_OBJECT_PATH_SYNTAX_BAD, and the
         * leftover RootDirectory handle the title passes (meaningless for a
         * drive-absolute path) trips ObReferenceObjectByHandle ->
         * STATUS_INVALID_HANDLE (c0000008).  Rewrite "X:\..." -> "\??\X:\..."
         * and drop the root so the symlink IoCreateSymbolicLink created
         * resolves. */
        if (((Drive >= 'A' && Drive <= 'Z') || (Drive >= 'a' && Drive <= 'z')) &&
            Ansi->Buffer[1] == ':')
        {
            static const CHAR Prefix[] = "\\??\\";
            USHORT PrefixLen = sizeof(Prefix) - 1;
            USHORT Total = PrefixLen + Ansi->Length;
            PCHAR Buf = ExAllocatePoolWithTag(NonPagedPool, Total, 'aOxN');

            if (Buf != NULL)
            {
                ANSI_STRING Dos;
                RtlCopyMemory(Buf, Prefix, PrefixLen);
                RtlCopyMemory(Buf + PrefixLen, Ansi->Buffer, Ansi->Length);
                Dos.Buffer = Buf;
                Dos.Length = Dos.MaximumLength = Total;
                /* OOM is harmless: Name was pre-zeroed at function entry, so
                 * a failed conversion leaves Buffer == NULL and the
                 * Name->Buffer ? Name : NULL check below picks it up. */
                NTSTATUS _ign = RtlAnsiStringToUnicodeString(Name, &Dos, TRUE);
                (void)_ign;
                ExFreePool(Buf);
                RootDirectory = NULL;
            }
            else
            {
                NTSTATUS _ign = RtlAnsiStringToUnicodeString(Name, Ansi, TRUE);
                (void)_ign;
            }
        }
        else if (RootDirectory == (HANDLE)(LONG_PTR)-3 &&
                 Ansi->Length != 0 && Ansi->Buffer[0] != '\\')
        {
            /* DOS-devices pseudo root: same rewrite as drive-letter
             * paths -- "name" under (HANDLE)-3 means "\??\name". */
            static const CHAR Prefix[] = "\\??\\";
            USHORT PrefixLen = sizeof(Prefix) - 1;
            USHORT Total = PrefixLen + Ansi->Length;
            PCHAR Buf = ExAllocatePoolWithTag(NonPagedPool, Total, 'aOxN');

            if (Buf != NULL)
            {
                ANSI_STRING Dos;
                RtlCopyMemory(Buf, Prefix, PrefixLen);
                RtlCopyMemory(Buf + PrefixLen, Ansi->Buffer, Ansi->Length);
                Dos.Buffer = Buf;
                Dos.Length = Dos.MaximumLength = Total;
                NTSTATUS _ign = RtlAnsiStringToUnicodeString(Name, &Dos, TRUE);
                (void)_ign;
                ExFreePool(Buf);
            }
            RootDirectory = NULL;
        }
        else
        {
            NTSTATUS _ign = RtlAnsiStringToUnicodeString(Name, Ansi, TRUE);
            (void)_ign;

            /* Unrooted non-path name: give it the retail-style shared
             * namespace via the \Xbox directory. */
            if (RootDirectory == NULL && Ansi->Length != 0 &&
                Ansi->Buffer[0] != '\\')
                RootDirectory = XeNamedObjectRoot();
        }
    }
    InitializeObjectAttributes(Nt, Name->Buffer != NULL ? Name : NULL,
                               Xbox->Attributes, RootDirectory, NULL);
    return Nt;
}

/* obcreate.c builds the shim object's attributes with the same
 * translation every other named entry uses. */
POBJECT_ATTRIBUTES
XeTranslateXboxOa(PVOID Xbox, POBJECT_ATTRIBUTES Nt, PUNICODE_STRING Name)
{
    return XeTranslateOa(Xbox, Nt, Name);
}

NTSTATUS NTAPI
XeNtAllocateVirtualMemory(PVOID *Base, ULONG_PTR ZeroBits, PSIZE_T Size,
                            ULONG Type, ULONG Protect)
{
#ifdef NXK_MM_VM
    extern NTSTATUS NTAPI NxVmAllocateVirtualMemory(PVOID *, ULONG_PTR,
                                                    PSIZE_T, ULONG, ULONG);
    return NxVmAllocateVirtualMemory(Base, ZeroBits, Size, Type, Protect);
#else
    return ros_NtAllocateVirtualMemory(ZwCurrentProcess(), Base, ZeroBits,
                                       Size, Type, Protect);
#endif
}

/* Xbox dropped NT's POOL_TYPE arg -- the kernel has no paged pool, so every
 * allocation is implicitly NonPagedPool. */
PVOID NTAPI
XeExAllocatePool(SIZE_T NumberOfBytes)
{
    return ExAllocatePool(NonPagedPool, NumberOfBytes);
}
PVOID NTAPI
XeExAllocatePoolWithTag(SIZE_T NumberOfBytes, ULONG Tag)
{
    return ExAllocatePoolWithTag(NonPagedPool, NumberOfBytes, Tag);
}

/*
 * Interlocked SLIST primitives.  SLIST_HEADER is identical on Xbox and NT
 * x86 (8 bytes: ULONGLONG Alignment ∪ {SLIST_ENTRY Next; USHORT Depth;
 * USHORT Sequence}), so the data is compatible -- but Xbox uses a lockless
 * 1/2-arg FASTCALL ABI while NT's `ExInterlockedPop/PushEntrySList` threads
 * a separate spinlock pointer through.
 *
 * Inline a small CMPXCHG8B loop instead of forwarding to
 * `RtlInterlockedPopEntrySList` -- that lives in librtl and re-linking it
 * into ntoskrnl conflicts with ntoskrnl's own `fastinterlck_asm.S`
 * (duplicate `ExpInterlockedPopEntrySListFault@0`).
 *
 * InterlockedFlushSList(ListHead) is FASTCALL 1 arg.  ReactOS's
 * `ExInterlockedFlushSList(ptr)` is already fastcall 1 arg with the same
 * shape and lives in ntoskrnl itself, so it forwards directly via the spec
 * file; only Pop/Push need thunks. */
/* Build a SLIST_HEADER value from its three fields.  Layout (8 bytes,
 * little-endian): [0..3]=Next, [4..5]=Depth, [6..7]=Sequence -- so as a
 * ULONGLONG, Next occupies bits 0..31, Depth bits 32..47, Sequence bits
 * 48..63. */
static inline ULONGLONG
NxpSListPack(PSLIST_ENTRY Next, USHORT Depth, USHORT Sequence)
{
    return (ULONGLONG)(ULONG_PTR)Next
         | ((ULONGLONG)Depth    << 32)
         | ((ULONGLONG)Sequence << 48);
}

PSLIST_ENTRY __fastcall
XeInterlockedPopEntrySList(PSLIST_HEADER ListHead)
{
    ULONGLONG oldHead, newHead;
    PSLIST_ENTRY entry;
    USHORT depth, sequence;

    do {
        oldHead = ListHead->Alignment;
        entry = (PSLIST_ENTRY)(ULONG_PTR)oldHead;
        if (entry == NULL)
            return NULL;
        depth    = (USHORT)((oldHead >> 32) & 0xFFFF);
        sequence = (USHORT)((oldHead >> 48) & 0xFFFF);
        /* Pop: replace Next with entry->Next; Depth--; Sequence++ (ABA). */
        newHead = NxpSListPack(entry->Next, depth - 1, sequence + 1);
    } while (!__sync_bool_compare_and_swap(&ListHead->Alignment,
                                            oldHead, newHead));
    return entry;
}

PSLIST_ENTRY __fastcall
XeInterlockedPushEntrySList(PSLIST_HEADER ListHead, PSLIST_ENTRY Entry)
{
    ULONGLONG oldHead, newHead;
    PSLIST_ENTRY oldFirst;
    USHORT depth, sequence;

    do {
        oldHead = ListHead->Alignment;
        oldFirst = (PSLIST_ENTRY)(ULONG_PTR)oldHead;
        depth    = (USHORT)((oldHead >> 32) & 0xFFFF);
        sequence = (USHORT)((oldHead >> 48) & 0xFFFF);
        Entry->Next = oldFirst;
        /* Push: Next = Entry; Depth++; Sequence unchanged. */
        newHead = NxpSListPack(Entry, depth + 1, sequence);
    } while (!__sync_bool_compare_and_swap(&ListHead->Alignment,
                                            oldHead, newHead));
    return oldFirst;
}
NTSTATUS NTAPI
XeNtFreeVirtualMemory(PVOID *Base, PSIZE_T Size, ULONG FreeType)
{
#ifdef NXK_MM_VM
    extern NTSTATUS NTAPI NxVmFreeVirtualMemory(PVOID *, PSIZE_T, ULONG);
    return NxVmFreeVirtualMemory(Base, Size, FreeType);
#else
    return ros_NtFreeVirtualMemory(ZwCurrentProcess(), Base, Size, FreeType);
#endif
}
/* Xbox drops NT's process handle: there is only one address space. */
NTSTATUS NTAPI
XeNtProtectVirtualMemory(PVOID *Base, PSIZE_T Size, ULONG NewProtect,
                           PULONG OldProtect)
{
#ifdef NXK_MM_VM
    return NxVmProtectVirtualMemory(Base, Size, NewProtect, OldProtect);
#else
    return ros_NtProtectVirtualMemory(ZwCurrentProcess(), Base, Size,
                                      NewProtect, OldProtect);
#endif
}
/* Xbox NtQueryVirtualMemory takes a base and returns
 * MEMORY_BASIC_INFORMATION; NT's takes a process handle, info class, length,
 * and a return-length pair. */
NTSTATUS NTAPI
XeNtQueryVirtualMemory(PVOID Base, PVOID /* PMEMORY_BASIC_INFORMATION */ Mbi)
{
#ifdef NXK_MM_VM
    extern NTSTATUS NTAPI NxVmQueryVirtualMemory(PVOID, PVOID);
    return NxVmQueryVirtualMemory(Base, Mbi);
#else
    /* MemoryBasicInformation == 0; MEMORY_BASIC_INFORMATION is 32 bytes. */
    SIZE_T ReturnLength = 0;
    return ros_NtQueryVirtualMemory(ZwCurrentProcess(), Base,
                                    0 /* MemoryBasicInformation */,
                                    Mbi, 32, &ReturnLength);
#endif
}
/*
 * Xbox NtPulseEvent works from DPC context; NT's NtPulseEvent asserts
 * `IRQL <= APC_LEVEL` (PAGED_CODE) and walks the handle table via
 * ObReferenceObjectByHandle -- which also asserts.  Frame-loop DPCs in
 * Xbox 3D middleware pulse their VBlank event at DISPATCH_LEVEL, so the
 * NT path bugchecks with KMODE_EXCEPTION_NOT_HANDLED in ex/event.c.
 *
 * Cache (handle -> KEVENT) at PASSIVE inside XeNtCreateEvent.
 * XeNtPulseEvent skips the Ob/PAGED_CODE machinery and calls the DPC-safe
 * KePulseEvent directly.  The handle keeps the underlying object alive, so
 * the cached pointer stays valid until the handle is closed -- eviction
 * hangs off ObpCloseHandle, the point every close path funnels through.
 */
typedef struct _XBE_EVENT_ENTRY
{
    HANDLE  Handle;
    PKEVENT Event;
} XBE_EVENT_ENTRY;

#define XBE_EVENT_SLOTS    64

static XBE_EVENT_ENTRY XeEventTable[XBE_EVENT_SLOTS];

/* Reported once per fill so a title holding more than XBE_EVENT_SLOTS
 * events at a time doesn't bury the log; cleared again when a slot frees. */
static BOOLEAN XeEventTableFullReported;

static
VOID
XeTrackEvent(HANDLE Handle, PKEVENT Event)
{
    KIRQL OldIrql;
    ULONG i;
    BOOLEAN Report = FALSE;

    KeRaiseIrql(HIGH_LEVEL, &OldIrql);
    for (i = 0; i < XBE_EVENT_SLOTS; i++)
    {
        if (XeEventTable[i].Handle == NULL)
        {
            XeEventTable[i].Handle = Handle;
            XeEventTable[i].Event = Event;
            break;
        }
    }
    if (i == XBE_EVENT_SLOTS && !XeEventTableFullReported)
    {
        XeEventTableFullReported = TRUE;
        Report = TRUE;
    }
    KeLowerIrql(OldIrql);
    if (Report)
        XbDbg("all %u pulse-cache slots are in use; events created from "
              "now on are not cached, so a later NtPulseEvent on one takes "
              "the NT path and is unsafe above APC_LEVEL\n",
              XBE_EVENT_SLOTS);
}

/* Closing a handle must evict the cache entry: the object dies with the
 * last handle, and handle values are recycled, so a stale slot would hand
 * XeNtPulseEvent a freed (or unrelated) KEVENT.  Called from
 * ObpCloseHandle, which every close funnels through -- NtClose is only one
 * of them, NtDuplicateObject(DUPLICATE_CLOSE_SOURCE) is another. */
VOID NTAPI
XeUntrackEvent(HANDLE Handle)
{
    KIRQL OldIrql;
    ULONG i;

    KeRaiseIrql(HIGH_LEVEL, &OldIrql);
    for (i = 0; i < XBE_EVENT_SLOTS; i++)
    {
        if (XeEventTable[i].Handle == Handle)
        {
            XeEventTable[i].Handle = NULL;
            XeEventTable[i].Event = NULL;
            XeEventTableFullReported = FALSE;
        }
    }
    KeLowerIrql(OldIrql);
}

static
PKEVENT
XeLookupEvent(HANDLE Handle)
{
    /* Registered at PASSIVE before the title ever pulses from a DPC, so the
     * lookup is lock-free.  Hot path: once per NtPulseEvent (per vblank). */
    ULONG i;

    for (i = 0; i < XBE_EVENT_SLOTS; i++)
    {
        if (XeEventTable[i].Handle == Handle)
        {
            return XeEventTable[i].Event;
        }
    }
    return NULL;
}

NTSTATUS NTAPI
XeNtCreateEvent(PHANDLE Handle, PXBE_OBJECT_ATTRIBUTES XAttr, EVENT_TYPE Type,
                  BOOLEAN InitialState)
{
    OBJECT_ATTRIBUTES ntoa;
    UNICODE_STRING name;
    POBJECT_ATTRIBUTES oa = XeTranslateOa(XAttr, &ntoa, &name);
    NTSTATUS status = ros_NtCreateEvent(Handle, EVENT_ALL_ACCESS, oa, Type,
                                        InitialState);
    if (name.Buffer != NULL)
        RtlFreeUnicodeString(&name);

    /* Cache the handle -> KEVENT mapping so XeNtPulseEvent can find the
     * object without Ob (which requires PASSIVE). */
    if (NT_SUCCESS(status))
    {
        PVOID Object;
        if (NT_SUCCESS(ObReferenceObjectByHandle(*Handle, 0,
                                                 ExEventObjectType,
                                                 KernelMode, &Object, NULL)))
        {
            /* The handle holds its own reference; ours is no longer needed. */
            ObDereferenceObject(Object);
            XeTrackEvent(*Handle, (PKEVENT)Object);
        }
    }
    return status;
}

/* NtPulseEvent prototype -- not in xbe.c's headers; declare locally. */
NTSYSCALLAPI NTSTATUS NTAPI NtPulseEvent(IN HANDLE EventHandle,
                                         OUT PLONG PreviousState OPTIONAL);

NTSTATUS NTAPI
XeNtPulseEvent(HANDLE Handle, PLONG PreviousState)
{
    PKEVENT Event = XeLookupEvent(Handle);
    LONG Prev;

    if (Event == NULL)
    {
        /* Unknown handle (probably non-title-created): take the NT path. */
        return NtPulseEvent(Handle, PreviousState);
    }
    Prev = KePulseEvent(Event, EVENT_INCREMENT, FALSE);
    if (PreviousState != NULL)
        *PreviousState = Prev;
    return STATUS_SUCCESS;
}
/* The Xbox form of NT's IoCreateFile: no EA buffer and no create-file
 * type, but the trailing Options word survives -- it is the only way a
 * caller can ask for the create-parameter validation that NtCreateFile,
 * passing zero, always skips. */
NTSTATUS NTAPI
XeIoCreateFile(PHANDLE Handle, ACCESS_MASK Access, PXBE_OBJECT_ATTRIBUTES XAttr,
                 PIO_STATUS_BLOCK Iosb, PLARGE_INTEGER AllocSize,
                 ULONG Attributes, ULONG Share, ULONG Disposition,
                 ULONG CreateOptions, ULONG Options)
{
    OBJECT_ATTRIBUTES ntoa;
    UNICODE_STRING name;
    POBJECT_ATTRIBUTES oa = XeTranslateOa(XAttr, &ntoa, &name);
    NTSTATUS status = IoCreateFile(Handle, Access, oa, Iosb, AllocSize,
                                   Attributes, Share, Disposition,
                                   CreateOptions, NULL, 0, CreateFileTypeNone,
                                   NULL, Options);
    XbStraceDbg("IoCreateFile(\"%wZ\", access=%08lx, disp=%lx, options=%lx) -> %08lx\n",
                oa ? oa->ObjectName : &name, Access, Disposition, Options,
                status);
    if (name.Buffer != NULL)
        RtlFreeUnicodeString(&name);
    return status;
}
NTSTATUS NTAPI
XeNtCreateFile(PHANDLE Handle, ACCESS_MASK Access, PXBE_OBJECT_ATTRIBUTES XAttr,
                 PIO_STATUS_BLOCK Iosb, PLARGE_INTEGER AllocSize,
                 ULONG Attributes, ULONG Share, ULONG Disposition,
                 ULONG Options)
{
    OBJECT_ATTRIBUTES ntoa;
    UNICODE_STRING name;
    POBJECT_ATTRIBUTES oa = XeTranslateOa(XAttr, &ntoa, &name);
    NTSTATUS status = NtCreateFile(Handle, Access, oa, Iosb, AllocSize,
                                   Attributes, Share, Disposition, Options,
                                   NULL, 0);
    XbStraceDbg("NtCreateFile(\"%wZ\", access=%08lx, disp=%lx) -> %08lx\n",
                oa ? oa->ObjectName : &name, Access, Disposition, status);
    if (name.Buffer != NULL)
        RtlFreeUnicodeString(&name);
    return status;
}
NTSTATUS NTAPI
XeNtOpenFile(PHANDLE Handle, ACCESS_MASK Access, PXBE_OBJECT_ATTRIBUTES XAttr,
               PIO_STATUS_BLOCK Iosb, ULONG ShareAccess, ULONG OpenOptions)
{
    OBJECT_ATTRIBUTES ntoa;
    UNICODE_STRING name;
    POBJECT_ATTRIBUTES oa = XeTranslateOa(XAttr, &ntoa, &name);
    NTSTATUS status = NtOpenFile(Handle, Access, oa, Iosb, ShareAccess,
                                 OpenOptions);
    XbStraceDbg("NtOpenFile(\"%wZ\", access=%08lx) -> %08lx\n",
                oa ? oa->ObjectName : &name, Access, status);
    if (name.Buffer != NULL)
        RtlFreeUnicodeString(&name);
    return status;
}

/* Trace wrapper for NtFsControlFile so we can see which FsControlCode the
 * title is using and whether it succeeds. */
extern NTSTATUS NTAPI ros_NtFsControlFile(
    HANDLE, HANDLE, PIO_APC_ROUTINE, PVOID, PIO_STATUS_BLOCK,
    ULONG, PVOID, ULONG, PVOID, ULONG) __asm__("_NtFsControlFile@40");

NTSTATUS NTAPI
XeNtFsControlFile(HANDLE FileHandle, HANDLE Event, PIO_APC_ROUTINE Apc,
                   PVOID ApcCtx, PIO_STATUS_BLOCK Iosb, ULONG FsCtl,
                   PVOID InBuf, ULONG InLen, PVOID OutBuf, ULONG OutLen)
{
    NTSTATUS s = ros_NtFsControlFile(FileHandle, Event, Apc, ApcCtx, Iosb,
                                     FsCtl, InBuf, InLen, OutBuf, OutLen);
    XbStraceDbg("NtFsControlFile(handle=%p, fsctl=%08lx, in=%lu, out=%lu) -> %08lx (iosb.info=%lx)\n",
                FileHandle, FsCtl, InLen, OutLen, s, Iosb ? Iosb->Information : 0);
    return s;
}
NTSTATUS NTAPI
XeNtQueryFullAttributesFile(PXBE_OBJECT_ATTRIBUTES XAttr, PVOID Info)
{
    OBJECT_ATTRIBUTES ntoa;
    UNICODE_STRING name;
    POBJECT_ATTRIBUTES oa = XeTranslateOa(XAttr, &ntoa, &name);
    NTSTATUS status = ros_NtQueryFullAttributesFile(oa, Info);
    if (name.Buffer != NULL)
        RtlFreeUnicodeString(&name);
    return status;
}
NTSTATUS NTAPI
XeNtDeleteFile(PXBE_OBJECT_ATTRIBUTES XAttr)
{
    OBJECT_ATTRIBUTES ntoa;
    UNICODE_STRING name;
    POBJECT_ATTRIBUTES oa = XeTranslateOa(XAttr, &ntoa, &name);
    NTSTATUS status = ros_NtDeleteFile(oa);
    XbStraceDbg("NtDeleteFile(\"%wZ\") -> %08lx\n",
                oa ? oa->ObjectName : &name, status);
    if (name.Buffer != NULL)
        RtlFreeUnicodeString(&name);
    return status;
}
NTSTATUS NTAPI
XeNtReadFile(HANDLE File, HANDLE Event, PIO_APC_ROUTINE Apc, PVOID ApcCtx,
               PIO_STATUS_BLOCK Iosb, PVOID Buffer, ULONG Length,
               PLARGE_INTEGER Offset)
{
    return NtReadFile(File, Event, Apc, ApcCtx, Iosb, Buffer, Length,
                      Offset, NULL);
}
NTSTATUS NTAPI
XeNtWriteFile(HANDLE File, HANDLE Event, PIO_APC_ROUTINE Apc, PVOID ApcCtx,
                PIO_STATUS_BLOCK Iosb, PVOID Buffer, ULONG Length,
                PLARGE_INTEGER Offset)
{
    return NtWriteFile(File, Event, Apc, ApcCtx, Iosb, Buffer, Length, Offset,
                       NULL);
}
/* Honor the explicit Xbox WaitMode so a UserMode wait stays alertable for the
 * title's async-IO completion APCs.  The Zw-forwarded NT path derives
 * KernelMode from PreviousMode and never wakes early. */
NTSTATUS NTAPI
NtWaitForSingleObjectEx(HANDLE Handle, CHAR WaitMode, BOOLEAN Alertable,
                            PLARGE_INTEGER Timeout)
{
    return NxkWaitForSingleObjectMode(Handle, (KPROCESSOR_MODE)WaitMode,
                                      Alertable, Timeout);
}
NTSTATUS NTAPI
NtWaitForMultipleObjectsEx(ULONG Count, PHANDLE Handles, WAIT_TYPE Type,
                               CHAR WaitMode, BOOLEAN Alertable,
                               PLARGE_INTEGER Timeout)
{
    return NxkWaitForMultipleObjectsMode(Count, Handles, Type,
                                         (KPROCESSOR_MODE)WaitMode,
                                         Alertable, Timeout);
}
/*
 * RtlCaptureContext, filling the Xbox public CONTEXT layout (see
 * XBOX_CONTEXT in sdk/lib/rtl/i386/except.c: ContextFlags, 0x204-byte
 * float area, Edi at 0x208 ... SegSs at 0x234).  NT x86 semantics: the
 * caller must have an EBP frame, and the captured context is the
 * caller's state as of its own return -- Ebp = [ebp], Eip = [ebp+4],
 * Esp = ebp+8.  Volatiles are stored as at entry; the float area AND
 * ContextFlags are left untouched (retail-verified: the caller's
 * ContextFlags value survives the call).
 */
__asm__(
    ".text\n"
    ".globl _XeRtlCaptureContext@4\n"
    "_XeRtlCaptureContext@4:\n\t"
    "pushl %edx\n\t"                  /* preserve entry edx */
    "movl 8(%esp), %edx\n\t"          /* ContextRecord */
    "movl %eax, 0x21C(%edx)\n\t"      /* Eax */
    "movl %ecx, 0x218(%edx)\n\t"      /* Ecx */
    "popl %eax\n\t"
    "movl %eax, 0x214(%edx)\n\t"      /* Edx (entry value) */
    "movl %ebx, 0x210(%edx)\n\t"      /* Ebx */
    "movl %esi, 0x20C(%edx)\n\t"      /* Esi */
    "movl %edi, 0x208(%edx)\n\t"      /* Edi */
    "xorl %eax, %eax\n\t"
    "movw %cs, %ax\n\t"
    "movl %eax, 0x228(%edx)\n\t"      /* SegCs */
    "movw %ss, %ax\n\t"
    "movl %eax, 0x234(%edx)\n\t"      /* SegSs */
    "pushfl\n\t"
    "popl %eax\n\t"
    "movl %eax, 0x22C(%edx)\n\t"      /* EFlags */
    "movl (%ebp), %eax\n\t"
    "movl %eax, 0x220(%edx)\n\t"      /* Ebp = [ebp] */
    "movl 4(%ebp), %eax\n\t"
    "movl %eax, 0x224(%edx)\n\t"      /* Eip = [ebp+4] */
    "leal 8(%ebp), %eax\n\t"
    "movl %eax, 0x230(%edx)\n\t"      /* Esp = ebp+8 */
    "ret $4\n");

NTSTATUS NTAPI
NtSignalAndWaitForSingleObjectEx(HANDLE SignalHandle, HANDLE WaitHandle,
                                     CHAR WaitMode, BOOLEAN Alertable,
                                     PLARGE_INTEGER Timeout)
{
    return NxkSignalAndWaitForSingleObjectMode(SignalHandle, WaitHandle,
                                               (KPROCESSOR_MODE)WaitMode,
                                               Alertable, Timeout);
}
/*
 * Titles pass the exported 28-byte Xbox-shape type structs
 * (xb/object-types.c); map them back to the ReactOS internal types.  A
 * pointer that isn't one of the decoys is assumed to already be an
 * internal type (kernel-internal callers).
 */
static POBJECT_TYPE
XeObjectTypeToInternal(PVOID Type)
{
    if (Type == &XeExEventObjectType)     return ExEventObjectType;
    if (Type == &XeExSemaphoreObjectType) return ExSemaphoreObjectType;
    if (Type == &XeExMutantObjectType)    return ExMutantObjectType;
    if (Type == &XeExTimerObjectType)     return ExTimerType;
    if (Type == &XePsThreadObjectType)    return PsThreadType;
    if (Type == &XeIoFileObjectType)      return IoFileObjectType;
    if (Type == &XeIoDeviceObjectType)    return IoDeviceObjectType;
    if (Type == &XeIoCompletionObjectType) return IoCompletionType;
    if (Type == &XeObDirectoryObjectType) return ObpDirectoryObjectType;
    return (POBJECT_TYPE)Type;
}
NTSTATUS NTAPI
XeObReferenceObjectByHandle(HANDLE Handle, POBJECT_TYPE Type, PVOID *Object)
{
    /* A title's own objects reach the handle table through a shim, so
     * once any exist every handle may be one. */
    if (XobHasTitleObjects())
    {
        PVOID Shim;

        if (NT_SUCCESS(ObReferenceObjectByHandle(Handle, 0,
                                                 XobShimObjectType(),
                                                 KernelMode, &Shim, NULL)))
            return XobUnwrapShim(Shim, Type, Object);

        /* Nothing but a title object can be of a title's type, and the
         * lookup below would take that pointer for one of ntoskrnl's. */
        if (XobIsTitleType(Type))
        {
            *Object = NULL;
            return STATUS_OBJECT_TYPE_MISMATCH;
        }
    }
    return ObReferenceObjectByHandle(Handle, 0, XeObjectTypeToInternal(Type),
                                     KernelMode, Object, NULL);
}
/*
 * The pointer-taking pair.  NT treats the requested type as advisory for
 * a kernel-mode caller, and there is no other kind here, so the check is
 * ours to make: the type argument is all that stands between a title and
 * the wrong kind of object.
 */
static BOOLEAN
XeObjectIsOfType(PVOID Object, POBJECT_TYPE Internal)
{
    return Internal == NULL ||
           OBJECT_TO_OBJECT_HEADER(Object)->Type == Internal;
}
NTSTATUS NTAPI
XeObReferenceObjectByPointer(PVOID Object, PVOID Type)
{
    POBJECT_TYPE Internal;

    if (XobHasTitleObjects() && XobIsTitleObject(Object))
    {
        if (Type != NULL && XobObjectType(Object) != Type)
            return STATUS_OBJECT_TYPE_MISMATCH;
        XobReferenceObject(Object);
        return STATUS_SUCCESS;
    }

    Internal = XeObjectTypeToInternal(Type);
    if (!XeObjectIsOfType(Object, Internal))
        return STATUS_OBJECT_TYPE_MISMATCH;
    return ObReferenceObjectByPointer(Object, 0, Internal, KernelMode);
}
/* A refused open reports through the return value alone, so the
 * caller's handle still has to be cleared -- the console leaves nothing
 * of its own there either. */
NTSTATUS NTAPI
XeObOpenObjectByPointer(PVOID Object, PVOID Type, PHANDLE Handle)
{
    POBJECT_TYPE Internal;

    if (XobHasTitleObjects() && XobIsTitleObject(Object))
        return XobOpenTitleObject(Object, Type, Handle);

    Internal = XeObjectTypeToInternal(Type);
    if (!XeObjectIsOfType(Object, Internal))
    {
        *Handle = NULL;
        return STATUS_OBJECT_TYPE_MISMATCH;
    }
    return ObOpenObjectByPointer(Object, 0, NULL, GENERIC_ALL, Internal,
                                 KernelMode, Handle);
}
/*
 * The unqualified reference pair and ObMakeTemporaryObject take a bare
 * object pointer, so each has to know whose object it is before it
 * touches the header in front of it.
 */
VOID FASTCALL
XeObfReferenceObject(PVOID Object)
{
    if (XobHasTitleObjects() && XobIsTitleObject(Object))
        XobReferenceObject(Object);
    else
        ObfReferenceObject(Object);
}
VOID FASTCALL
XeObfDereferenceObject(PVOID Object)
{
    if (XobHasTitleObjects() && XobIsTitleObject(Object))
        XobDereferenceObject(Object);
    else
        ObfDereferenceObject(Object);
}
VOID NTAPI
XeObMakeTemporaryObject(PVOID Object)
{
    if (XobHasTitleObjects() && XobIsTitleObject(Object))
        XobMakeTemporary(Object);
    else
        ObMakeTemporaryObject(Object);
}
NTSTATUS NTAPI
XeObOpenObjectByName(PXBE_OBJECT_ATTRIBUTES XAttr, PVOID Type,
                       PVOID ParseContext, PHANDLE Handle)
{
    OBJECT_ATTRIBUTES ntoa;
    UNICODE_STRING name;
    POBJECT_ATTRIBUTES oa = XeTranslateOa(XAttr, &ntoa, &name);
    NTSTATUS status = ObOpenObjectByName(oa,
                                         XobIsTitleType(Type)
                                             ? XobShimObjectType()
                                             : XeObjectTypeToInternal(Type),
                                         KernelMode, NULL, GENERIC_ALL,
                                         ParseContext, Handle);
    if (name.Buffer != NULL)
        RtlFreeUnicodeString(&name);
    return status;
}
/*
 * Xbox ObReferenceObjectByName takes the path on its own -- there is no
 * root-directory handle -- so only the name rewriting of the shared
 * translator applies.  An unrooted name has nothing to resolve against
 * and the object manager rejects it, matching NT.
 */
NTSTATUS NTAPI
XeObReferenceObjectByName(PANSI_STRING ObjectName, ULONG Attributes,
                            PVOID Type, PVOID ParseContext, PVOID *Object)
{
    XBE_OBJECT_ATTRIBUTES xoa;
    OBJECT_ATTRIBUTES ntoa;
    UNICODE_STRING name;
    POBJECT_ATTRIBUTES oa;
    POBJECT_TYPE Internal;
    NTSTATUS status;

    xoa.RootDirectory = NULL;
    xoa.ObjectName = ObjectName;
    xoa.Attributes = Attributes;

    oa = XeTranslateOa(&xoa, &ntoa, &name);
    if (oa == NULL || oa->ObjectName == NULL)
        return STATUS_OBJECT_NAME_INVALID;

    Internal = XobIsTitleType(Type) ? XobShimObjectType()
                                    : XeObjectTypeToInternal(Type);

    /* The lookup reads its generic mapping straight out of the type it is
     * handed, so it needs a real one even when the caller asked for any
     * type at all.  Which type is immaterial: for a kernel-mode caller the
     * lookup treats it as advisory, and the caller's actual request is
     * enforced below. */
    status = ObReferenceObjectByName(oa->ObjectName, Attributes, NULL,
                                     GENERIC_ALL,
                                     Internal != NULL ? Internal
                                                      : ObpDirectoryObjectType,
                                     KernelMode, ParseContext, Object);
    if (name.Buffer != NULL)
        RtlFreeUnicodeString(&name);

    /* A title object stands in the namespace behind its shim. */
    if (NT_SUCCESS(status) && XobHasTitleObjects() && XobIsShim(*Object))
        return XobUnwrapShim(*Object, Type, Object);

    /* NT treats the requested type as advisory for kernel-mode callers;
     * the console has no user mode, so the type argument is the caller's
     * only guard and has to be enforced here. */
    if (NT_SUCCESS(status) && Internal != NULL &&
        OBJECT_TO_OBJECT_HEADER(*Object)->Type != Internal)
    {
        ObfDereferenceObject(*Object);
        *Object = NULL;
        status = STATUS_OBJECT_TYPE_MISMATCH;
    }
    return status;
}
/*
 * Xbox FILE_RENAME_INFORMATION carries the target as an ANSI
 * OBJECT_STRING; NT's inlines a counted WCHAR array.  Rebuild the NT
 * shape in pool for rename, pass everything else through untouched.
 */
NTSTATUS NTAPI
XeNtSetInformationFile(HANDLE File, PIO_STATUS_BLOCK Iosb, PVOID Info,
                         ULONG Length, FILE_INFORMATION_CLASS Class)
{
    typedef struct {
        BOOLEAN ReplaceIfExists;
        HANDLE RootDirectory;
        ANSI_STRING FileName;
    } XBE_RENAME;

    if (Class == FileRenameInformation)
    {
        XBE_RENAME *x = Info;
        PFILE_RENAME_INFORMATION nt;
        UNICODE_STRING u;
        ULONG ntlen;
        NTSTATUS status;

        if (Length < sizeof(XBE_RENAME) || x->FileName.Buffer == NULL ||
            x->FileName.Length == 0)
            return STATUS_INVALID_PARAMETER;

        status = RtlAnsiStringToUnicodeString(&u, &x->FileName, TRUE);
        if (!NT_SUCCESS(status))
            return status;

        ntlen = FIELD_OFFSET(FILE_RENAME_INFORMATION, FileName) + u.Length;
        nt = ExAllocatePoolWithTag(PagedPool, ntlen, 'nRbX');
        if (nt == NULL)
        {
            RtlFreeUnicodeString(&u);
            return STATUS_INSUFFICIENT_RESOURCES;
        }
        nt->ReplaceIfExists = x->ReplaceIfExists;
        nt->RootDirectory = x->RootDirectory;
        nt->FileNameLength = u.Length;
        RtlCopyMemory(nt->FileName, u.Buffer, u.Length);

        status = NtSetInformationFile(File, Iosb, nt, ntlen, Class);

        ExFreePoolWithTag(nt, 'nRbX');
        RtlFreeUnicodeString(&u);
        return status;
    }

    return NtSetInformationFile(File, Iosb, Info, Length, Class);
}
/* Xbox returns the link target as ANSI; the ReactOS implementation
 * fills a UNICODE_STRING, so query into a scratch buffer and fold. */
NTSTATUS NTAPI
XeNtQuerySymbolicLinkObject(HANDLE Handle, PANSI_STRING Target,
                              PULONG ReturnedLength)
{
    WCHAR wbuf[128];
    UNICODE_STRING u = { 0, sizeof(wbuf), wbuf };
    NTSTATUS status = ros_NtQuerySymbolicLinkObject(Handle, &u, NULL);
    if (!NT_SUCCESS(status))
        return status;

    ULONG chars = u.Length / sizeof(WCHAR);
    if (ReturnedLength != NULL)
        *ReturnedLength = chars;
    if (Target->MaximumLength < chars)
        return STATUS_BUFFER_TOO_SMALL;

    ANSI_STRING a = { 0, Target->MaximumLength, Target->Buffer };
    status = RtlUnicodeStringToAnsiString(&a, &u, FALSE);
    Target->Length = a.Length;
    return status;
}
NTSTATUS NTAPI
XeNtDuplicateObject(HANDLE Source, PHANDLE Target, ULONG Options)
{
    return ros_NtDuplicateObject(NtCurrentProcess(), Source,
                                 NtCurrentProcess(), Target, 0, 0,
                                 Options | DUPLICATE_SAME_ACCESS);
}
/* The Xbox form takes no desired access: a directory is created wide
 * open, and the handle is the caller's only way back to an unnamed one. */
NTSTATUS NTAPI
XeNtCreateDirectoryObject(PHANDLE Handle, PXBE_OBJECT_ATTRIBUTES XAttr)
{
    OBJECT_ATTRIBUTES ntoa;
    UNICODE_STRING name;
    POBJECT_ATTRIBUTES oa = XeTranslateOa(XAttr, &ntoa, &name);
    NTSTATUS status = ros_NtCreateDirectoryObject(Handle, DIRECTORY_ALL_ACCESS,
                                                  oa);
    if (name.Buffer != NULL)
        RtlFreeUnicodeString(&name);
    return status;
}
NTSTATUS NTAPI
XeNtOpenDirectoryObject(PHANDLE Handle, PXBE_OBJECT_ATTRIBUTES XAttr)
{
    OBJECT_ATTRIBUTES ntoa;
    UNICODE_STRING name;
    POBJECT_ATTRIBUTES oa = XeTranslateOa(XAttr, &ntoa, &name);
    NTSTATUS status = ros_NtOpenDirectoryObject(
        Handle, DIRECTORY_QUERY | DIRECTORY_TRAVERSE, oa);
    if (name.Buffer != NULL)
        RtlFreeUnicodeString(&name);
    return status;
}
NTSTATUS NTAPI
XeNtOpenSymbolicLinkObject(PHANDLE Handle, PXBE_OBJECT_ATTRIBUTES XAttr)
{
    OBJECT_ATTRIBUTES ntoa;
    UNICODE_STRING name;
    POBJECT_ATTRIBUTES oa = XeTranslateOa(XAttr, &ntoa, &name);
    NTSTATUS status = ros_NtOpenSymbolicLinkObject(Handle, SYMBOLIC_LINK_QUERY,
                                                   oa);
    if (name.Buffer != NULL)
        RtlFreeUnicodeString(&name);
    return status;
}
/*
 * Xbox OBJECT_DIRECTORY_INFORMATION is { OBJECT_STRING Name; ULONG Type; }
 * with ANSI in-buffer names; ReactOS fills NT-shape entries ({ UNICODE
 * Name; UNICODE TypeName; }).  Query into a scratch pool buffer and
 * repack: entry headers first (zeroed terminator included), ANSI name
 * data after.  The ANSI repack always fits: it needs at most the same
 * space the NT shape already fit into.  Type is left 0 -- the retail
 * field's encoding is undocumented and titles enumerate by name.
 */
NTSTATUS NTAPI
XeNtQueryDirectoryObject(HANDLE Handle, PVOID Buffer, ULONG Length,
                           BOOLEAN RestartScan, PULONG Context,
                           PULONG ReturnLength)
{
    typedef struct { ANSI_STRING Name; ULONG Type; } XBE_ODI;
    typedef struct { UNICODE_STRING Name; UNICODE_STRING TypeName; } NT_ODI;
    NT_ODI *nt;
    NTSTATUS status;
    ULONG count, i;
    PVOID tmp;

    if (Length < sizeof(XBE_ODI))
        return STATUS_BUFFER_TOO_SMALL;

    tmp = ExAllocatePoolWithTag(PagedPool, Length, 'iDbX');
    if (tmp == NULL)
        return STATUS_INSUFFICIENT_RESOURCES;

    status = ros_NtQueryDirectoryObject(Handle, tmp, Length, FALSE,
                                        RestartScan, Context, ReturnLength);
    if (!NT_SUCCESS(status))
    {
        ExFreePoolWithTag(tmp, 'iDbX');
        return status;
    }

    nt = tmp;
    for (count = 0; nt[count].Name.Length != 0; count++)
        ;

    XBE_ODI *out = Buffer;
    PCHAR data = (PCHAR)&out[count + 1];
    for (i = 0; i < count; i++)
    {
        USHORT chars = nt[i].Name.Length / sizeof(WCHAR);
        ULONG c;
        for (c = 0; c < chars; c++)
            data[c] = (CHAR)nt[i].Name.Buffer[c];
        out[i].Name.Buffer = data;
        out[i].Name.Length = chars;
        out[i].Name.MaximumLength = chars;
        out[i].Type = 0;
        data += chars;
    }
    RtlZeroMemory(&out[count], sizeof(out[count]));

    ExFreePoolWithTag(tmp, 'iDbX');
    if (ReturnLength != NULL)
        *ReturnLength = (ULONG)(data - (PCHAR)Buffer);
    return status;
}

/*
 * Xbox NtQueryDirectoryFile drops NT's ReturnSingleEntry parameter (Xbox
 * always returns a single entry) and switches the mask + returned filename to
 * ANSI (FATX names are ASCII; FILE_DIRECTORY_INFORMATION.FileName is OCHAR
 * with the length in bytes-of-chars).  Widen the mask in, fold the returned
 * name back to ANSI in place out.  The in-place fold is safe: dst[k] lands at
 * byte k while src[k] is read from byte 2k -- a forward copy never clobbers
 * an unread source char.
 */
NTSTATUS NTAPI
XeNtQueryDirectoryFile(HANDLE FileHandle, HANDLE Event,
                         PIO_APC_ROUTINE ApcRoutine, PVOID ApcContext,
                         PIO_STATUS_BLOCK IoStatusBlock, PVOID FileInformation,
                         ULONG Length,
                         FILE_INFORMATION_CLASS FileInformationClass,
                         PANSI_STRING FileName, BOOLEAN RestartScan)
{
    UNICODE_STRING umask;
    PUNICODE_STRING pmask = NULL;
    NTSTATUS status;

    umask.Buffer = NULL;
    if (FileName != NULL && FileName->Buffer != NULL && FileName->Length != 0)
    {
        status = RtlAnsiStringToUnicodeString(&umask, FileName, TRUE);
        if (!NT_SUCCESS(status))
            return status;
        pmask = &umask;
    }

    status = ZwQueryDirectoryFile(FileHandle, Event, ApcRoutine, ApcContext,
                                  IoStatusBlock, FileInformation, Length,
                                  FileInformationClass, TRUE /* single entry */,
                                  pmask, RestartScan);

    if (umask.Buffer != NULL)
        RtlFreeUnicodeString(&umask);

    /* The ANSI fold below must only run once the FSD has actually written
     * the buffer.  For the plain synchronous-call shape (no event, no APC)
     * resolve a pending IRP by waiting on the file handle, NT-style; if
     * the title requested genuinely async completion we cannot fold from
     * here, so leave the buffer alone and say so. */
    if (status == STATUS_PENDING)
    {
        if (Event == NULL && ApcRoutine == NULL)
        {
            ZwWaitForSingleObject(FileHandle, FALSE, NULL);
            status = IoStatusBlock != NULL ? IoStatusBlock->Status
                                           : STATUS_SUCCESS;
        }
        else
        {
            XbDbg("async NtQueryDirectoryFile completes with UTF-16 "
                  "names (ANSI fold not possible)\n");
            return status;
        }
    }

    if (NT_SUCCESS(status) &&
        FileInformationClass != FileDirectoryInformation)
        XbDbg("NtQueryDirectoryFile class %u returns UTF-16 names "
              "(only FileDirectoryInformation is folded)\n",
              FileInformationClass);

    if (NT_SUCCESS(status) &&
        FileInformationClass == FileDirectoryInformation &&
        FileInformation != NULL)
    {
        PFILE_DIRECTORY_INFORMATION di =
            (PFILE_DIRECTORY_INFORMATION)FileInformation;
        ULONG nchars = di->FileNameLength / sizeof(WCHAR);
        PWCH wsrc = di->FileName;
        PCHAR adst = (PCHAR)di->FileName;
        ULONG k;

        for (k = 0; k < nchars; k++)
            adst[k] = (CHAR)wsrc[k];
        di->FileNameLength = nchars;
        if (IoStatusBlock != NULL && IoStatusBlock->Information >= nchars)
            IoStatusBlock->Information -= nchars;
    }

    return status;
}

/*
 * The Xbox object namespace is ANSI: IoCreate/DeleteSymbolicLink take ANSI
 * STRINGs (nxdk's nxMountDrive builds them with RtlInitAnsiString), while
 * ReactOS's take UNICODE_STRINGs.  Convert at the boundary.
 */
NTSTATUS NTAPI
XeIoCreateSymbolicLink(PANSI_STRING LinkName, PANSI_STRING DeviceName)
{
    UNICODE_STRING uLink, uDevice;
    NTSTATUS status;

    status = RtlAnsiStringToUnicodeString(&uLink, LinkName, TRUE);
    if (!NT_SUCCESS(status))
        return status;
    status = RtlAnsiStringToUnicodeString(&uDevice, DeviceName, TRUE);
    if (!NT_SUCCESS(status))
    {
        RtlFreeUnicodeString(&uLink);
        return status;
    }
    status = IoCreateSymbolicLink(&uLink, &uDevice);
    RtlFreeUnicodeString(&uDevice);
    RtlFreeUnicodeString(&uLink);
    return status;
}
NTSTATUS NTAPI
XeIoDeleteSymbolicLink(PANSI_STRING LinkName)
{
    UNICODE_STRING uLink;
    NTSTATUS status;

    status = RtlAnsiStringToUnicodeString(&uLink, LinkName, TRUE);
    if (!NT_SUCCESS(status))
        return status;
    status = IoDeleteSymbolicLink(&uLink);
    RtlFreeUnicodeString(&uLink);
    return status;
}

/* --- mutants -------------------------------------------------------------- *
 * nxdk's C11 mtx (and its libc startup) are recursive mutants: mtx_init
 * calls NtCreateMutant.  Route the create through ReactOS's real
 * ExMutantObjectType so NtClose runs the right Delete procedure
 * (ExpDeleteMutant) over the KMUTANT body. */

extern NTSTATUS NTAPI ros_ObCreateObject(
    KPROCESSOR_MODE, POBJECT_TYPE, POBJECT_ATTRIBUTES, KPROCESSOR_MODE,
    PVOID, ULONG, ULONG, ULONG, PVOID *) __asm__("_ObCreateObject@36");
extern NTSTATUS NTAPI ros_ObInsertObject(
    PVOID, PVOID, ACCESS_MASK, ULONG, PVOID *, PHANDLE)
    __asm__("_ObInsertObject@24");
extern VOID NTAPI ros_KeInitializeMutant(PVOID, BOOLEAN)
    __asm__("_KeInitializeMutant@8");
extern LONG NTAPI ros_KeReleaseMutant(PVOID, LONG, BOOLEAN, BOOLEAN)
    __asm__("_KeReleaseMutant@16");
extern VOID NTAPI ros_KeInitializeSemaphore(PVOID, LONG, LONG)
    __asm__("_KeInitializeSemaphore@12");
extern POBJECT_TYPE ExSemaphoreObjectType;

NTSTATUS NTAPI
XeNtCreateMutant(PHANDLE MutantHandle, PXBE_OBJECT_ATTRIBUTES XAttr,
                   BOOLEAN InitialOwner)
{
    OBJECT_ATTRIBUTES ntoa;
    UNICODE_STRING name;
    POBJECT_ATTRIBUTES oa = XeTranslateOa(XAttr, &ntoa, &name);
    PVOID mutant;
    NTSTATUS status;

    status = ros_ObCreateObject(KernelMode, ExMutantObjectType, oa, KernelMode,
                                NULL, sizeof(KMUTANT), 0, 0, &mutant);
    if (NT_SUCCESS(status))
    {
        ros_KeInitializeMutant(mutant, InitialOwner);
        status = ros_ObInsertObject(mutant, NULL, MUTANT_ALL_ACCESS, 0, NULL,
                                    MutantHandle);
    }

    /* Unrooted names are rooted under \Xbox by XeTranslateOa, so the
     * named create now succeeds and same-name creates share one mutant
     * (the anonymous-retry fallback this replaced handed every caller a
     * DISTINCT object -- a lock that excluded nothing). */
    if (!NT_SUCCESS(status))
        XbDbg("NtCreateMutant(named) -> %08lx\n", status);

    if (name.Buffer != NULL)
        RtlFreeUnicodeString(&name);
    return status;
}

NTSTATUS NTAPI
XeNtReleaseMutant(HANDLE MutantHandle, PLONG PreviousCount)
{
    PKMUTANT mutant;
    NTSTATUS status;
    LONG previous;

    status = ObReferenceObjectByHandle(MutantHandle, 0, NULL, KernelMode,
                                       (PVOID *)&mutant, NULL);
    if (!NT_SUCCESS(status))
        return status;

    /* NT's KeReleaseMutant raises STATUS_MUTANT_NOT_OWNED via the
     * unstructured kernel exception model when the caller doesn't own the
     * mutant.  ReactOS's own NtReleaseMutant wraps the call in
     * _SEH2_TRY/_SEH2_EXCEPT(ExSystemExceptionFilter); without that the
     * raise propagates up to the dispatcher and bugchecks (0x7E /
     * c0000046).  Pre-check OwnerThread here to avoid the raise entirely.
     * KMUTANT layout matches Xbox: DispatcherHeader + MutantListEntry +
     * OwnerThread at +0x18. */
    if (mutant->OwnerThread != KeGetCurrentThread())
    {
        ObfDereferenceObject(mutant);
        if (PreviousCount != NULL)
            *PreviousCount = mutant->Header.SignalState;
        return STATUS_MUTANT_NOT_OWNED;
    }

    previous = ros_KeReleaseMutant(mutant, IO_NO_INCREMENT, FALSE, FALSE);
    ObfDereferenceObject(mutant);
    if (PreviousCount != NULL)
        *PreviousCount = previous;
    return STATUS_SUCCESS;
}

/* Xbox NtCreateSemaphore: 4 args (no DesiredAccess), Xbox-flavour
 * OBJECT_ATTRIBUTES.  Build the kernel object and insert it with full
 * access. */
NTSTATUS NTAPI
XeNtCreateSemaphore(PHANDLE SemaphoreHandle, PXBE_OBJECT_ATTRIBUTES XAttr,
                     LONG InitialCount, LONG MaximumCount)
{
    OBJECT_ATTRIBUTES ntoa;
    UNICODE_STRING name;
    POBJECT_ATTRIBUTES oa = XeTranslateOa(XAttr, &ntoa, &name);
    PVOID sem;
    NTSTATUS status;

    if (MaximumCount <= 0 || InitialCount < 0 || InitialCount > MaximumCount)
    {
        if (name.Buffer != NULL)
            RtlFreeUnicodeString(&name);
        return STATUS_INVALID_PARAMETER;
    }

    status = ros_ObCreateObject(KernelMode, ExSemaphoreObjectType, oa,
                                KernelMode, NULL, sizeof(KSEMAPHORE),
                                0, 0, &sem);
    if (NT_SUCCESS(status))
    {
        ros_KeInitializeSemaphore(sem, InitialCount, MaximumCount);
        status = ros_ObInsertObject(sem, NULL, SEMAPHORE_ALL_ACCESS, 0, NULL,
                                    SemaphoreHandle);
    }

    /* Same named-object fallback as XeNtCreateMutant -- titles create
     * named semaphores with unrooted strings ("sem_foo") that NT rejects
     * with STATUS_OBJECT_PATH_SYNTAX_BAD; retry anonymous. */
    /* Unrooted names are rooted under \Xbox (see XeTranslateOa); no
     * anonymous fallback -- it broke same-name sharing. */

    if (name.Buffer != NULL)
        RtlFreeUnicodeString(&name);
    return status;
}

/* Read an EEPROM setting.  No EEPROM/SMC subsystem yet -- return zeros with
 * the requested length reported back (asserted by libcs seeding rand_s
 * with the 256-byte whole-image read at ValueIndex 0xFFFF). */
NTSTATUS NTAPI
ExQueryNonVolatileSetting(ULONG ValueIndex, PULONG Type, PVOID Value,
                              ULONG ValueLength, PULONG ResultLength)
{
    UNREFERENCED_PARAMETER(ValueIndex);

    if (Value != NULL && ValueLength != 0)
        RtlZeroMemory(Value, ValueLength);
    if (Type != NULL)
        *Type = 0;
    if (ResultLength != NULL)
        *ResultLength = ValueLength;
    return STATUS_SUCCESS;
}

/* Defer to the HAL's Type-1 PCI config driver; it handles arbitrary offset
 * and length and serialises on HalpPCIConfigLock. */
VOID NTAPI
HalReadWritePCISpace(ULONG BusNumber, ULONG SlotNumber, ULONG RegisterNumber,
                         PVOID Buffer, ULONG Length, BOOLEAN WritePCISpace)
{
    if (WritePCISpace)
        HalSetBusDataByOffset(PCIConfiguration, BusNumber, SlotNumber,
                              Buffer, RegisterNumber, Length);
    else
        HalGetBusDataByOffset(PCIConfiguration, BusNumber, SlotNumber,
                              Buffer, RegisterNumber, Length);
}

extern NTSTATUS NTAPI ros_NtOpenFile(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES,
                                     PIO_STATUS_BLOCK, ULONG, ULONG)
    __asm__("_NtOpenFile@24");

/*
 * The console takes the device path on its own and reports success for
 * anything that resolves -- a raw partition with nothing mounted and a
 * volume with handles still open included -- so the file system's own
 * answer to the request is not the caller's business.  Only a name that
 * resolves to nothing is reported, as STATUS_OBJECT_NAME_NOT_FOUND.
 */
static NTSTATUS
XeDismountNamedVolume(PUNICODE_STRING Name)
{
    OBJECT_ATTRIBUTES oa;
    IO_STATUS_BLOCK iosb;
    HANDLE handle;
    NTSTATUS status;

    InitializeObjectAttributes(&oa, Name,
                               OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE,
                               NULL, NULL);
    status = ros_NtOpenFile(&handle, SYNCHRONIZE | FILE_READ_ATTRIBUTES, &oa,
                            &iosb,
                            FILE_SHARE_READ | FILE_SHARE_WRITE |
                                FILE_SHARE_DELETE,
                            FILE_SYNCHRONOUS_IO_NONALERT);
    if (!NT_SUCCESS(status)) return status;

    ros_NtFsControlFile(handle, NULL, NULL, NULL, &iosb,
                        FSCTL_DISMOUNT_VOLUME, NULL, 0, NULL, 0);
    NtClose(handle);
    return STATUS_SUCCESS;
}
NTSTATUS NTAPI
XeIoDismountVolumeByName(PANSI_STRING DeviceName)
{
    UNICODE_STRING name;
    NTSTATUS status;

    status = RtlAnsiStringToUnicodeString(&name, DeviceName, TRUE);
    if (!NT_SUCCESS(status)) return status;

    status = XeDismountNamedVolume(&name);
    RtlFreeUnicodeString(&name);
    return status;
}
/*
 * The by-object form dispatches straight into the driver that owns the
 * device: a console file system publishes a dismount entry point in its
 * driver object, and whatever it answers is what the caller gets.
 * Nothing else of the driver object is touched -- a title owns that
 * storage and it ends at the dispatch table.
 *
 * Ours are NT file systems with no such entry point, so for those the
 * request goes down as the FSCTL the by-name form uses, addressed at the
 * device the volume is mounted on.  A device with nothing mounted has
 * nothing to take down and reports success, which is what the console
 * answers for a raw partition.
 */
typedef NTSTATUS (NTAPI *PXBE_DRIVER_DISMOUNTVOLUME)(PDEVICE_OBJECT);

NTSTATUS NTAPI
XeIoDismountVolume(PDEVICE_OBJECT DeviceObject)
{
    UCHAR buffer[sizeof(OBJECT_NAME_INFORMATION) + 128 * sizeof(WCHAR)];
    POBJECT_NAME_INFORMATION name = (POBJECT_NAME_INFORMATION)buffer;
    PDRIVER_OBJECT driver = DeviceObject->DriverObject;
    PDEVICE_OBJECT real;
    ULONG returned;
    NTSTATUS status;

    if (driver != NULL && driver->DriverDismountVolume != NULL)
        return ((PXBE_DRIVER_DISMOUNTVOLUME)driver->DriverDismountVolume)(
                   DeviceObject);

    if (DeviceObject->Vpb == NULL || DeviceObject->Vpb->RealDevice == NULL)
        return STATUS_SUCCESS;
    real = DeviceObject->Vpb->RealDevice;

    status = ObQueryNameString(real, name, sizeof(buffer), &returned);
    if (!NT_SUCCESS(status) || name->Name.Length == 0)
        return STATUS_SUCCESS;
    return XeDismountNamedVolume(&name->Name);
}

extern NTSTATUS NTAPI NtQueryEvent(HANDLE, EVENT_INFORMATION_CLASS, PVOID,
                                   ULONG, PULONG);
extern NTSTATUS NTAPI NtQueryMutant(HANDLE, MUTANT_INFORMATION_CLASS, PVOID,
                                    ULONG, PULONG);
extern NTSTATUS NTAPI NtQuerySemaphore(HANDLE, SEMAPHORE_INFORMATION_CLASS,
                                       PVOID, ULONG, PULONG);

/*
 * Object-state queries.  Each answers exactly one information class --
 * the console has none to choose from -- so the class, the buffer
 * length and the returned length all collapse away.
 */
NTSTATUS NTAPI
XeNtQueryEvent(HANDLE EventHandle, PVOID EventInformation)
{
    return NtQueryEvent(EventHandle, EventBasicInformation, EventInformation,
                        sizeof(EVENT_BASIC_INFORMATION), NULL);
}

NTSTATUS NTAPI
XeNtQueryMutant(HANDLE MutantHandle, PVOID MutantInformation)
{
    return NtQueryMutant(MutantHandle, MutantBasicInformation,
                         MutantInformation,
                         sizeof(MUTANT_BASIC_INFORMATION), NULL);
}

NTSTATUS NTAPI
XeNtQuerySemaphore(HANDLE SemaphoreHandle, PVOID SemaphoreInformation)
{
    return NtQuerySemaphore(SemaphoreHandle, SemaphoreBasicInformation,
                            SemaphoreInformation,
                            sizeof(SEMAPHORE_BASIC_INFORMATION), NULL);
}

extern NTSTATUS NTAPI NtCreateTimer(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES,
                                    TIMER_TYPE);
extern NTSTATUS NTAPI NtQueryTimer(HANDLE, TIMER_INFORMATION_CLASS, PVOID,
                                   ULONG, PULONG);
extern NTSTATUS NTAPI NtSetTimer(HANDLE, PLARGE_INTEGER, PTIMER_APC_ROUTINE,
                                 PVOID, BOOLEAN, LONG, PBOOLEAN);

/*
 * Executive timers.  The console's creator takes no access mask, its setter
 * names the mode the APC is delivered in rather than inheriting the
 * caller's, and its query answers exactly one information class.
 * NtCancelTimer is NT's, unchanged, and is exported directly.
 */
NTSTATUS NTAPI
XeNtCreateTimer(PHANDLE TimerHandle, PXBE_OBJECT_ATTRIBUTES XAttr,
                  TIMER_TYPE TimerType)
{
    OBJECT_ATTRIBUTES ntoa;
    UNICODE_STRING name;
    POBJECT_ATTRIBUTES oa = XeTranslateOa(XAttr, &ntoa, &name);
    NTSTATUS status = NtCreateTimer(TimerHandle, TIMER_ALL_ACCESS, oa,
                                    TimerType);

    if (name.Buffer != NULL)
        RtlFreeUnicodeString(&name);
    return status;
}

/*
 * ApcMode is accepted and cannot be honored: there is no ring 3 to return
 * to, so a UserMode APC would be queued and never run.  Title APCs are
 * delivered as kernel APCs throughout -- the async-IO completion path does
 * the same -- which is what makes the routine run either way.
 */
NTSTATUS NTAPI
XeNtSetTimerEx(HANDLE TimerHandle, PLARGE_INTEGER DueTime,
                 PTIMER_APC_ROUTINE TimerApcRoutine, KPROCESSOR_MODE ApcMode,
                 PVOID TimerContext, BOOLEAN WakeTimer, LONG Period,
                 PBOOLEAN PreviousState)
{
    UNREFERENCED_PARAMETER(ApcMode);
    return NtSetTimer(TimerHandle, DueTime, TimerApcRoutine, TimerContext,
                      WakeTimer, Period, PreviousState);
}

NTSTATUS NTAPI
XeNtQueryTimer(HANDLE TimerHandle, PVOID TimerInformation)
{
    return NtQueryTimer(TimerHandle, TimerBasicInformation, TimerInformation,
                        sizeof(TIMER_BASIC_INFORMATION), NULL);
}

extern NTSTATUS NTAPI IoSetIoCompletion(PVOID, PVOID, PVOID, NTSTATUS,
                                        ULONG_PTR, BOOLEAN);
extern NTSTATUS NTAPI NtCreateIoCompletion(PHANDLE, ACCESS_MASK,
                                           POBJECT_ATTRIBUTES, ULONG);
extern NTSTATUS NTAPI NtQueryIoCompletion(HANDLE,
                                          IO_COMPLETION_INFORMATION_CLASS,
                                          PVOID, ULONG, PULONG);

/*
 * I/O completion ports.  The creator's argument list is NT's, but the
 * attributes are the console's; the query answers exactly one
 * information class.  The setters and the remover are NT's, unchanged,
 * and are exported directly.
 *
 * The access mask is passed through and nothing consults it: titles run
 * in kernel mode, where the object manager skips the access check, which
 * is what makes a port created with no access at all fully usable here
 * exactly as it is on the console.
 */
NTSTATUS NTAPI
XeNtCreateIoCompletion(PHANDLE IoCompletionHandle, ACCESS_MASK DesiredAccess,
                         PXBE_OBJECT_ATTRIBUTES XAttr, ULONG Count)
{
    OBJECT_ATTRIBUTES ntoa;
    UNICODE_STRING name;
    POBJECT_ATTRIBUTES oa = XeTranslateOa(XAttr, &ntoa, &name);
    NTSTATUS status = NtCreateIoCompletion(IoCompletionHandle, DesiredAccess,
                                           oa, Count);

    if (name.Buffer != NULL)
        RtlFreeUnicodeString(&name);
    return status;
}

NTSTATUS NTAPI
XeNtQueryIoCompletion(HANDLE IoCompletionHandle, PVOID IoCompletionInformation)
{
    return NtQueryIoCompletion(IoCompletionHandle, IoCompletionBasicInformation,
                               IoCompletionInformation,
                               sizeof(IO_COMPLETION_BASIC_INFORMATION), NULL);
}

/* The console's setter has no quota argument: a packet is never charged
 * to anyone, so the queue is the only thing that limits it. */
NTSTATUS NTAPI
XeIoSetIoCompletion(PVOID IoCompletion, PVOID KeyContext, PVOID ApcContext,
                      NTSTATUS IoStatus, ULONG_PTR IoStatusInformation)
{
    return IoSetIoCompletion(IoCompletion, KeyContext, ApcContext, IoStatus,
                             IoStatusInformation, FALSE);
}

/* Xbox HalGetInterruptVector takes just an IRQ and yields the IDT vector
 * (plus IRQL); NT's takes a full bus descriptor.  Xbox is one fixed
 * legacy-PIC bus, so bus/vector collapse to the IRQ. */
ULONG NTAPI
XeHalGetInterruptVector(ULONG BusInterruptLevel, PKIRQL Irql)
{
    KAFFINITY Affinity;

    return HalGetInterruptVector(Isa, 0, BusInterruptLevel, BusInterruptLevel,
                                 Irql, &Affinity);
}

/* Accepted but no-op: bring-up exits via HalReturnToFirmware without
 * walking the shutdown-callback chain, so the callback never fires. */
VOID NTAPI
HalRegisterShutdownNotification(PVOID ShutdownRegistration, BOOLEAN Register)
{
    UNREFERENCED_PARAMETER(ShutdownRegistration);
    UNREFERENCED_PARAMETER(Register);
}

/* One-shot SMBus byte/word transaction.  Xbox uses the slave's 8-bit
 * write address (R/W cleared) -- e.g. 0x20 for the SMC, 0xd4 for the Focus
 * FS454 -- while HalpXboxSmBus*Byte/Word take the 7-bit address.  Shift to translate.
 *
 * Data is a ULONG; byte transfers occupy the low 8 bits, word transfers the
 * low 16 bits.  Read calls overwrite *Data with the slave's response. */
NTSTATUS NTAPI
HalReadSMBusValue(UCHAR Address, UCHAR Command, BOOLEAN ReadWord,
                     PULONG DataValue)
{
    UCHAR slave = (UCHAR)(Address >> 1);

    if (DataValue == NULL)
        return STATUS_INVALID_PARAMETER;

    if (ReadWord)
    {
        USHORT word = 0;
        NTSTATUS s = HalpXboxSmBusReadWord(slave, Command, &word);
        if (NT_SUCCESS(s))
            *DataValue = word;
        return s;
    }
    else
    {
        UCHAR byte = 0;
        NTSTATUS s = HalpXboxSmBusReadByte(slave, Command, &byte);
        if (NT_SUCCESS(s))
            *DataValue = byte;
        return s;
    }
}

NTSTATUS NTAPI
HalWriteSMBusValue(UCHAR Address, UCHAR Command, BOOLEAN WriteWord,
                      ULONG DataValue)
{
    UCHAR slave = (UCHAR)(Address >> 1);

    if (WriteWord)
        return HalpXboxSmBusWriteWord(slave, Command, (USHORT)DataValue);
    return HalpXboxSmBusWriteByte(slave, Command, (UCHAR)DataValue);
}

/* Xbox KDPC/KTIMER are the kernel's native layouts on SARCH=xbox (the
 * 28-byte retail KDPC in xdk/ketypes.h; the Inserted-flag queue protocol
 * in ke/dpc.c), so the DPC and timer ordinals map straight to the kernel
 * exports -- no shadow table, no Dpc substitution.
 */

/* --- APCs ------------------------------------------------------------------ *
 *
 * The console's KAPC is 40 bytes and the caller owns the storage: no
 * Size/Spare pair up front, the mode flags packed into the header instead
 * of the tail.  NT's is 48, so nothing NT-shaped may be initialised into a
 * title's buffer.  Initialisation touches no kernel state at all -- it is
 * a pure structure write -- so it is done here against the console layout;
 * the list entry and both system arguments are left as the caller left
 * them, exactly as retail does.
 */

typedef struct _XBE_KAPC
{
    SHORT      Type;
    CHAR       ApcMode;
    UCHAR      Inserted;
    PVOID      Thread;
    LIST_ENTRY ApcListEntry;
    PVOID      KernelRoutine;
    PVOID      RundownRoutine;
    PVOID      NormalRoutine;
    PVOID      NormalContext;
    PVOID      SystemArgument1;
    PVOID      SystemArgument2;
} XBE_KAPC, *PXBE_KAPC;

C_ASSERT(sizeof(XBE_KAPC) == 40);

VOID NTAPI
XeKeInitializeApc(PXBE_KAPC Apc,
                    PVOID Thread,
                    PVOID KernelRoutine,
                    PVOID RundownRoutine,
                    PVOID NormalRoutine,
                    CHAR ApcMode,
                    PVOID NormalContext)
{
    Apc->Type = ApcObject;
    Apc->Inserted = FALSE;
    Apc->Thread = Thread;
    Apc->KernelRoutine = KernelRoutine;
    Apc->RundownRoutine = RundownRoutine;
    Apc->NormalRoutine = NormalRoutine;

    /* No normal routine means nothing to run in user mode: the requested
     * mode is overridden and the context it would have received dropped. */
    if (NormalRoutine != NULL)
    {
        Apc->NormalContext = NormalContext;
        Apc->ApcMode = ApcMode;
    }
    else
    {
        Apc->NormalContext = NULL;
        Apc->ApcMode = KernelMode;
    }
}

/* Insertion is the other half, and it needs a real NT KAPC.  Shadow the
 * title's structure with one allocated at insert time and released when
 * the APC leaves the queue -- delivered or run down -- so an APC that is
 * initialised and never inserted costs nothing.  Both callbacks go
 * through a trampoline: the title's routines expect the pointer the
 * title passed, not the shadow, and the rundown path in ps/kill.c frees
 * the KAPC itself when no rundown routine is registered, which would be
 * the wrong pointer here.
 */

/* KeInitializeApc / KeInsertQueueApc are NDK prototypes, not part of the
 * DDK headers xbe.c includes -- declare them locally. */
NTKERNELAPI
VOID
NTAPI
KeInitializeApc(IN PKAPC Apc,
                IN PKTHREAD Thread,
                IN KAPC_ENVIRONMENT TargetEnvironment,
                IN PKKERNEL_ROUTINE KernelRoutine,
                IN PKRUNDOWN_ROUTINE RundownRoutine OPTIONAL,
                IN PKNORMAL_ROUTINE NormalRoutine OPTIONAL,
                IN KPROCESSOR_MODE Mode OPTIONAL,
                IN PVOID Context OPTIONAL);

NTKERNELAPI
BOOLEAN
NTAPI
KeInsertQueueApc(IN PKAPC Apc,
                 IN PVOID SystemArgument1,
                 IN PVOID SystemArgument2,
                 IN KPRIORITY PriorityBoost);

typedef VOID (NTAPI *PXBE_APC_KERNEL_ROUTINE)(PVOID Apc,
                                              PKNORMAL_ROUTINE *NormalRoutine,
                                              PVOID *NormalContext,
                                              PVOID *SystemArgument1,
                                              PVOID *SystemArgument2);
typedef VOID (NTAPI *PXBE_APC_RUNDOWN_ROUTINE)(PVOID Apc);

typedef struct _XBE_APC_SHADOW
{
    PXBE_KAPC TitleApc;
    KAPC      NtApc;
} XBE_APC_SHADOW, *PXBE_APC_SHADOW;

/* A title reaches a thread object either through KeGetCurrentThread, which
 * hands back the Xbox-shaped shadow embedded in the KTHREAD, or through the
 * object manager, which hands back the KTHREAD itself.  The shadow's first
 * bytes are zeroed, so the dispatcher header tells the two apart. */
static PKTHREAD
XeResolveThread(_In_opt_ PVOID Thread)
{
    PKTHREAD Resolved;

    if (Thread == NULL)
        return NULL;

    if (((PKTHREAD)Thread)->Header.Type == ThreadObject)
        return (PKTHREAD)Thread;

    Resolved = CONTAINING_RECORD(Thread, KTHREAD, XeXboxShadow);
    if (Resolved->Header.Type != ThreadObject)
    {
        XbDbg("APC: %p is neither a thread nor a thread shadow\n", Thread);
        return NULL;
    }
    return Resolved;
}

static VOID NTAPI
XeApcKernelTrampoline(PKAPC Apc,
                        PKNORMAL_ROUTINE *NormalRoutine,
                        PVOID *NormalContext,
                        PVOID *SystemArgument1,
                        PVOID *SystemArgument2)
{
    PXBE_APC_SHADOW Shadow = CONTAINING_RECORD(Apc, XBE_APC_SHADOW, NtApc);
    PXBE_KAPC Title = Shadow->TitleApc;
    PXBE_APC_KERNEL_ROUTINE Routine =
        (PXBE_APC_KERNEL_ROUTINE)Title->KernelRoutine;

    /* The kernel has already dequeued it; mirror that, and the arguments
     * it delivered, into the structure the title can see. */
    Title->Inserted = FALSE;
    Title->ApcListEntry.Flink = NULL;
    Title->ApcListEntry.Blink = NULL;
    Title->SystemArgument1 = *SystemArgument1;
    Title->SystemArgument2 = *SystemArgument2;

    /* Release the shadow before the callback: re-queueing the same KAPC
     * from inside the kernel routine is a legitimate thing to do. */
    ExFreePoolWithTag(Shadow, XBE_TAG);

    if (Routine != NULL)
        Routine(Title, NormalRoutine, NormalContext,
                SystemArgument1, SystemArgument2);
}

static VOID NTAPI
XeApcRundownTrampoline(PKAPC Apc)
{
    PXBE_APC_SHADOW Shadow = CONTAINING_RECORD(Apc, XBE_APC_SHADOW, NtApc);
    PXBE_KAPC Title = Shadow->TitleApc;
    PXBE_APC_RUNDOWN_ROUTINE Routine =
        (PXBE_APC_RUNDOWN_ROUTINE)Title->RundownRoutine;

    Title->Inserted = FALSE;
    Title->ApcListEntry.Flink = NULL;
    Title->ApcListEntry.Blink = NULL;
    ExFreePoolWithTag(Shadow, XBE_TAG);

    if (Routine != NULL)
        Routine(Title);
}

/* An inserted KAPC carries its shadow in the list entry: that is the field
 * retail uses for its own queue linkage while the APC is queued, and it is
 * the only way back to the shadow from the title's structure. */
#define XeApcShadow(Apc)  ((PXBE_APC_SHADOW)(Apc)->ApcListEntry.Flink)

BOOLEAN NTAPI
XeKeInsertQueueApc(PXBE_KAPC Apc,
                     PVOID SystemArgument1,
                     PVOID SystemArgument2,
                     KPRIORITY Increment)
{
    PXBE_APC_SHADOW Shadow;
    PKTHREAD Thread;

    if (Apc == NULL)
        return FALSE;

    /* The console stamps the arguments before it decides, so a refused
     * insert re-arms an APC that is already queued.  Keep the shadow in
     * step with it; at DISPATCH_LEVEL the target thread cannot be running,
     * so the shadow cannot be delivered out from under this. */
    if (Apc->Inserted)
    {
        KIRQL OldIrql;

        KeRaiseIrql(DISPATCH_LEVEL, &OldIrql);
        Apc->SystemArgument1 = SystemArgument1;
        Apc->SystemArgument2 = SystemArgument2;
        if (Apc->Inserted && XeApcShadow(Apc) != NULL)
        {
            XeApcShadow(Apc)->NtApc.SystemArgument1 = SystemArgument1;
            XeApcShadow(Apc)->NtApc.SystemArgument2 = SystemArgument2;
        }
        KeLowerIrql(OldIrql);
        return FALSE;
    }

    Apc->SystemArgument1 = SystemArgument1;
    Apc->SystemArgument2 = SystemArgument2;

    Thread = XeResolveThread(Apc->Thread);
    if (Thread == NULL)
        return FALSE;

    Shadow = ExAllocatePoolWithTag(NonPagedPool, sizeof(*Shadow), XBE_TAG);
    if (Shadow == NULL)
        return FALSE;

    Shadow->TitleApc = Apc;
    KeInitializeApc(&Shadow->NtApc, Thread, CurrentApcEnvironment,
                    XeApcKernelTrampoline, XeApcRundownTrampoline,
                    (PKNORMAL_ROUTINE)Apc->NormalRoutine,
                    (KPROCESSOR_MODE)Apc->ApcMode, Apc->NormalContext);

    /* Both marks have to be in place before the insert: a kernel APC on
     * the running thread is delivered before it returns. */
    Apc->ApcListEntry.Flink = (PLIST_ENTRY)Shadow;
    Apc->ApcListEntry.Blink = (PLIST_ENTRY)Shadow;
    Apc->Inserted = TRUE;

    if (!KeInsertQueueApc(&Shadow->NtApc, SystemArgument1, SystemArgument2,
                          Increment))
    {
        Apc->Inserted = FALSE;
        Apc->ApcListEntry.Flink = NULL;
        Apc->ApcListEntry.Blink = NULL;
        ExFreePoolWithTag(Shadow, XBE_TAG);
        return FALSE;
    }
    return TRUE;
}

/* KeInitializeInterrupt / Connect / Disconnect are NDK (ndk/kefuncs.h)
 * prototypes, not part of the DDK headers xbe.c includes -- declare them
 * locally. */
NTKERNELAPI
VOID
NTAPI
KeInitializeInterrupt(IN PKINTERRUPT Interrupt,
                      IN PKSERVICE_ROUTINE ServiceRoutine,
                      IN PVOID ServiceContext,
                      IN PKSPIN_LOCK SpinLock,
                      IN ULONG Vector,
                      IN KIRQL Irql,
                      IN KIRQL SynchronizeIrql,
                      IN KINTERRUPT_MODE InterruptMode,
                      IN BOOLEAN ShareVector,
                      IN CHAR ProcessorNumber,
                      IN BOOLEAN FloatingSave);
NTKERNELAPI BOOLEAN NTAPI KeConnectInterrupt(IN PKINTERRUPT Interrupt);
NTKERNELAPI BOOLEAN NTAPI KeDisconnectInterrupt(IN PKINTERRUPT Interrupt);

/* KINTERRUPT shadow.  Xbox KINTERRUPT is 112 bytes; NT's is 484 (60-byte
 * header + 424-byte DispatchCode tail).  Letting NT initialise into the
 * caller's buffer writes 372 bytes past the end.  Maintain an NT-shaped
 * buffer per Xbox KINTERRUPT and dispatch the lifecycle through it. */
typedef struct _XBE_INTERRUPT_SHADOW
{
    PKINTERRUPT XboxInterrupt;          /* title's pointer -- lookup key */
    /* 512 bytes is comfortably > NT's sizeof(KINTERRUPT) (484) and a clean
     * paragraph round.  C_ASSERT below pins down the choice. */
    UCHAR       NtInterruptStorage[512];
} XBE_INTERRUPT_SHADOW;

#define XBE_INTERRUPT_SHADOW_SLOTS 8

static XBE_INTERRUPT_SHADOW XeInterruptShadows[XBE_INTERRUPT_SHADOW_SLOTS];

static XBE_INTERRUPT_SHADOW *
XeInterruptGetShadow(PKINTERRUPT XboxInt, BOOLEAN Allocate)
{
    KIRQL OldIrql;
    XBE_INTERRUPT_SHADOW *Result = NULL;
    ULONG i;

    KeRaiseIrql(HIGH_LEVEL, &OldIrql);
    for (i = 0; i < XBE_INTERRUPT_SHADOW_SLOTS; i++)
    {
        if (XeInterruptShadows[i].XboxInterrupt == XboxInt)
        {
            Result = &XeInterruptShadows[i];
            break;
        }
    }
    if (Result == NULL && Allocate)
    {
        for (i = 0; i < XBE_INTERRUPT_SHADOW_SLOTS; i++)
        {
            if (XeInterruptShadows[i].XboxInterrupt == NULL)
            {
                XeInterruptShadows[i].XboxInterrupt = XboxInt;
                RtlZeroMemory(XeInterruptShadows[i].NtInterruptStorage,
                              sizeof(XeInterruptShadows[i].NtInterruptStorage));
                Result = &XeInterruptShadows[i];
                break;
            }
        }
    }
    KeLowerIrql(OldIrql);
    return Result;
}

/* Xbox KeInitializeInterrupt has no caller-supplied spin lock and no SMP
 * params; NT's does.  Pass NULL for the lock (use KINTERRUPT's built-in
 * one), mirror Irql as the synchronize IRQL, pin processor 0 without FP
 * save.  Writes 484 bytes into the *shadow*; the title's buffer is left
 * alone (its address is still the lookup key for Connect/Disconnect). */
VOID NTAPI
XeKeInitializeInterrupt(PKINTERRUPT Interrupt,
                          PKSERVICE_ROUTINE ServiceRoutine,
                          PVOID ServiceContext,
                          ULONG Vector,
                          KIRQL Irql,
                          KINTERRUPT_MODE InterruptMode,
                          BOOLEAN ShareVector)
{
    XBE_INTERRUPT_SHADOW *Shadow = XeInterruptGetShadow(Interrupt, TRUE);
    if (Shadow == NULL)
    {
        XbDbg("KeInitializeInterrupt: shadow table full, KINTERRUPT %p "
                 "ignored\n", Interrupt);
        return;
    }
    KeInitializeInterrupt((PKINTERRUPT)Shadow->NtInterruptStorage,
                          ServiceRoutine, ServiceContext,
                          NULL, Vector, Irql, Irql, InterruptMode,
                          ShareVector, 0, FALSE);
}

/* Title KINTERRUPT leaves ActualLock NULL.  NT's KeSynchronizeExecution
 * derefs Interrupt->ActualLock at +0x1C; KefAcquireSpinLockAtDpcLevel then
 * bugchecks with IRQL_NOT_GREATER_OR_EQUAL.  Route through the shadow's NT
 * KINTERRUPT (stamps ActualLock = &SpinLock) so the lock is real.
 * If no shadow exists, just call the routine directly -- uniprocessor Xbox
 * makes the lock a no-op anyway and titles expect forward progress. */
BOOLEAN NTAPI
XeKeSynchronizeExecution(_In_ PKINTERRUPT Interrupt,
                           _In_ PKSYNCHRONIZE_ROUTINE SynchronizeRoutine,
                           _In_opt_ PVOID SynchronizeContext)
{
    XBE_INTERRUPT_SHADOW *Shadow = XeInterruptGetShadow(Interrupt, FALSE);
    if (Shadow == NULL)
    {
        XbDbg("KeSynchronizeExecution(%p): no shadow; running "
                 "uncontended\n", Interrupt);
        return SynchronizeRoutine(SynchronizeContext);
    }
    return KeSynchronizeExecution((PKINTERRUPT)Shadow->NtInterruptStorage,
                                  SynchronizeRoutine, SynchronizeContext);
}

/* No-shadow means KeInitializeInterrupt was never called for this
 * KINTERRUPT -- a caller bug; log and refuse. */
BOOLEAN NTAPI
XeKeConnectInterrupt(PKINTERRUPT Interrupt)
{
    XBE_INTERRUPT_SHADOW *Shadow = XeInterruptGetShadow(Interrupt, FALSE);
    if (Shadow == NULL)
    {
        XbDbg("KeConnectInterrupt(%p): no shadow; ignored\n", Interrupt);
        return FALSE;
    }
    return KeConnectInterrupt((PKINTERRUPT)Shadow->NtInterruptStorage);
}

BOOLEAN NTAPI
XeKeDisconnectInterrupt(PKINTERRUPT Interrupt)
{
    XBE_INTERRUPT_SHADOW *Shadow = XeInterruptGetShadow(Interrupt, FALSE);
    if (Shadow == NULL)
        return FALSE;
    return KeDisconnectInterrupt((PKINTERRUPT)Shadow->NtInterruptStorage);
}

/* --- data exports --------------------------------------------------------- *
 * Some ordinals are data, not functions: the thunk slot must hold the address
 * of the datum (the title reads through it).  A NULL slot would fault.
 *
 * Other DATA exports that aren't XBE-loader-specific live next to the
 * subsystem they belong to (HalBootSMCVideoMode /
 * HalDisk{Model,Serial}Number / XboxHardwareInfo with the HAL, etc). */

PLAUNCH_DATA_PAGE LaunchDataPage = NULL;

static CHAR XeImageNameBuffer[] = "\\Device\\CdRom0\\default.xbe";
ANSI_STRING XeImageFileName =
    { sizeof(XeImageNameBuffer) - 1, sizeof(XeImageNameBuffer),
      XeImageNameBuffer };

/*
 * NT critical sections live in ntdll and aren't exported from ntoskrnl, but
 * a title's libc startup uses them.  Xbox layout:
 *   +0   DISPATCHER_HEADER Event     (16 B, statically inited {Type=1,Size=4})
 *   +16  LONG  LockCount
 *   +20  LONG  RecursionCount
 *   +24  PVOID OwningThread
 * Minimal recursive spinlock (PASSIVE_LEVEL only).
 */
typedef struct _XBE_CS {
    UCHAR Type;
    UCHAR Absolute;
    UCHAR Size;
    UCHAR Inserted;
    LONG  SignalState;
    LIST_ENTRY WaitListHead;
    LONG  LockCount;
    LONG  RecursionCount;
    PVOID OwningThread;
} XBE_CS;

VOID NTAPI XeRtlInitializeCriticalSection(_Out_ PVOID Cs)
{
    XBE_CS *cs = Cs;
    RtlZeroMemory(cs, sizeof(*cs));
    cs->Size = sizeof(XBE_CS) / sizeof(LONG);  /* match toolchain static init */
    cs->WaitListHead.Flink = &cs->WaitListHead;
    cs->WaitListHead.Blink = &cs->WaitListHead;
    cs->LockCount = -1;
}
VOID NTAPI XeRtlEnterCriticalSection(_Inout_ PVOID Cs)
{
    XBE_CS *cs = Cs;
    PVOID me = KeGetCurrentThread();

    if (cs->OwningThread == me) { cs->RecursionCount++; return; }
    while (InterlockedCompareExchangePointer(&cs->OwningThread, me, NULL) != NULL)
        ;                               /* preempted while we spin */
    cs->RecursionCount = 1;
}
VOID NTAPI XeRtlLeaveCriticalSection(_Inout_ PVOID Cs)
{
    XBE_CS *cs = Cs;
    if (--cs->RecursionCount == 0)
        (void)InterlockedExchangePointer(&cs->OwningThread, NULL);
}
ULONG NTAPI XeRtlTryEnterCriticalSection(_Inout_ PVOID Cs)
{
    XBE_CS *cs = Cs;
    PVOID me = KeGetCurrentThread();

    if (cs->OwningThread == me) { cs->RecursionCount++; return TRUE; }
    if (InterlockedCompareExchangePointer(&cs->OwningThread, me, NULL) == NULL)
    {
        cs->RecursionCount = 1;
        return TRUE;
    }
    return FALSE;
}

/* The AndRegion variants pair the critical section with a critical region:
 * each entry disables kernel-APC delivery, each exit re-enables it, so a
 * recursive acquire/release stays balanced (one region push per enter). */
VOID NTAPI XeRtlEnterCriticalSectionAndRegion(_Inout_ PVOID Cs)
{
    KeEnterCriticalRegion();
    XeRtlEnterCriticalSection(Cs);
}
VOID NTAPI XeRtlLeaveCriticalSectionAndRegion(_Inout_ PVOID Cs)
{
    XeRtlLeaveCriticalSection(Cs);
    KeLeaveCriticalRegion();
}

/* Xbox Interlocked* ordinals are __fastcall (args in ECX/EDX), so the
 * wrappers must be FASTCALL too -- a stdcall wrapper reads stack garbage. */
LONG FASTCALL XeInterlockedCompareExchange(_Inout_ LONG volatile *Destination, _In_ LONG Exchange, _In_ LONG Comperand)
{ return InterlockedCompareExchange(Destination, Exchange, Comperand); }
LONG FASTCALL XeInterlockedIncrement(_Inout_ LONG volatile *V)
{ return InterlockedIncrement(V); }
LONG FASTCALL XeInterlockedDecrement(_Inout_ LONG volatile *V)
{ return InterlockedDecrement(V); }
LONG FASTCALL XeInterlockedExchange(_Inout_ LONG volatile *V, _In_ LONG N)
{ return InterlockedExchange(V, N); }
LONG FASTCALL XeInterlockedExchangeAdd(_Inout_ LONG volatile *Addend, _In_ LONG Value)
{ return InterlockedExchangeAdd(Addend, Value); }

/*
 * Resolve an Xbox kernel ordinal against the kernel image's PE export
 * directory.  Function and DATA ordinals share the same path -- a function
 * thunk gets called through, a DATA thunk gets read through, both want the
 * same address.  An unmapped ordinal resolves to a generated scaffold stub
 * that bugchecks (NXK_UNIMPL) naming itself.
 */
#define XBE_BUGCHECK_UNIMPL  0xE0000001    /* P1 = the ordinal */

extern UCHAR __ImageBase[];                 /* this image -- ntoskrnl.exe */

static PVOID
XeResolveOrdinal(_In_ ULONG Ordinal)
{
    PUCHAR base = __ImageBase;
    PIMAGE_NT_HEADERS nt;
    PIMAGE_EXPORT_DIRECTORY exports;
    PULONG functions;
    ULONG index, rva;

    nt = (PIMAGE_NT_HEADERS)(base + ((PIMAGE_DOS_HEADER)base)->e_lfanew);
    exports = (PIMAGE_EXPORT_DIRECTORY)(base + nt->OptionalHeader.DataDirectory[
                  IMAGE_DIRECTORY_ENTRY_EXPORT].VirtualAddress);
    functions = (PULONG)(base + exports->AddressOfFunctions);

    /* AddressOfFunctions is indexed by (ordinal - Base); a zero RVA marks a
     * gap in the ordinal space.  Either means the kernel does not export it. */
    index = Ordinal - exports->Base;
    rva = (Ordinal >= exports->Base && index < exports->NumberOfFunctions)
        ? functions[index] : 0;
    if (rva == 0)
    {
        XbDbg("*** ordinal %lu not in the kernel export directory "
                 "-- bugcheck\n", Ordinal);
        KeBugCheckEx(XBE_BUGCHECK_UNIMPL, Ordinal, 0, 0, 0);
    }

    return base + rva;
}

/* --- file I/O ------------------------------------------------------------- */

static NTSTATUS
XeOpenFile(
    _In_ PCWSTR Path,
    _Out_ PHANDLE Handle)
{
    UNICODE_STRING name;
    OBJECT_ATTRIBUTES oa;
    IO_STATUS_BLOCK iosb;

    *Handle = NULL;

    RtlInitUnicodeString(&name, Path);
    InitializeObjectAttributes(&oa, &name,
                               OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE,
                               NULL, NULL);

    /* Open for streaming reads.  The loader pulls the header + each section
     * straight into the freshly-committed image, so we never hold a
     * whole-file copy -- on a bare 64 MB system that buffer (often several
     * MB) is pure pressure at the tightest point of title bring-up. */
    return ZwCreateFile(Handle, GENERIC_READ | SYNCHRONIZE, &oa, &iosb,
                        NULL, FILE_ATTRIBUTE_NORMAL, FILE_SHARE_READ,
                        FILE_OPEN, FILE_SYNCHRONOUS_IO_NONALERT |
                        FILE_NON_DIRECTORY_FILE, NULL, 0);
}

/*
 * Convert a chainload path from the Xbox convention to an NT path.  nxdk
 * XLaunchXBEEx leaves szLaunchPath looking like
 *     \Device\Harddisk0\Partition2\dir;app.xbe
 * with `;` as the separator before the XBE filename; restore the `\` and
 * widen to WCHAR.  Returns FALSE if the path doesn't fit in Dst (including
 * the NUL) -- the caller treats that as "no chainload, do a real reboot".
 */
static BOOLEAN
XeConvertLaunchPath(_In_z_ PCSTR Src, _Out_writes_z_(DstChars) PWSTR Dst,
                     _In_ SIZE_T DstChars)
{
    SIZE_T i;

    if (DstChars == 0)
        return FALSE;
    for (i = 0; Src[i] != '\0'; i++)
    {
        if (i + 1 >= DstChars)
            return FALSE;
        Dst[i] = (Src[i] == ';') ? L'\\' : (WCHAR)(UCHAR)Src[i];
    }
    Dst[i] = L'\0';
    return TRUE;
}

/* --- XBE loader ----------------------------------------------------------- */

static ULONG
XeDecodeAddr(_In_ ULONG Encoded, _In_ ULONG XorRetail, _In_ ULONG XorDebug,
               _In_ ULONG Base, _In_ ULONG ImageSize)
{
    ULONG addr = Encoded ^ XorRetail;

    /* The retail/debug key pair is told apart by which yields an in-image
     * address (xboxdevwiki).  nxdk titles use the retail keys. */
    if (addr >= Base && addr < Base + ImageSize)
        return addr;
    return Encoded ^ XorDebug;
}

/*
 * Set up the TLS block for the title's *entry* thread.  Spawned threads get
 * theirs from PsCreateSystemThreadEx; but the entry thread runs the nxdk crt0
 * directly, which expects the kernel to have provided its TLS already.  Mirror
 * what nxdk's per-thread startup does: a self-reference word, the initial TLS
 * data copied in, the zero-fill region cleared.  Returns the raw block and,
 * via TlsSizeOut, its size -- the caller points fs:[0x04] at block + size.
 */
static PVOID
XeSetupEntryTls(_In_ PXBE_TLS Tls, _Out_ PSIZE_T TlsSizeOut)
{
    ULONG rawSize = Tls->EndAddressOfRawData - Tls->StartAddressOfRawData;
    ULONG tlsSize = (rawSize + Tls->SizeOfZeroFill + 15) & ~15u;
    PUCHAR block;
    PUCHAR data;

    tlsSize += 4;                       /* self-reference word */
    *TlsSizeOut = tlsSize;
    block = XeAllocTls(tlsSize);       /* base+4 is 16-byte aligned */
    if (block == NULL)
        return NULL;

    RtlZeroMemory(block, tlsSize);
    data = block + 4;
    *(PVOID *)block = data;             /* self-reference */
    if (rawSize != 0)
        RtlCopyMemory(data, (PVOID)(ULONG_PTR)Tls->StartAddressOfRawData,
                      rawSize);

    XbDbg("entry-thread TLS block=%p size=%lx (raw=%lx zero=%lx)\n",
             block, tlsSize, rawSize, Tls->SizeOfZeroFill);
    return block;
}

/* stdcall shim so the title's __cdecl, no-argument XBE entry point can serve
 * as a thread start routine (PKSTART_ROUTINE_X).  StartContext carries the
 * entry address. */
static VOID NTAPI
XeEntryThunk(_In_ PVOID Entry)
{
    ((VOID (__cdecl *)(VOID))(ULONG_PTR)Entry)();
    XbDbg("XBE entry thread returned\n");
}

static NTSTATUS
XeLoadXbe(_In_ HANDLE FileHandle)
{
    XBE_HEADER hdrBuf;
    PXBE_HEADER hdr;
    PXBE_SECTION sections;
    PVOID imageBase;
    SIZE_T imageSize;
    IO_STATUS_BLOCK iosb;
    LARGE_INTEGER off;
    NTSTATUS status;
    ULONG entry, thunkAddr, i;
    PULONG thunk;
    PVOID tlsBlock = NULL;

    /* Read the fixed-size header first so we know the image layout before
     * committing anything. */
    off.QuadPart = 0;
    status = ZwReadFile(FileHandle, NULL, NULL, NULL, &iosb, &hdrBuf,
                        sizeof(hdrBuf), &off, NULL);
    if (!NT_SUCCESS(status) || iosb.Information < sizeof(hdrBuf))
    {
        XbDbg("XBE header read failed (%08lx, got=%lx)\n",
                 status, (ULONG)iosb.Information);
        return NT_SUCCESS(status) ? STATUS_INVALID_IMAGE_FORMAT : status;
    }
    hdr = &hdrBuf;
    if (hdr->Magic != XBE_MAGIC)
    {
        XbDbg("not an XBE (magic %08lx)\n", hdr->Magic);
        return STATUS_INVALID_IMAGE_FORMAT;
    }

    XbDbg("XBE base=%08lx image=%08lx headers=%08lx sections=%lu\n",
             hdr->BaseAddr, hdr->SizeOfImage, hdr->SizeOfHeaders,
             hdr->Sections);

    /* Reserve the whole image at its preferred base; only the headers and
     * PRELOAD sections get committed here.  Non-preload sections stay
     * uncommitted until the title calls XeLoadSection. */
    imageBase = (PVOID)(ULONG_PTR)hdr->BaseAddr;
    imageSize = hdr->SizeOfImage;
    status = XeNtAllocateVirtualMemory(&imageBase, 0, &imageSize,
                                       MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!NT_SUCCESS(status))
    {
        XbDbg("cannot map image at %08lx (status %08lx)\n",
                 hdr->BaseAddr, status);
        return status;
    }
    if ((ULONG_PTR)imageBase != hdr->BaseAddr)
    {
        XbDbg("image landed at %p, not %08lx\n",
                 imageBase, hdr->BaseAddr);
        return STATUS_CONFLICTING_ADDRESSES;
    }
    XeImageRangeStart = (ULONG_PTR)imageBase;
    XeImageRangeEnd = (ULONG_PTR)imageBase + hdr->SizeOfImage;

    imageSize = hdr->SizeOfHeaders;
    status = XeNtAllocateVirtualMemory(&imageBase, 0, &imageSize,
                                       MEM_COMMIT, PAGE_EXECUTE_READWRITE);
    if (!NT_SUCCESS(status))
    {
        XbDbg("cannot commit headers (status %08lx)\n", status);
        return status;
    }

    /* Stream the headers straight into the image, then re-point hdr at the
     * in-image copy (SectionHeadersAddr and the section VAs are BaseAddr-
     * relative, so they only resolve once the headers live at BaseAddr). */
    off.QuadPart = 0;
    status = ZwReadFile(FileHandle, NULL, NULL, NULL, &iosb, imageBase,
                        hdr->SizeOfHeaders, &off, NULL);
    if (!NT_SUCCESS(status))
    {
        XbDbg("XBE headers read failed (%08lx)\n", status);
        return status;
    }
    hdr = (PXBE_HEADER)imageBase;

    /* XeLoadSection re-reads raw data through this handle for the life of
     * the title; the caller no longer closes it. */
    XepImageFileHandle = FileHandle;
    KeInitializeMutex(&XepSectionMutex, 0);

    /* Load the PRELOAD sections through the same path the title's own
     * XeLoadSection calls take (commit + stream + zero-fill + shared-page
     * counters); each ends with SectionRefCount == 1.  Everything else
     * waits for the title. */
    sections = (PXBE_SECTION)(ULONG_PTR)hdr->SectionHeadersAddr;
    for (i = 0; i < hdr->Sections; i++)
    {
        PXBE_SECTION s = &sections[i];
        XbDbg("section %lu  va=%08lx vsize=%08lx raw=%08lx rsize=%08lx%s\n",
                 i, s->VirtualAddr, s->VirtualSize, s->RawAddr, s->SizeOfRaw,
                 (s->Flags & XBE_SECTION_FLAG_PRELOAD) ? "" : "  (not preloaded)");
        if (!(s->Flags & XBE_SECTION_FLAG_PRELOAD))
            continue;
        status = XeLoadSection(s);
        if (!NT_SUCCESS(status))
        {
            XbDbg("section %lu load failed (%08lx)\n", i, status);
            return status;
        }
    }

    /* De-XOR the entry point and the kernel thunk table pointer. */
    entry = XeDecodeAddr(hdr->EntryAddr, XOR_EP_RETAIL, XOR_EP_DEBUG,
                           hdr->BaseAddr, hdr->SizeOfImage);
    thunkAddr = XeDecodeAddr(hdr->KernelThunkAddr, XOR_KT_RETAIL,
                               XOR_KT_DEBUG, hdr->BaseAddr, hdr->SizeOfImage);
    XbDbg("entry=%08lx kernel-thunks=%08lx tls-dir=%08lx\n",
             entry, thunkAddr, hdr->TlsAddr);

    /* Resolve the kernel thunk table: each entry with bit 31 set names an
     * ordinal; overwrite it in place with our implementation's address. */
    if (thunkAddr != 0)
    {
        thunk = (PULONG)(ULONG_PTR)thunkAddr;
        for (i = 0; thunk[i] != 0; i++)
        {
            if (thunk[i] & XBE_THUNK_ORDINAL)
            {
                ULONG ord = thunk[i] & ~XBE_THUNK_ORDINAL;
                /* DATA ordinals must not be wrapped in a strace trampoline:
                 * the title reads through the thunk slot for the *value*
                 * (KeTickCount, LaunchDataPage, ...), not a function pointer.
                 * Wrapping in a trampoline would feed a function prologue
                 * back as data. */
                BOOLEAN isData = FALSE;
                switch (ord) {
                case 16: case 22: case 30: case 31: case 40: case 41:
                case 42: case 64: case 70: case 71: case 88: case 89:
                case 102: case 120: case 154: case 156: case 157: case 162:
                case 164: case 240: case 245: case 249: case 259: case 321:
                case 322: case 323: case 324: case 325: case 326: case 353:
                case 354: case 355: case 356: case 357:
                    isData = TRUE;
                    break;
                }
                if (isData)
                    thunk[i] = (ULONG)(ULONG_PTR)XeResolveOrdinal(ord);
                else
                    thunk[i] = (ULONG)(ULONG_PTR)
                        XbStraceWrap(ord, XeResolveOrdinal(ord));
            }
        }
        XbDbg("resolved %lu kernel thunk(s)\n", i);
    }

    /* The entry thread's TLS isn't stamped here -- the title runs on a
     * dedicated thread below, and XeThreadTrampoline does the per-thread
     * registration. */
    {
        SIZE_T tlsSize = 0;
        if (hdr->TlsAddr != 0)
            tlsBlock = XeSetupEntryTls((PXBE_TLS)(ULONG_PTR)hdr->TlsAddr,
                                         &tlsSize);

        /* Spawn entry on a dedicated thread so the small Phase 1 stack
         * isn't the title's kernel stack -- titles run ring 0 and overflow
         * it.  The thread gets a per-XBE-sized stack from PeStackCommit. */
        XbDbg("spawning XBE entry %08lx on a dedicated thread"
                 " (PeStackCommit=%lx)\n", entry, hdr->PeStackCommit);
        {
            PXBE_THREAD_CTX ctx = ExAllocatePoolWithTag(NonPagedPool,
                                                         sizeof(*ctx), XBE_TAG);
            HANDLE thread;

            if (ctx == NULL)
                return STATUS_INSUFFICIENT_RESOURCES;
            ctx->SystemRoutine = NULL;
            ctx->StartRoutine  = XeEntryThunk;
            ctx->StartContext  = (PVOID)(ULONG_PTR)entry;
            ctx->TlsData       = tlsBlock;
            ctx->TlsSize       = tlsSize;

            status = NxkPsCreateSystemThread(&thread, THREAD_ALL_ACCESS, NULL,
                                             NULL, NULL, XeThreadTrampoline,
                                             ctx, hdr->PeStackCommit, FALSE);
            if (!NT_SUCCESS(status))
            {
                ExFreePoolWithTag(ctx, XBE_TAG);
                XbDbg("title entry thread spawn failed (%08lx)\n",
                         status);
                return status;
            }
            ZwClose(thread);
        }
    }

    return STATUS_SUCCESS;
}

/* --- driver entry --------------------------------------------------------- */


/*
 * Find the initial XBE: walk the retail search order (D:\default.xbe, then
 * C:\xboxdash.xbe) and return the first path that opens.  Returns NULL if
 * nothing opens.
 */
static PCWSTR
XeFindInitialPath(VOID)
{
    HANDLE fh;
    ULONG i;

    for (i = 0; i < RTL_NUMBER_OF(XeTitleSearchPaths); ++i)
    {
        PCWSTR path = XeTitleSearchPaths[i];
        NTSTATUS s = XeOpenFile(path, &fh);
        if (NT_SUCCESS(s))
        {
            ZwClose(fh);
            return path;
        }
        XbDbg("skipping %S (status %08lx)\n", path, s);
    }
    return NULL;
}

/*
 * Reconstruct the chainload path from the persistent slot, if a warm reset
 * planted one.  Clears the slot's magic so the next cold boot doesn't
 * mistakenly re-trigger the same chainload.  Returns the (caller-owned)
 * WCHAR path on hit, NULL on cold boot.
 */
static PCWSTR
XeConsumeChainloadSlot(VOID)
{
    static WCHAR chainloadBuf[520];
    volatile PXB_PERSIST_SLOT slot = (PXB_PERSIST_SLOT)XB_PERSIST_VA;
    ULONG type;

    if (slot->Magic != XB_PERSIST_MAGIC)
        return NULL;          /* cold boot -- caller falls back to search */

    type = slot->Header.dwLaunchDataType;
    slot->Magic = 0;

    /* Rebuild LaunchDataPage for the next image: retail RAM carries the
     * page across the quick reboot, and the relaunched XBE reads the
     * type and payload through XGetLaunchInfo (then frees the page with
     * MmFreeContiguousMemory, so it must come from the contig heap). */
    if (slot->HadLaunchData)
    {
        PVOID NTAPI NxMmAllocateContiguousMemory(IN SIZE_T NumberOfBytes);
        PLAUNCH_DATA_PAGE page =
            NxMmAllocateContiguousMemory(sizeof(LAUNCH_DATA_PAGE));
        if (page != NULL)
        {
            RtlZeroMemory(page, sizeof(*page));
            page->Header = slot->Header;
            RtlCopyMemory(page->LaunchData, (const VOID *)slot->LaunchData,
                          sizeof(page->LaunchData));
            LaunchDataPage = page;
        }
        else
        {
            XbDbg("no contig page for LaunchDataPage; launch data "
                  "dropped\n");
        }
    }

    if (type == LDT_TITLE && slot->Header.szLaunchPath[0] != '\0')
    {
        if (!XeConvertLaunchPath(slot->Header.szLaunchPath, chainloadBuf,
                                  RTL_NUMBER_OF(chainloadBuf)))
        {
            XbDbg("persistent chainload path doesn't fit, ignoring\n");
            return NULL;
        }
        XbDbg("warm reset -- chainloading %S\n", chainloadBuf);
        return chainloadBuf;
    }

    /* Reboot without an explicit chainload target: route to the dashboard
     * rather than re-launching the disc title. */
    XbDbg("warm reset -- booting dashboard\n");
    return L"\\Device\\Harddisk0\\Partition2\\xboxdash.xbe";
}

/*
 * Run the title.  Called from Phase 1 init in place of launching the NT
 * initial process (smss) -- the XBE loader *is* the initial "process".
 *
 * The chainload protocol on retail is a warm platform reset: the title fills
 * LaunchDataPage and calls HalReturnToFirmware(HalQuickRebootRoutine), the
 * SMC resets the CPU without power-cycling, and the kernel re-initialises
 * over preserved RAM.  We do the same: the previous boot's
 * XeHalReturnToFirmware copied the launch header into a fixed-PA slot
 * (XB_PERSIST_*); if the magic is still there, we honour the chained path
 * instead of the disc search.
 */
/*
 * Some titles walk the kernel PE image at the retail base VA 0x80010000:
 * read e_lfanew at +0x3C, follow it to the NT headers, then compare the
 * last section's Name to 'INIT' to build a far-call descriptor aliasing
 * that section.  When the kernel links at a higher base, VA 0x80010000 is
 * uninitialised RAM, so the walk hits stale fill and AVs.  Paint a
 * minimal PE-shaped stub there whose only section Name is not 'INIT', so
 * the walk reads defined bytes and the compare harmlessly fails.
 * NXK_LOW_RECLAIM_FLOOR excludes this page from the title heap so the
 * painted bytes stay stable for the title's life.
 */
static VOID
XeStubKernelHeader(VOID)
{
    PUCHAR base = (PUCHAR)0x80010000;

    /* Linked at the retail base: the real PE header lives here. */
    if (NxkIsRetailBase())
        return;

    RtlZeroMemory(base, 0x200);

    /* DOS header: MZ + e_lfanew. */
    base[0x00] = 'M';
    base[0x01] = 'Z';
    *(PULONG)(base + 0x3C) = 0x40;

    /* IMAGE_NT_HEADERS at +0x40. */
    *(PULONG)(base + 0x40) = 0x00004550;     /* "PE\0\0"                     */
    *(PUSHORT)(base + 0x44) = 0x014C;        /* FileHeader.Machine = i386    */
    *(PUSHORT)(base + 0x46) = 1;             /* NumberOfSections             */
    *(PUSHORT)(base + 0x54) = 0xE0;          /* SizeOfOptionalHeader         */
    *(PUSHORT)(base + 0x56) = 0x010E;        /* Characteristics (executable) */

    /* Last (only) section header at base + 0x40 + 0x18 + 0xE0 = 0x138.
     * Title walks to base + SizeOfOptionalHeader + N*40 - 16 -- same offset
     * with N=1.  Name "TEXT\0\0\0\0" so the cmp against 'INIT' fails. */
    RtlCopyMemory(base + 0x138, "TEXT\0\0\0\0", 8);
}

VOID
XeRunInitialTitle(VOID)
{
    PCWSTR path = XeConsumeChainloadSlot();
    HANDLE fh;
    NTSTATUS s;

    XbDbg("XBE loader starting\n");
    XeStubKernelHeader();

    if (path == NULL)
        path = XeFindInitialPath();
    if (path == NULL)
    {
        XbDbg("no bootable XBE found\n");
        return;
    }

    /* Record the launched image for relaunch-current warm resets
     * (device paths are ASCII; plain narrowing is lossless). */
    {
        ULONG i;
        for (i = 0; path[i] != L'\0' &&
                    i + 1 < sizeof(XeRunningTitlePathA); i++)
            XeRunningTitlePathA[i] = (CHAR)path[i];
        XeRunningTitlePathA[i] = '\0';
    }

    s = XeOpenFile(path, &fh);
    if (!NT_SUCCESS(s))
    {
        XbDbg("failed to open %S (%08lx)\n", path, s);
        return;
    }
    XbDbg("opened %S\n", path);
    /* The handle stays open: XeLoadSection streams section raw data from
     * it for the life of the title. */
    XeLoadXbe(fh);
}
