/*
 * PROJECT:     nxkrnl (HAL)
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Stub replacement for hal/halx86/generic/x86bios.c.
 *
 * The Xbox console has no PC BIOS -- the MCPX bootrom hands off to the
 * second-stage loader, which jumps straight into
 * kernel code; NV2A graphics is programmed directly with no INT 10h BIOS
 * thunk.  videoprt's int-10 path that calls these routines is not linked
 * into the Xbox kernel at all.
 *
 * To still satisfy the HAL ABI we provide tiny no-op stubs for every entry
 * point the original x86bios.c exported.  Dropping the real file pulls
 * fast486 (~464 KB of x86 emulator) out of the kernel image -- crucial for
 * getting the kernel small enough to fit below the title's hardcoded
 * contig-pool PAs.
 */

#include <hal.h>
#define NDEBUG
#include <debug.h>

BOOLEAN x86BiosIsInitialized = FALSE;
ULONG64 x86BiosBufferPhysical = 0;

VOID
NTAPI
HalInitializeBios(
    _In_ ULONG Phase,
    _In_ PLOADER_PARAMETER_BLOCK LoaderBlock)
{
    UNREFERENCED_PARAMETER(Phase);
    UNREFERENCED_PARAMETER(LoaderBlock);
    /* No-op: Xbox has no PC BIOS to call. */
}

NTSTATUS
NTAPI
x86BiosAllocateBuffer(
    _Inout_ ULONG *Size,
    _Out_ USHORT *Segment,
    _Out_ USHORT *Offset)
{
    UNREFERENCED_PARAMETER(Size);
    *Segment = 0;
    *Offset = 0;
    return STATUS_INSUFFICIENT_RESOURCES;
}

NTSTATUS
NTAPI
x86BiosFreeBuffer(
    _In_ USHORT Segment,
    _In_ USHORT Offset)
{
    UNREFERENCED_PARAMETER(Segment);
    UNREFERENCED_PARAMETER(Offset);
    return STATUS_INVALID_PARAMETER;
}

NTSTATUS
NTAPI
x86BiosReadMemory(
    _In_ USHORT Segment,
    _In_ USHORT Offset,
    _Out_writes_bytes_(Size) PVOID Buffer,
    _In_ ULONG Size)
{
    UNREFERENCED_PARAMETER(Segment);
    UNREFERENCED_PARAMETER(Offset);
    UNREFERENCED_PARAMETER(Buffer);
    UNREFERENCED_PARAMETER(Size);
    return STATUS_NOT_IMPLEMENTED;
}

NTSTATUS
NTAPI
x86BiosWriteMemory(
    _In_ USHORT Segment,
    _In_ USHORT Offset,
    _In_reads_bytes_(Size) PVOID Buffer,
    _In_ ULONG Size)
{
    UNREFERENCED_PARAMETER(Segment);
    UNREFERENCED_PARAMETER(Offset);
    UNREFERENCED_PARAMETER(Buffer);
    UNREFERENCED_PARAMETER(Size);
    return STATUS_NOT_IMPLEMENTED;
}

BOOLEAN
NTAPI
x86BiosCall(
    _In_ ULONG InterruptNumber,
    _Inout_ PX86_BIOS_REGISTERS Registers)
{
    UNREFERENCED_PARAMETER(InterruptNumber);
    UNREFERENCED_PARAMETER(Registers);
    return FALSE;
}

/* generic/trap.S declares TRAP_ENTRY HalpTrap0D, which expects this
 * V86 GPF handler in generic/bios.c.  We don't link bios.c on Xbox --
 * no V86 mode means HalpTrap0D's IDT slot is never installed and this
 * stub is never reached.  Present only to satisfy the asm extern. */
DECLSPEC_NORETURN VOID FASTCALL
HalpTrap0DHandler(IN PKTRAP_FRAME TrapFrame)
{
    UNREFERENCED_PARAMETER(TrapFrame);
    while (TRUE) { }
}

/* halinit.c assigns HalResetDisplay = HalpBiosDisplayReset unconditionally
 * (bootvid/i386/xbox/bootvid.c:ResetDisplay calls it through that pointer
 * with no NULL check).  Xbox has no PC BIOS to invoke -- return FALSE so
 * the original bios.c body's early-return on SARCH_XBOX is preserved. */
BOOLEAN NTAPI
HalpBiosDisplayReset(VOID)
{
    return FALSE;
}
