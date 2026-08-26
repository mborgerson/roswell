/*
 * Xbox executive read/write lock (ERWLOCK).
 *
 * A lightweight multiple-reader / single-writer lock the Xbox kernel
 * exports in place of NT's ERESOURCE.  The lock word starts at -1 (free);
 * every acquire InterlockedIncrements it and every release decrements it,
 * so it doubles as a contention counter.  Writers block on an
 * auto-reset event, readers on a counting semaphore, and both wait
 * queues are drained by the releasing owner while interrupts are off.
 *
 * Not an NT primitive, so there is no ReactOS source to reuse; the
 * behavior mirrors the retail kernel.
 */

#include <ntoskrnl.h>
#define NDEBUG
#include <debug.h>

typedef struct _ERWLOCK {
    LONG LockCount;
    ULONG WritersWaitingCount;
    ULONG ReadersWaitingCount;
    ULONG ReadersEntryCount;
    KEVENT WriterEvent;
    KSEMAPHORE ReaderSemaphore;
} ERWLOCK, *PERWLOCK;

VOID
NTAPI
ExInitializeReadWriteLock(IN PERWLOCK ReadWriteLock)
{
    ReadWriteLock->LockCount = -1;
    ReadWriteLock->WritersWaitingCount = 0;
    ReadWriteLock->ReadersWaitingCount = 0;
    ReadWriteLock->ReadersEntryCount = 0;
    KeInitializeEvent(&ReadWriteLock->WriterEvent, SynchronizationEvent, FALSE);
    KeInitializeSemaphore(&ReadWriteLock->ReaderSemaphore, 0, MAXLONG);
}

VOID
NTAPI
ExAcquireReadWriteLockExclusive(IN PERWLOCK ReadWriteLock)
{
    BOOLEAN Enable = KeDisableInterrupts();

    if (InterlockedIncrement(&ReadWriteLock->LockCount) != 0)
    {
        /* Another owner is active: queue behind it and wait. */
        ReadWriteLock->WritersWaitingCount++;
        KeRestoreInterrupts(Enable);
        KeWaitForSingleObject(&ReadWriteLock->WriterEvent,
                              Executive, KernelMode, FALSE, NULL);
    }
    else
    {
        KeRestoreInterrupts(Enable);
    }
}

VOID
NTAPI
ExAcquireReadWriteLockShared(IN PERWLOCK ReadWriteLock)
{
    BOOLEAN Enable = KeDisableInterrupts();

    /* A reader must wait when a writer holds the lock, or when a writer
     * is queued ahead of it (writers take priority over fresh readers). */
    BOOLEAN MustWait = (ReadWriteLock->ReadersEntryCount == 0) ||
                       (ReadWriteLock->WritersWaitingCount != 0);

    if (InterlockedIncrement(&ReadWriteLock->LockCount) != 0 && MustWait)
    {
        ReadWriteLock->ReadersWaitingCount++;
        KeRestoreInterrupts(Enable);
        KeWaitForSingleObject(&ReadWriteLock->ReaderSemaphore,
                              Executive, KernelMode, FALSE, NULL);
    }
    else
    {
        ReadWriteLock->ReadersEntryCount++;
        KeRestoreInterrupts(Enable);
    }
}

VOID
NTAPI
ExReleaseReadWriteLock(IN PERWLOCK ReadWriteLock)
{
    BOOLEAN Enable = KeDisableInterrupts();

    if (InterlockedDecrement(&ReadWriteLock->LockCount) == -1)
    {
        /* Last owner out: the lock is free. */
        ReadWriteLock->ReadersEntryCount = 0;
        KeRestoreInterrupts(Enable);
        return;
    }

    if (ReadWriteLock->ReadersEntryCount == 0)
    {
        /* A writer just released; wake any waiting readers as a batch. */
        if (ReadWriteLock->ReadersWaitingCount != 0)
        {
            ULONG Waiting = ReadWriteLock->ReadersWaitingCount;
            ReadWriteLock->ReadersEntryCount = Waiting;
            ReadWriteLock->ReadersWaitingCount = 0;
            KeRestoreInterrupts(Enable);
            KeReleaseSemaphore(&ReadWriteLock->ReaderSemaphore,
                               1, (LONG)Waiting, FALSE);
            return;
        }
    }
    else
    {
        /* A reader released; only the last one hands off to a writer. */
        ReadWriteLock->ReadersEntryCount--;
        if (ReadWriteLock->ReadersEntryCount != 0)
        {
            KeRestoreInterrupts(Enable);
            return;
        }
    }

    /* Hand the lock to the next waiting writer. */
    ReadWriteLock->WritersWaitingCount--;
    KeRestoreInterrupts(Enable);
    KeSetEvent(&ReadWriteLock->WriterEvent, 1, FALSE);
}
