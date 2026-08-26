/*
 * ExInitializeReadWriteLock / ExAcquireReadWriteLock{Exclusive,Shared} /
 * ExReleaseReadWriteLock.
 *
 * The Xbox ERWLOCK is a multiple-reader / single-writer lock whose word
 * starts at -1 and counts owners as it is acquired.  These are all the
 * uncontended, single-threaded transitions -- the only ones a lone thread
 * can exercise without blocking on the writer event / reader semaphore:
 * a fresh lock's field layout, a writer round-trip, a reader round-trip,
 * nested readers, and sequential reuse.  We read the public ERWLOCK
 * fields directly to confirm the owner-count bookkeeping.
 */

#include "../harness.h"

static bool t_init_fields(void)
{
    ERWLOCK lock;
    ExInitializeReadWriteLock(&lock);
    ASSERT_EQ_U32((ULONG)lock.LockCount, (ULONG)-1);
    ASSERT_EQ_U32(lock.WritersWaitingCount, 0);
    ASSERT_EQ_U32(lock.ReadersWaitingCount, 0);
    ASSERT_EQ_U32(lock.ReadersEntryCount, 0);
    return true;
}

static bool t_exclusive_roundtrip(void)
{
    ERWLOCK lock;
    ExInitializeReadWriteLock(&lock);

    ExAcquireReadWriteLockExclusive(&lock);
    ASSERT_EQ_U32((ULONG)lock.LockCount, 0);

    ExReleaseReadWriteLock(&lock);
    ASSERT_EQ_U32((ULONG)lock.LockCount, (ULONG)-1);
    return true;
}

static bool t_shared_roundtrip(void)
{
    ERWLOCK lock;
    ExInitializeReadWriteLock(&lock);

    ExAcquireReadWriteLockShared(&lock);
    ASSERT_EQ_U32((ULONG)lock.LockCount, 0);
    ASSERT_EQ_U32(lock.ReadersEntryCount, 1);

    ExReleaseReadWriteLock(&lock);
    ASSERT_EQ_U32((ULONG)lock.LockCount, (ULONG)-1);
    ASSERT_EQ_U32(lock.ReadersEntryCount, 0);
    return true;
}

static bool t_nested_readers(void)
{
    ERWLOCK lock;
    ExInitializeReadWriteLock(&lock);

    ExAcquireReadWriteLockShared(&lock);
    ExAcquireReadWriteLockShared(&lock);
    ASSERT_EQ_U32((ULONG)lock.LockCount, 1);
    ASSERT_EQ_U32(lock.ReadersEntryCount, 2);

    ExReleaseReadWriteLock(&lock);
    ASSERT_EQ_U32((ULONG)lock.LockCount, 0);
    ASSERT_EQ_U32(lock.ReadersEntryCount, 1);

    ExReleaseReadWriteLock(&lock);
    ASSERT_EQ_U32((ULONG)lock.LockCount, (ULONG)-1);
    ASSERT_EQ_U32(lock.ReadersEntryCount, 0);
    return true;
}

static bool t_sequential_reuse(void)
{
    ERWLOCK lock;
    ExInitializeReadWriteLock(&lock);

    /* Writer, fully released, then reader: the same lock serves both. */
    ExAcquireReadWriteLockExclusive(&lock);
    ExReleaseReadWriteLock(&lock);
    ASSERT_EQ_U32((ULONG)lock.LockCount, (ULONG)-1);

    ExAcquireReadWriteLockShared(&lock);
    ASSERT_EQ_U32(lock.ReadersEntryCount, 1);
    ExReleaseReadWriteLock(&lock);
    ASSERT_EQ_U32((ULONG)lock.LockCount, (ULONG)-1);
    ASSERT_EQ_U32(lock.ReadersEntryCount, 0);
    return true;
}

static const test_entry_t ex_rwlock_entries[] = {
    {"init_fields",        t_init_fields},
    {"exclusive_roundtrip", t_exclusive_roundtrip},
    {"shared_roundtrip",   t_shared_roundtrip},
    {"nested_readers",     t_nested_readers},
    {"sequential_reuse",   t_sequential_reuse},
};

DEFINE_GROUP(ex_rwlock, "ex/rwlock");
