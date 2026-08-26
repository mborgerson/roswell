/*
 * RtlEnterCriticalSectionAndRegion / RtlLeaveCriticalSectionAndRegion.
 *
 * The AndRegion variants are the plain critical-section enter/leave paired
 * with a critical region (each enter disables kernel-APC delivery, the
 * matching leave re-enables it).  A lone thread can drive the uncontended
 * acquire/release transitions and read back the public bookkeeping.
 *
 * We assert only what is identical on the retail kernel and ours:
 * RecursionCount, and that OwningThread is non-NULL while held and NULL once
 * released.  We do NOT compare OwningThread against KeGetCurrentThread():
 * retail stores the same pointer its exported KeGetCurrentThread returns,
 * whereas our lock records the internal KTHREAD while the exported ordinal
 * hands titles the Xbox thread shadow -- a different address for the same
 * thread.  LockCount's contention encoding also differs and is not checked.
 *
 * Recursive acquire is deliberately not exercised: on the retail kernel a
 * recursive AndRegion enters the critical region on every acquire but leaves
 * it only on the final release, so a nested acquire/release cycle leaks one
 * region level and suppresses kernel-APC delivery for the rest of the run --
 * a corner we cannot observe without corrupting unrelated later cases.
 */

#include "../harness.h"

static bool t_and_region_roundtrip(void)
{
    RTL_CRITICAL_SECTION cs;
    RtlInitializeCriticalSection(&cs);
    ASSERT_EQ_U32((ULONG)cs.RecursionCount, 0);
    ASSERT_TRUE(cs.OwningThread == NULL);

    RtlEnterCriticalSectionAndRegion(&cs);
    ASSERT_EQ_U32((ULONG)cs.RecursionCount, 1);
    ASSERT_NOT_NULL(cs.OwningThread);

    RtlLeaveCriticalSectionAndRegion(&cs);
    ASSERT_EQ_U32((ULONG)cs.RecursionCount, 0);
    ASSERT_TRUE(cs.OwningThread == NULL);
    return true;
}

static bool t_and_region_reuse(void)
{
    RTL_CRITICAL_SECTION cs;
    RtlInitializeCriticalSection(&cs);

    /* The same lock serves back-to-back acquire/release cycles, each of
     * which enters and leaves the critical region exactly once (balanced). */
    for (int i = 0; i < 2; i++)
    {
        RtlEnterCriticalSectionAndRegion(&cs);
        ASSERT_EQ_U32((ULONG)cs.RecursionCount, 1);
        ASSERT_NOT_NULL(cs.OwningThread);
        RtlLeaveCriticalSectionAndRegion(&cs);
        ASSERT_EQ_U32((ULONG)cs.RecursionCount, 0);
        ASSERT_TRUE(cs.OwningThread == NULL);
    }
    return true;
}

/*
 * The plain Enter/Leave/Init/TryEnter variants (no critical region), so
 * unlike the AndRegion pair above they can safely exercise recursion: a
 * nested plain acquire touches only the lock's RecursionCount, never the
 * thread's kernel-APC-disable counter.
 */
static bool t_plain_init_state(void)
{
    RTL_CRITICAL_SECTION cs;
    RtlInitializeCriticalSection(&cs);
    ASSERT_EQ_U32((ULONG)cs.LockCount, (ULONG)-1);
    ASSERT_EQ_U32((ULONG)cs.RecursionCount, 0);
    ASSERT_TRUE(cs.OwningThread == NULL);
    return true;
}

static bool t_plain_roundtrip(void)
{
    RTL_CRITICAL_SECTION cs;
    RtlInitializeCriticalSection(&cs);

    RtlEnterCriticalSection(&cs);
    ASSERT_EQ_U32((ULONG)cs.RecursionCount, 1);
    ASSERT_NOT_NULL(cs.OwningThread);

    RtlLeaveCriticalSection(&cs);
    ASSERT_EQ_U32((ULONG)cs.RecursionCount, 0);
    ASSERT_TRUE(cs.OwningThread == NULL);
    return true;
}

/* Recursive acquire nests RecursionCount and only the final leave frees it. */
static bool t_plain_recursion(void)
{
    RTL_CRITICAL_SECTION cs;
    RtlInitializeCriticalSection(&cs);

    RtlEnterCriticalSection(&cs);
    RtlEnterCriticalSection(&cs);
    RtlEnterCriticalSection(&cs);
    ASSERT_EQ_U32((ULONG)cs.RecursionCount, 3);
    ASSERT_NOT_NULL(cs.OwningThread);

    RtlLeaveCriticalSection(&cs);
    RtlLeaveCriticalSection(&cs);
    ASSERT_EQ_U32((ULONG)cs.RecursionCount, 1);
    ASSERT_NOT_NULL(cs.OwningThread);   /* still held by the outer acquire */

    RtlLeaveCriticalSection(&cs);
    ASSERT_EQ_U32((ULONG)cs.RecursionCount, 0);
    ASSERT_TRUE(cs.OwningThread == NULL);
    return true;
}

/* TryEnter on a free lock acquires it and reports success. */
static bool t_try_enter_free(void)
{
    RTL_CRITICAL_SECTION cs;
    RtlInitializeCriticalSection(&cs);

    ASSERT_TRUE(RtlTryEnterCriticalSection(&cs));
    ASSERT_EQ_U32((ULONG)cs.RecursionCount, 1);
    ASSERT_NOT_NULL(cs.OwningThread);

    RtlLeaveCriticalSection(&cs);
    ASSERT_TRUE(cs.OwningThread == NULL);
    return true;
}

/* TryEnter on a lock this thread already owns nests, like a plain enter. */
static bool t_try_enter_recursive(void)
{
    RTL_CRITICAL_SECTION cs;
    RtlInitializeCriticalSection(&cs);

    RtlEnterCriticalSection(&cs);
    ASSERT_TRUE(RtlTryEnterCriticalSection(&cs));
    ASSERT_EQ_U32((ULONG)cs.RecursionCount, 2);

    RtlLeaveCriticalSection(&cs);
    RtlLeaveCriticalSection(&cs);
    ASSERT_EQ_U32((ULONG)cs.RecursionCount, 0);
    ASSERT_TRUE(cs.OwningThread == NULL);
    return true;
}

static const test_entry_t rtl_critsec_entries[] = {
    {"and_region_roundtrip", t_and_region_roundtrip},
    {"and_region_reuse",     t_and_region_reuse},
    {"plain_init_state",     t_plain_init_state},
    {"plain_roundtrip",      t_plain_roundtrip},
    {"plain_recursion",      t_plain_recursion},
    {"try_enter_free",       t_try_enter_free},
    {"try_enter_recursive",  t_try_enter_recursive},
};

DEFINE_GROUP(rtl_critsec, "rtl/critsec");
