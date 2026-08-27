/*
 * The Nt object-state queries: event, mutant and semaphore.
 *
 * Each takes a handle and one fixed-shape information buffer -- the
 * console has no information class to choose -- and reports the state
 * a waiter would find, which is the only way a title reads a signal
 * state back out of a handle.
 */

#include "../harness.h"
#include <string.h>

#define GUARD 0x5A5A5A5Au

static bool t_event_reports_type_and_state(void)
{
    EVENT_BASIC_INFORMATION info;
    HANDLE h = NULL;
    NTSTATUS s;

    s = NtCreateEvent(&h, NULL, NotificationEvent, FALSE);
    ASSERT_NTSTATUS(s, STATUS_SUCCESS);

    memset(&info, 0xCC, sizeof(info));
    s = NtQueryEvent(h, &info);
    if (!NT_SUCCESS(s)) {
        NtClose(h);
        FAIL_AND_RETURN("query -> 0x%08x", (unsigned)s);
    }
    if (info.EventType != NotificationEvent || info.EventState != 0) {
        NtClose(h);
        FAIL_AND_RETURN("unsignalled: type=%d state=%d",
                        (int)info.EventType, (int)info.EventState);
    }

    NtSetEvent(h, NULL);
    memset(&info, 0xCC, sizeof(info));
    s = NtQueryEvent(h, &info);
    NtClose(h);
    ASSERT_NTSTATUS(s, STATUS_SUCCESS);
    ASSERT_EQ_U32(info.EventType, NotificationEvent);
    ASSERT_EQ_U32(info.EventState, 1);
    return true;
}

static bool t_event_reports_the_synchronization_type(void)
{
    EVENT_BASIC_INFORMATION info;
    HANDLE h = NULL;
    NTSTATUS s;

    s = NtCreateEvent(&h, NULL, SynchronizationEvent, TRUE);
    ASSERT_NTSTATUS(s, STATUS_SUCCESS);

    memset(&info, 0xCC, sizeof(info));
    s = NtQueryEvent(h, &info);
    NtClose(h);
    ASSERT_NTSTATUS(s, STATUS_SUCCESS);
    ASSERT_EQ_U32(info.EventType, SynchronizationEvent);
    ASSERT_EQ_U32(info.EventState, 1);
    return true;
}

/* A mutant's count runs the other way from a semaphore's: one means
 * free, zero means held once, and it goes negative on recursion. */
static bool t_mutant_reports_ownership(void)
{
    MUTANT_BASIC_INFORMATION info;
    HANDLE h = NULL;
    NTSTATUS s;

    s = NtCreateMutant(&h, NULL, FALSE /* not owned */);
    ASSERT_NTSTATUS(s, STATUS_SUCCESS);

    memset(&info, 0xCC, sizeof(info));
    s = NtQueryMutant(h, &info);
    if (!NT_SUCCESS(s)) {
        NtClose(h);
        FAIL_AND_RETURN("query -> 0x%08x", (unsigned)s);
    }
    if (info.CurrentCount != 1 || info.OwnedByCaller != 0 ||
        info.AbandonedState != 0) {
        NtClose(h);
        FAIL_AND_RETURN("free: count=%d owned=%u abandoned=%u",
                        (int)info.CurrentCount, (unsigned)info.OwnedByCaller,
                        (unsigned)info.AbandonedState);
    }

    /* Take it, then take it again: recursive acquisition drives the
     * count below zero. */
    s = NtWaitForSingleObject(h, FALSE, NULL);
    if (!NT_SUCCESS(s)) {
        NtClose(h);
        FAIL_AND_RETURN("wait -> 0x%08x", (unsigned)s);
    }
    memset(&info, 0xCC, sizeof(info));
    s = NtQueryMutant(h, &info);
    if (!NT_SUCCESS(s) || info.CurrentCount != 0 || info.OwnedByCaller != 1) {
        NtReleaseMutant(h, NULL);
        NtClose(h);
        FAIL_AND_RETURN("held: 0x%08x count=%d owned=%u", (unsigned)s,
                        (int)info.CurrentCount, (unsigned)info.OwnedByCaller);
    }

    NtWaitForSingleObject(h, FALSE, NULL);
    memset(&info, 0xCC, sizeof(info));
    s = NtQueryMutant(h, &info);
    NtReleaseMutant(h, NULL);
    NtReleaseMutant(h, NULL);
    NtClose(h);
    ASSERT_NTSTATUS(s, STATUS_SUCCESS);
    ASSERT_EQ_U32((ULONG)info.CurrentCount, (ULONG)-1);
    ASSERT_EQ_U32(info.OwnedByCaller, 1);
    ASSERT_EQ_U32(info.AbandonedState, 0);
    return true;
}

