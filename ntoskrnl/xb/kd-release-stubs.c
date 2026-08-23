/*
 * KD64 (WinDbg remote debugger protocol) replacement stubs for the
 * SARCH=xbox release build.
 *
 * The kd64 subsystem provides the packet-mode kernel debugger -- KdSendPacket,
 * KdReceivePacket, KdInitSystem, KdpReportExceptionStateChange, etc.
 * In release we never attach a debugger and don't have a serial port
 * connected to WinDbg, so the whole subsystem (~25 KB code + 12 KB
 * persistent buffers) is dead weight.  This file provides just enough
 * Kd* surface to keep ke/, mm/, ps/, and io/ linking when kd64/ is
 * unlinked.  DbgPrint output is silently dropped on release; for
 * traceable runs build with DBG=1.
 */

#include <ntoskrnl.h>
#define NDEBUG
#include <debug.h>

#ifdef SARCH_XBOX
#if !DBG

/* --- Globals ke/ and bug-check paths read --------------------------- */

BOOLEAN KdDebuggerEnabled    = FALSE;
BOOLEAN KdDebuggerNotPresent = TRUE;
BOOLEAN KdBreakAfterSymbolLoad = FALSE;
BOOLEAN KdPitchDebugger       = TRUE;
BOOLEAN KdIgnoreUmExceptions  = FALSE;
ULONG   KdDisableCount        = 0;
ULONG   Kd_DEFAULT_MASK       = 0;

/* The debug-routine vector exception dispatchers call.  Must recognize
 * DbgPrint / Load/UnloadSymbols / CommandString breakpoints (int3 with
 * ExceptionInformation[0] in BREAKPOINT_*) and step past them -- if we
 * blindly return FALSE the kernel re-executes the int3 and recurses
 * until it dies.  This mirrors KdpStub() in kd64/kdtrap.c. */

static BOOLEAN
NTAPI
NxkKdpStub(IN PKTRAP_FRAME TrapFrame,
           IN PKEXCEPTION_FRAME ExceptionFrame,
           IN PEXCEPTION_RECORD ExceptionRecord,
           IN PCONTEXT Context,
           IN KPROCESSOR_MODE PreviousMode,
           IN BOOLEAN SecondChanceException)
{
    if (ExceptionRecord->ExceptionCode == STATUS_BREAKPOINT &&
        ExceptionRecord->NumberParameters > 0)
    {
        ULONG_PTR cmd = ExceptionRecord->ExceptionInformation[0];
        if (cmd == BREAKPOINT_LOAD_SYMBOLS ||
            cmd == BREAKPOINT_UNLOAD_SYMBOLS ||
            cmd == BREAKPOINT_COMMAND_STRING ||
            cmd == BREAKPOINT_PRINT)
        {
            /* Step past the 1-byte int3 -- the kernel uses these as
             * lightweight DbgPrint / module-load notifications, not as
             * real exceptions. */
            KeSetContextPc(Context, KeGetContextPc(Context) + KD_BREAKPOINT_SIZE);
            return TRUE;
        }
    }
    return FALSE;
}

PKDEBUG_ROUTINE KiDebugRoutine = NxkKdpStub;

/* --- Init + enable/disable ----------------------------------------- */

BOOLEAN
NTAPI
KdInitSystem(IN ULONG BootPhase,
             IN PLOADER_PARAMETER_BLOCK LoaderBlock)
{
    return TRUE;
}

VOID
NTAPI
KdUpdateDataBlock(VOID)
{
}

BOOLEAN
NTAPI
KdRefreshDebuggerNotPresent(VOID)
{
    return TRUE;
}

NTSTATUS
NTAPI
KdEnableDebugger(VOID)
{
    return STATUS_DEBUGGER_INACTIVE;
}

NTSTATUS
NTAPI
KdDisableDebugger(VOID)
{
    return STATUS_SUCCESS;
}

NTSTATUS
NTAPI
KdEnableDebuggerWithLock(IN BOOLEAN NeedLock)
{
    return STATUS_DEBUGGER_INACTIVE;
}

NTSTATUS
NTAPI
KdDisableDebuggerWithLock(IN BOOLEAN NeedLock)
{
    return STATUS_SUCCESS;
}

BOOLEAN
NTAPI
KdEnterDebugger(IN PKTRAP_FRAME TrapFrame,
                IN PKEXCEPTION_FRAME ExceptionFrame)
{
    return FALSE;
}

VOID
NTAPI
KdExitDebugger(IN BOOLEAN Enable)
{
}

BOOLEAN
NTAPI
KdPollBreakIn(VOID)
{
    return FALSE;
}

/* --- DbgPrint output dropped silently on release ------------------- */

BOOLEAN
NTAPI
KdpPrintString(IN PSTRING Output)
{
    return FALSE;
}

