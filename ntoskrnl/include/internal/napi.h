/*
 * FILE:            ntoskrnl/include/internal/napi.h
 * COPYRIGHT:       GNU GPL, see COPYING in the top level directory
 * PURPOSE:         System Call Table for Native API
 * PROGRAMMER:      Timo Kreuzer
 */

#ifdef SARCH_XBOX

/*
 * Titles run ring 0; the int 0x2E syscall trap is unused (see
 * xb/gen-zw.py).  Empty MainSSDT/MainSSPT keep the link-time
 * references in krnlinit/ex-init from failing without pinning
 * every Nt* through a function pointer.
 */
ULONG_PTR MainSSDT[1] = { 0 };
UCHAR MainSSPT[1] = { 0 };
#define MIN_SYSCALL_NUMBER    0
#define NUMBER_OF_SYSCALLS    0
#define MAX_SYSCALL_NUMBER    0
ULONG MainNumberOfSysCalls = 0;

#else

#define SVC_(name, argcount) (ULONG_PTR)Nt##name,
ULONG_PTR MainSSDT[] = {
#include "sysfuncs.h"
};
#undef SVC_

#define SVC_(name, argcount) argcount * sizeof(void *),
UCHAR MainSSPT[] = {
#include "sysfuncs.h"
};

#define MIN_SYSCALL_NUMBER    0
#define NUMBER_OF_SYSCALLS    (sizeof(MainSSPT) / sizeof(MainSSPT[0]))
#define MAX_SYSCALL_NUMBER    (NUMBER_OF_SYSCALLS - 1)
ULONG MainNumberOfSysCalls = NUMBER_OF_SYSCALLS;

#endif