static bool t_semaphore_reports_both_counts(void)
{
    SEMAPHORE_BASIC_INFORMATION info;
    HANDLE h = NULL;
    NTSTATUS s;

    s = NtCreateSemaphore(&h, NULL, 2, 5);
    ASSERT_NTSTATUS(s, STATUS_SUCCESS);

    memset(&info, 0xCC, sizeof(info));
    s = NtQuerySemaphore(h, &info);
    if (!NT_SUCCESS(s)) {
        NtClose(h);
        FAIL_AND_RETURN("query -> 0x%08x", (unsigned)s);
    }
    if (info.CurrentCount != 2 || info.MaximumCount != 5) {
        NtClose(h);
        FAIL_AND_RETURN("initial: cur=%d max=%d",
                        (int)info.CurrentCount, (int)info.MaximumCount);
    }

    /* One wait consumes a count; the maximum never moves. */
    s = NtWaitForSingleObject(h, FALSE, NULL);
    if (!NT_SUCCESS(s)) {
        NtClose(h);
        FAIL_AND_RETURN("wait -> 0x%08x", (unsigned)s);
    }
    memset(&info, 0xCC, sizeof(info));
    s = NtQuerySemaphore(h, &info);
    NtClose(h);
    ASSERT_NTSTATUS(s, STATUS_SUCCESS);
    ASSERT_EQ_U32(info.CurrentCount, 1);
    ASSERT_EQ_U32(info.MaximumCount, 5);
    return true;
}

/* Each query type-checks its handle. */
static bool t_wrong_object_type_is_refused(void)
{
    EVENT_BASIC_INFORMATION einfo;
    MUTANT_BASIC_INFORMATION minfo;
    HANDLE h = NULL;
    NTSTATUS s, mutant_q, event_q;

    s = NtCreateEvent(&h, NULL, NotificationEvent, FALSE);
    ASSERT_NTSTATUS(s, STATUS_SUCCESS);
    mutant_q = NtQueryMutant(h, &minfo);
    NtClose(h);

    s = NtCreateMutant(&h, NULL, FALSE);
    ASSERT_NTSTATUS(s, STATUS_SUCCESS);
    event_q = NtQueryEvent(h, &einfo);
    NtClose(h);

    ASSERT_NTSTATUS(mutant_q, STATUS_OBJECT_TYPE_MISMATCH);
    ASSERT_NTSTATUS(event_q, STATUS_OBJECT_TYPE_MISMATCH);
    return true;
}

/* Each query writes only its own structure -- the mutant's is six
 * bytes, not the eight its alignment reserves. */
static bool t_writes_only_its_structure(void)
{
    struct { EVENT_BASIC_INFORMATION info; ULONG guard; } e;
    struct { MUTANT_BASIC_INFORMATION info; ULONG guard; } m;
    struct { SEMAPHORE_BASIC_INFORMATION info; ULONG guard; } q;
    HANDLE h = NULL;

    memset(&e, 0xCC, sizeof(e)); e.guard = GUARD;
    if (NT_SUCCESS(NtCreateEvent(&h, NULL, NotificationEvent, FALSE))) {
        NtQueryEvent(h, &e.info);
        NtClose(h);
    }
    memset(&m, 0xCC, sizeof(m)); m.guard = GUARD;
    if (NT_SUCCESS(NtCreateMutant(&h, NULL, FALSE))) {
        NtQueryMutant(h, &m.info);
        NtClose(h);
    }
    memset(&q, 0xCC, sizeof(q)); q.guard = GUARD;
    if (NT_SUCCESS(NtCreateSemaphore(&h, NULL, 1, 1))) {
        NtQuerySemaphore(h, &q.info);
        NtClose(h);
    }
    ASSERT_EQ_U32(e.guard, GUARD);
    ASSERT_EQ_U32(m.guard, GUARD);
    ASSERT_EQ_U32(q.guard, GUARD);
    ASSERT_EQ_U32(((unsigned char *)&m.info)[6], 0xCC);
    ASSERT_EQ_U32(((unsigned char *)&m.info)[7], 0xCC);
    return true;
}

static const test_entry_t ob_query_entries[] = {
    { "event_reports_type_and_state",  t_event_reports_type_and_state,  NULL },
    { "event_reports_the_synchronization_type",
      t_event_reports_the_synchronization_type, NULL },
    { "mutant_reports_ownership",      t_mutant_reports_ownership,      NULL },
    { "semaphore_reports_both_counts", t_semaphore_reports_both_counts, NULL },
    { "wrong_object_type_is_refused",  t_wrong_object_type_is_refused,  NULL },
    { "writes_only_its_structure",     t_writes_only_its_structure,     NULL },
};

DEFINE_GROUP(ob_query, "ob/query");
