/*
 * Boot-CPU stacks, in their own non-LTO TU so their PAGE_SIZE alignment
 * packs at the front of .bss (the linker script sorts input sections by
 * alignment) instead of punching alignment holes into whichever LTO
 * partition they would otherwise land in.
 *
 * The double-fault stack is a single page with no guard page below it.
 * It only hosts the #DF and NMI task-gate handlers -- both run straight
 * into the bugcheck path, which fits well inside one page -- and stands
 * in as the DpcStack placeholder until KiInitializeKernel allocates the
 * real one, before interrupts are enabled.  A guard page would cost a
 * resident page to protect a stack that is only ever entered on the way
 * to a bugcheck, so it is omitted here (see kiinit.c, which skips the
 * read-only marking for this stack on Xbox).
 */

#include <ntoskrnl.h>

#define KI_DOUBLE_FAULT_STACK_SIZE PAGE_SIZE

UCHAR DECLSPEC_ALIGN(PAGE_SIZE) P0BootStackData[KERNEL_STACK_SIZE] = {0};
UCHAR DECLSPEC_ALIGN(PAGE_SIZE) KiDoubleFaultStackData[KI_DOUBLE_FAULT_STACK_SIZE] = {0};
ULONG_PTR P0BootStack = (ULONG_PTR)&P0BootStackData[KERNEL_STACK_SIZE];
ULONG_PTR KiDoubleFaultStack =
    (ULONG_PTR)&KiDoubleFaultStackData[KI_DOUBLE_FAULT_STACK_SIZE];
