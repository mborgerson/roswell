/*
 * FILE:            ntoskrnl/ke/i386/trap.s
 * COPYRIGHT:       See COPYING in the top level directory
 * PURPOSE:         System Traps, Entrypoints and Exitpoints
 * PROGRAMMER:      Alex Ionescu (alex@relsoft.net)
 *                  Timo Kreuzer (timo.kreuzer@reactos.org)
 * NOTE:            See asmmacro.S for the shared entry/exit code.
 */

/* INCLUDES ******************************************************************/

#include <asm.inc>
#include <ks386.inc>
#include <internal/i386/asmmacro.S>

MACRO(GENERATE_IDT_STUB, Vector)
idt _KiUnexpectedInterrupt&Vector, INT_32_DPL0
ENDM

MACRO(GENERATE_INT_HANDLER, Vector)
//.func KiUnexpectedInterrupt&Vector
_KiUnexpectedInterrupt&Vector:
    /* This is a push instruction with 8bit operand. Since the instruction
       sign extends the value to 32 bits, we need to offset it */
    push (Vector - 128)
    jmp _KiEndUnexpectedRange@0
//.endfunc
ENDM

/* GLOBALS *******************************************************************/

.data
ASSUME CS:nothing

.align 16

PUBLIC _KiIdt
_KiIdt:
/* This is the Software Interrupt Table that we handle in this file:        */
idt _KiTrap00,         INT_32_DPL0  /* INT 00: Divide Error (#DE)           */
idt _KiTrap01,         INT_32_DPL0  /* INT 01: Debug Exception (#DB)        */
idt _KiTrap02,         INT_32_DPL0  /* INT 02: NMI Interrupt                */
idt _KiTrap03,         INT_32_DPL3  /* INT 03: Breakpoint Exception (#BP)   */
idt _KiTrap04,         INT_32_DPL3  /* INT 04: Overflow Exception (#OF)     */
idt _KiTrap05,         INT_32_DPL0  /* INT 05: BOUND Range Exceeded (#BR)   */
idt _KiTrap06,         INT_32_DPL0  /* INT 06: Invalid Opcode Code (#UD)    */
idt _KiTrap07,         INT_32_DPL0  /* INT 07: Device Not Available (#NM)   */
idt _KiTrap08,         INT_32_DPL0  /* INT 08: Double Fault Exception (#DF) */
idt _KiTrap09,         INT_32_DPL0  /* INT 09: RESERVED                     */
idt _KiTrap0A,         INT_32_DPL0  /* INT 0A: Invalid TSS Exception (#TS)  */
idt _KiTrap0B,         INT_32_DPL0  /* INT 0B: Segment Not Present (#NP)    */
idt _KiTrap0C,         INT_32_DPL0  /* INT 0C: Stack Fault Exception (#SS)  */
idt _KiTrap0D,         INT_32_DPL0  /* INT 0D: General Protection (#GP)     */
idt _KiTrap0E,         INT_32_DPL0  /* INT 0E: Page-Fault Exception (#PF)   */
idt _KiTrap0F,         INT_32_DPL0  /* INT 0F: RESERVED                     */
idt _KiTrap10,         INT_32_DPL0  /* INT 10: x87 FPU Error (#MF)          */
idt _KiTrap11,         INT_32_DPL0  /* INT 11: Align Check Exception (#AC)  */
idt _KiTrap0F,         INT_32_DPL0  /* INT 12: Machine Check Exception (#MC)*/
idt _KiTrap0F,         INT_32_DPL0  /* INT 13: SIMD FPU Exception (#XF)     */
REPEAT 21
idt _KiTrap0F,         INT_32_DPL0  /* INT 14-28: UNDEFINED INTERRUPTS      */
ENDR
idt _KiRaiseSecurityCheckFailure, INT_32_DPL3
                                    /* INT 29: Handler for __fastfail       */
#ifdef SARCH_XBOX
/* No ntdll on Xbox; the tick-count and callback-return services are unused. */
idt _KiTrap0F,         INT_32_DPL0  /* INT 2A: unused on Xbox               */
idt _KiTrap0F,         INT_32_DPL0  /* INT 2B: unused on Xbox               */
#else
idt _KiGetTickCount,   INT_32_DPL3  /* INT 2A: Get Tick Count Handler       */
idt _KiCallbackReturn, INT_32_DPL3  /* INT 2B: User-Mode Callback Return    */
#endif
idt _KiRaiseAssertion, INT_32_DPL3  /* INT 2C: Debug Assertion Handler      */
idt _KiDebugService,   INT_32_DPL3  /* INT 2D: Debug Service Handler        */
#ifdef SARCH_XBOX
/* Xbox titles run ring 0; the syscall trap path is unused. */
idt _KiTrap0F,         INT_32_DPL0  /* INT 2E: unused on Xbox               */
#else
idt _KiSystemService,  INT_32_DPL3  /* INT 2E: System Call Service Handler  */
#endif
idt _KiTrap0F,         INT_32_DPL0  /* INT 2F: RESERVED                     */
i = HEX(30)
#ifdef SARCH_XBOX
/* Xbox uses only the 16 PIC IRQ vectors 0x30-0x3F; 0x40-0xFF are unused, so the
 * IDT is 64 entries -- matching the retail kernel (512-byte IDT). */