VOID
__cdecl
KdpDprintf(IN PCSTR Format, ...)
{
}

NTSTATUS
NTAPI
KdpPrint(IN ULONG ComponentId,
         IN ULONG Level,
         IN PCHAR String,
         IN USHORT Length,
         IN KPROCESSOR_MODE PreviousMode,
         IN PKTRAP_FRAME TrapFrame,
         IN PKEXCEPTION_FRAME ExceptionFrame,
         OUT PBOOLEAN Handled)
{
    if (Handled) *Handled = TRUE;
    return STATUS_SUCCESS;
}

USHORT
NTAPI
KdpPrompt(IN PCHAR PromptString,
          IN USHORT PromptLength,
          OUT PCHAR ResponseString,
          IN USHORT MaximumResponseLength,
          IN KPROCESSOR_MODE PreviousMode,
          IN PKTRAP_FRAME TrapFrame,
          IN PKEXCEPTION_FRAME ExceptionFrame)
{
    return 0;
}

/* --- State-change callbacks (called from KeBugCheck + sysldr) ----- */

VOID
NTAPI
KdpReportLoadSymbolsStateChange(IN PSTRING PathName,
                                IN PKD_SYMBOLS_INFO SymbolInfo,
                                IN BOOLEAN UnloadEventRequired,
                                IN OUT PCONTEXT Context)
{
}

BOOLEAN
NTAPI
KdpReportExceptionStateChange(IN PEXCEPTION_RECORD ExceptionRecord,
                              IN OUT PCONTEXT Context,
                              IN BOOLEAN SecondChanceException)
{
    return FALSE;
}

VOID
NTAPI
KdpReportCommandStringStateChange(IN PSTRING NameString,
                                  IN PSTRING CommandString,
                                  IN OUT PCONTEXT Context)
{
}

/* --- KdSystemDebugControl -- called from ex/dbgctrl.c ------------- */

NTSTATUS
NTAPI
KdSystemDebugControl(IN SYSDBG_COMMAND ControlCode,
                     IN OUT PVOID InputBuffer OPTIONAL,
                     IN ULONG InputBufferLength,
                     OUT PVOID OutputBuffer OPTIONAL,
                     IN ULONG OutputBufferLength,
                     OUT PULONG ReturnLength OPTIONAL,
                     IN KPROCESSOR_MODE PreviousMode)
{
    if (ReturnLength) *ReturnLength = 0;
    return STATUS_DEBUGGER_INACTIVE;
}

/* No KdpBreakpointTable / KdpDebuggerLock / KdpContext on Xbox: kd64
 * is unlinked and ke/bug.c's KdDebuggerDataBlock write is already gated
 * under SARCH_XBOX, so nothing actually reads them.  Dropping them
 * removes ~1.3 KB of .bss. */

ULONG
NTAPI
KdpAddBreakpoint(IN PVOID Address)
{
    return 0;
}

BOOLEAN
NTAPI
KdpDeleteBreakpoint(IN ULONG Handle)
{
    return FALSE;
}

VOID
NTAPI
KdSetOwedBreakpoints(VOID)
{
}

VOID
NTAPI
KdpRestoreAllBreakpoints(VOID)
{
}

NTSTATUS
NTAPI
KdChangeOption(IN KD_OPTION Option,
               IN ULONG InBufferBytes OPTIONAL,
               IN PVOID InBuffer,
               IN ULONG OutBufferBytes OPTIONAL,
               OUT PVOID OutBuffer,
               OUT PULONG OutBufferNeeded OPTIONAL)
{
    if (OutBufferNeeded) *OutBufferNeeded = 0;
    return STATUS_DEBUGGER_INACTIVE;
}

NTSTATUS
NTAPI
KdPowerTransition(IN DEVICE_POWER_STATE NewState)
{
    return STATUS_SUCCESS;
}

BOOLEAN
NTAPI
KdIsThisAKdTrap(IN PEXCEPTION_RECORD ExceptionRecord,
                IN PCONTEXT Context,
                IN KPROCESSOR_MODE PreviousMode)
{
    return FALSE;
}

NTSTATUS
NTAPI
NtQueryDebugFilterState(IN ULONG ComponentId,
                        IN ULONG Level)
{
    return FALSE;
}

NTSTATUS
NTAPI
NtSetDebugFilterState(IN ULONG ComponentId,
                      IN ULONG Level,
                      IN BOOLEAN State)
{
    return STATUS_SUCCESS;
}

/* KdAutoEnableOnEvent / KdBlockEnable / KdPrintBufferSize /
 * KdpTimeSlipPending / KdEnteredDebugger / KdVersionBlock /
 * KdDebuggerDataBlock are not referenced on Xbox -- their writers
 * (kd64) are gone and the one reader in ke/bug.c is gated under
 * SARCH_XBOX. */

#endif /* !DBG */
#endif /* SARCH_XBOX */
