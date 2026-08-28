/*
 * PsQueryStatistics -- the two counters the console reports about the
 * running title: how many threads it has and how many handles it holds.
 *
 * The structure is caller-sized and the length is exact; anything else
 * is refused with the caller's buffer left untouched.  Lives apart from
 * the loader because it reads the process object's internals.
 */

#include <ntoskrnl.h>
#define NDEBUG
#include <debug.h>

typedef struct _XBE_PS_STATISTICS
{
    ULONG Length;
    ULONG ThreadCount;
    ULONG HandleCount;
} XBE_PS_STATISTICS, *PXBE_PS_STATISTICS;

NTSTATUS
NTAPI
XePsQueryStatistics(IN OUT PXBE_PS_STATISTICS Statistics)
{
    PEPROCESS Process = PsGetCurrentProcess();

    if (Statistics->Length != sizeof(*Statistics))
        return STATUS_INVALID_PARAMETER;

    Statistics->ThreadCount = Process->ActiveThreads;
    Statistics->HandleCount = ObGetProcessHandleCount(Process);
    return STATUS_SUCCESS;
}