REPEAT 16
#else
REPEAT 208
#endif
    GENERATE_IDT_STUB %i
    i = i + 1
ENDR

PUBLIC _KiIdtDescriptor
_KiIdtDescriptor:
    .short 0
#ifdef SARCH_XBOX
    .short HEX(1FF)  /* 64 entries x 8 - 1 */
#else
    .short HEX(7FF)
#endif
    .long _KiIdt

PUBLIC _KiUnexpectedEntrySize
_KiUnexpectedEntrySize:
    .long _KiUnexpectedInterrupt49 - _KiUnexpectedInterrupt48

/******************************************************************************/
.code

PUBLIC _KiStartUnexpectedRange@0
_KiStartUnexpectedRange@0:
i = HEX(30)
REPEAT 208
    GENERATE_INT_HANDLER %i
    i = i + 1
ENDR

TRAP_ENTRY KiTrap00, KI_PUSH_FAKE_ERROR_CODE
TRAP_ENTRY KiTrap01, KI_PUSH_FAKE_ERROR_CODE
TASK_ENTRY KiTrap02, KI_NMI
TRAP_ENTRY KiTrap03, KI_PUSH_FAKE_ERROR_CODE
TRAP_ENTRY KiTrap04, KI_PUSH_FAKE_ERROR_CODE
TRAP_ENTRY KiTrap05, KI_PUSH_FAKE_ERROR_CODE
TRAP_ENTRY KiTrap06, KI_PUSH_FAKE_ERROR_CODE
TRAP_ENTRY KiTrap07, KI_PUSH_FAKE_ERROR_CODE
TASK_ENTRY KiTrap08, 0
TRAP_ENTRY KiTrap09, KI_PUSH_FAKE_ERROR_CODE
TRAP_ENTRY KiTrap0A, 0
TRAP_ENTRY KiTrap0B, 0
TRAP_ENTRY KiTrap0C, 0
TRAP_ENTRY KiTrap0D, 0
TRAP_ENTRY KiTrap0E, 0
TRAP_ENTRY KiTrap0F, KI_PUSH_FAKE_ERROR_CODE
TRAP_ENTRY KiTrap10, KI_PUSH_FAKE_ERROR_CODE
TRAP_ENTRY KiTrap11, KI_PUSH_FAKE_ERROR_CODE
TRAP_ENTRY KiTrap13, KI_PUSH_FAKE_ERROR_CODE
TRAP_ENTRY KiRaiseSecurityCheckFailure, KI_PUSH_FAKE_ERROR_CODE
#ifndef SARCH_XBOX
TRAP_ENTRY KiGetTickCount, KI_PUSH_FAKE_ERROR_CODE
TRAP_ENTRY KiCallbackReturn, KI_PUSH_FAKE_ERROR_CODE
#endif
TRAP_ENTRY KiRaiseAssertion, KI_PUSH_FAKE_ERROR_CODE
TRAP_ENTRY KiDebugService, KI_PUSH_FAKE_ERROR_CODE
TRAP_ENTRY KiUnexpectedInterruptTail, 0

ALIGN 4
EXTERN @KiInterruptTemplateHandler@8:PROC
PUBLIC _KiInterruptTemplate
_KiInterruptTemplate:
    CFI_STARTPROC
    KiEnterTrap KI_PUSH_FAKE_ERROR_CODE
PUBLIC _KiInterruptTemplate2ndDispatch
_KiInterruptTemplate2ndDispatch:
    mov edx, 0
PUBLIC _KiInterruptTemplateObject
_KiInterruptTemplateObject:
    mov eax, offset @KiInterruptTemplateHandler@8
    jmp eax
PUBLIC _KiInterruptTemplateDispatch
_KiInterruptTemplateDispatch:
    CFI_ENDPROC

#ifndef SARCH_XBOX
EXTERN @KiSystemServiceHandler@8:PROC
PUBLIC _KiSystemService
.PROC _KiSystemService
    FPO 0, 0, 0, 0, 1, FRAME_TRAP
    KiEnterTrap (KI_PUSH_FAKE_ERROR_CODE OR KI_NONVOLATILES_ONLY OR KI_DONT_SAVE_SEGS)
    KiCallHandler @KiSystemServiceHandler@8
.ENDP

PUBLIC _KiFastCallEntry
.PROC _KiFastCallEntry
    FPO 0, 0, 0, 0, 1, FRAME_TRAP
    KiEnterTrap (KI_FAST_SYSTEM_CALL OR KI_NONVOLATILES_ONLY OR KI_DONT_SAVE_SEGS)
    KiCallHandler @KiSystemServiceHandler@8
.ENDP

PUBLIC _KiFastCallEntryWithSingleStep
.PROC _KiFastCallEntryWithSingleStep
    FPO 0, 0, 0, 0, 1, FRAME_TRAP
    KiEnterTrap (KI_FAST_SYSTEM_CALL OR KI_NONVOLATILES_ONLY OR KI_DONT_SAVE_SEGS)
    or dword ptr [ecx + KTRAP_FRAME_EFLAGS], EFLAGS_TF
    KiCallHandler @KiSystemServiceHandler@8
.ENDP
#endif

PUBLIC _KiEndUnexpectedRange@0
_KiEndUnexpectedRange@0:
    add dword ptr[esp], 128
    jmp _KiUnexpectedInterruptTail


/* EXIT CODE *****************************************************************/

#ifndef SARCH_XBOX
KiTrapExitStub KiSystemCallReturn,        (KI_RESTORE_EAX OR KI_RESTORE_EFLAGS OR KI_EXIT_JMP)
KiTrapExitStub KiSystemCallSysExitReturn, (KI_RESTORE_EAX OR KI_RESTORE_FS OR KI_RESTORE_EFLAGS OR KI_EXIT_SYSCALL)
KiTrapExitStub KiSystemCallTrapReturn,    (KI_RESTORE_EAX OR KI_RESTORE_FS OR KI_EXIT_IRET)
#endif

KiTrapExitStub KiEditedTrapReturn,        (KI_RESTORE_VOLATILES OR KI_RESTORE_EFLAGS OR KI_EDITED_FRAME OR KI_EXIT_RET)
KiTrapExitStub KiTrapReturn,              (KI_RESTORE_VOLATILES OR KI_RESTORE_SEGMENTS OR KI_EXIT_IRET)
KiTrapExitStub KiTrapReturnNoSegments,    (KI_RESTORE_VOLATILES OR KI_EXIT_IRET)
KiTrapExitStub KiTrapReturnNoSegmentsRet8,(KI_RESTORE_VOLATILES OR KI_RESTORE_EFLAGS OR KI_EXIT_RET8)

#ifndef SARCH_XBOX
EXTERN _PsConvertToGuiThread@0:PROC

PUBLIC _KiConvertToGuiThread@0
_KiConvertToGuiThread@0:

    /*
     * Converting to a GUI thread safely updates ESP in-place as well as the
     * current Thread->TrapFrame and EBP when KeSwitchKernelStack is called.
     *
     * However, PsConvertToGuiThread "helpfully" restores EBP to the original
     * caller's value, since it is considered a nonvolatile register. As such,
     * as soon as we're back after the conversion and we try to store the result
     * which will probably be in some stack variable (EBP-based), we'll crash as
     * we are touching the de-allocated non-expanded stack.
     *
     * Thus we need a way to update our EBP before EBP is touched, and the only
     * way to guarantee this is to do the call itself in assembly, use the EAX
     * register to store the result, fixup EBP, and then let the C code continue
     * on its merry way.
     *
     */

    /* Save ebx */
    push ebx

    /* Calculate the stack frame offset in ebx */
    mov ebx, ebp
    sub ebx, esp

    /* Call the worker function */
    call _PsConvertToGuiThread@0

    /* Adjust ebp to the new stack */
    mov ebp, esp
    add ebp, ebx

    /* Restore ebx */
    pop ebx

    /* return to the caller */
    ret

/*
NTSTATUS
NTAPI
KiSystemCallTrampoline(IN PVOID Handler,
                       IN PVOID Arguments,
                       IN ULONG StackBytes);
*/
PUBLIC _KiSystemCallTrampoline@12
_KiSystemCallTrampoline@12:
    push ebp
    mov ebp, esp
    push esi
    push edi

    /* Get handler */
    mov eax, [ebp + 8]
    /* Get arguments */
    mov esi, [ebp + 12]
    /* Get stack bytes */
    mov ecx, [ebp + 16]

    /* Copy args to the stack */
    sub esp, ecx
    mov edi, esp
    shr ecx, 2
    rep movsd

    call eax

    pop edi
    pop esi
    leave

    ret 12
#endif
END
