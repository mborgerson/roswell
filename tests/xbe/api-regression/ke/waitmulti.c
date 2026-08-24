/*
 * Multi-object waits: KeWaitForMultipleObjects (wait-any index selection,
 * wait-all draining), NtWaitForMultipleObjectsEx over handles, and
 * NtSignalAndWaitForSingleObjectEx's atomic signal+wait.
 *
 * Every KeWaitForMultipleObjects call passes an explicit WaitBlockArray:
 * the retail kernel crashes on NULL at any count -- its KTHREAD carries
 * no builtin multi-wait blocks (NT and ReactOS allow NULL up to three
 * objects).
 */

#include "../harness.h"

#ifndef STATUS_WAIT_1
#define STATUS_WAIT_1 ((NTSTATUS)0x00000001L)
#endif
#ifndef STATUS_WAIT_2
#define STATUS_WAIT_2 ((NTSTATUS)0x00000002L)
#endif

static const LARGE_INTEGER ZERO_TIMEOUT = { .QuadPart = 0 };

/* 10 ms, relative. */
static const LARGE_INTEGER SHORT_TIMEOUT = { .QuadPart = -10 * 10000LL };

static bool t_ke_any_picks_lowest_signaled(void)
{
    KEVENT e0, e1, e2;
    KeInitializeEvent(&e0, SynchronizationEvent, FALSE);
    KeInitializeEvent(&e1, SynchronizationEvent, TRUE);
    KeInitializeEvent(&e2, SynchronizationEvent, TRUE);

    PVOID objs[3] = { &e0, &e1, &e2 };
    KWAIT_BLOCK blocks[3];
    NTSTATUS s = KeWaitForMultipleObjects(3, objs, WaitAny, Executive,
                                          KernelMode, FALSE,
                                          (PLARGE_INTEGER)&ZERO_TIMEOUT,
                                          blocks);
    ASSERT_NTSTATUS(s, STATUS_WAIT_1);

    /* e1 was auto-reset by the satisfied wait; e2 must still be signaled. */
    s = KeWaitForMultipleObjects(3, objs, WaitAny, Executive,
                                 KernelMode, FALSE,
                                 (PLARGE_INTEGER)&ZERO_TIMEOUT, blocks);
    ASSERT_NTSTATUS(s, STATUS_WAIT_2);

    s = KeWaitForMultipleObjects(3, objs, WaitAny, Executive,
                                 KernelMode, FALSE,
                                 (PLARGE_INTEGER)&ZERO_TIMEOUT, blocks);
    ASSERT_NTSTATUS(s, STATUS_TIMEOUT);
    return true;
}

static bool t_ke_all_drains_every_object(void)
{
    KEVENT ev;
    KSEMAPHORE sem;
    KeInitializeEvent(&ev, SynchronizationEvent, TRUE);
    KeInitializeSemaphore(&sem, 1, 2);

    PVOID objs[2] = { &ev, &sem };
    KWAIT_BLOCK blocks[2];
    NTSTATUS s = KeWaitForMultipleObjects(2, objs, WaitAll, Executive,
                                          KernelMode, FALSE,
                                          (PLARGE_INTEGER)&ZERO_TIMEOUT,
                                          blocks);
    ASSERT_NTSTATUS(s, STATUS_SUCCESS);

    /* Both must have been consumed atomically. */
    s = KeWaitForSingleObject(&ev, Executive, KernelMode, FALSE,
                              (PLARGE_INTEGER)&ZERO_TIMEOUT);
    ASSERT_NTSTATUS(s, STATUS_TIMEOUT);
    s = KeWaitForSingleObject(&sem, Executive, KernelMode, FALSE,
                              (PLARGE_INTEGER)&ZERO_TIMEOUT);
    ASSERT_NTSTATUS(s, STATUS_TIMEOUT);
    return true;
}

static bool t_ke_all_unsatisfied_takes_nothing(void)
{
    KEVENT signaled, unsignaled;
    KeInitializeEvent(&signaled, SynchronizationEvent, TRUE);
    KeInitializeEvent(&unsignaled, SynchronizationEvent, FALSE);

    PVOID objs[2] = { &signaled, &unsignaled };
    KWAIT_BLOCK blocks[2];
    NTSTATUS s = KeWaitForMultipleObjects(2, objs, WaitAll, Executive,
                                          KernelMode, FALSE,
                                          (PLARGE_INTEGER)&ZERO_TIMEOUT,
                                          blocks);
    ASSERT_NTSTATUS(s, STATUS_TIMEOUT);

    /* The failed wait-all must not have eaten the signaled event. */
    s = KeWaitForSingleObject(&signaled, Executive, KernelMode, FALSE,
                              (PLARGE_INTEGER)&ZERO_TIMEOUT);
    ASSERT_NTSTATUS(s, STATUS_SUCCESS);
    return true;
}

/* More objects than the thread's builtin wait blocks: caller array. */
static bool t_ke_five_objects_with_block_array(void)
{
    KEVENT ev[5];
    KWAIT_BLOCK blocks[5];
    PVOID objs[5];

    for (int i = 0; i < 5; i++) {
        KeInitializeEvent(&ev[i], SynchronizationEvent, i == 4);
        objs[i] = &ev[i];
    }

    NTSTATUS s = KeWaitForMultipleObjects(5, objs, WaitAny, Executive,
                                          KernelMode, FALSE,
                                          (PLARGE_INTEGER)&ZERO_TIMEOUT,
                                          blocks);
    ASSERT_NTSTATUS(s, STATUS_WAIT_0 + 4);
    return true;
}

static bool t_ke_timeout_expires(void)
{
    KEVENT ev;
    KeInitializeEvent(&ev, SynchronizationEvent, FALSE);

    PVOID objs[1] = { &ev };
    KWAIT_BLOCK blocks[1];
    NTSTATUS s = KeWaitForMultipleObjects(1, objs, WaitAny, Executive,
                                          KernelMode, FALSE,
                                          (PLARGE_INTEGER)&SHORT_TIMEOUT,
                                          blocks);
    ASSERT_NTSTATUS(s, STATUS_TIMEOUT);
    return true;
}

static bool t_nt_any_over_handles(void)
{
    HANDLE h0 = NULL, h1 = NULL;
    NTSTATUS s = NtCreateEvent(&h0, NULL, SynchronizationEvent, FALSE);
    ASSERT_NTSTATUS(s, STATUS_SUCCESS);
    s = NtCreateEvent(&h1, NULL, SynchronizationEvent, TRUE);
    if (!NT_SUCCESS(s)) {
        NtClose(h0);
        FAIL_AND_RETURN("second NtCreateEvent -> 0x%08x", (unsigned)s);
    }

    HANDLE handles[2] = { h0, h1 };
    s = NtWaitForMultipleObjectsEx(2, handles, WaitAny, UserMode, FALSE,
                                   (PLARGE_INTEGER)&ZERO_TIMEOUT);
    NtClose(h0);
    NtClose(h1);
    ASSERT_NTSTATUS(s, STATUS_WAIT_1);
    return true;
}

static bool t_nt_all_timeout(void)
{
    HANDLE h0 = NULL, h1 = NULL;
    NTSTATUS s = NtCreateEvent(&h0, NULL, NotificationEvent, TRUE);
    ASSERT_NTSTATUS(s, STATUS_SUCCESS);
    s = NtCreateEvent(&h1, NULL, NotificationEvent, FALSE);
    if (!NT_SUCCESS(s)) {
        NtClose(h0);
        FAIL_AND_RETURN("second NtCreateEvent -> 0x%08x", (unsigned)s);
    }

    HANDLE handles[2] = { h0, h1 };
    s = NtWaitForMultipleObjectsEx(2, handles, WaitAll, UserMode, FALSE,
                                   (PLARGE_INTEGER)&SHORT_TIMEOUT);
    NtClose(h0);
    NtClose(h1);
    ASSERT_NTSTATUS(s, STATUS_TIMEOUT);
    return true;
}

static bool t_nt_signal_and_wait(void)
{
    /* Manual-reset A stays signaled after NtSignalAndWait sets it, so the
     * signal half can be verified after the wait half returns. */
    HANDLE sig = NULL, wait = NULL;
    NTSTATUS s = NtCreateEvent(&sig, NULL, NotificationEvent, FALSE);
    ASSERT_NTSTATUS(s, STATUS_SUCCESS);
    s = NtCreateEvent(&wait, NULL, NotificationEvent, TRUE);
    if (!NT_SUCCESS(s)) {
        NtClose(sig);
        FAIL_AND_RETURN("second NtCreateEvent -> 0x%08x", (unsigned)s);
    }

    s = NtSignalAndWaitForSingleObjectEx(sig, wait, UserMode, FALSE,
                                         (PLARGE_INTEGER)&ZERO_TIMEOUT);
    ASSERT_NTSTATUS(s, STATUS_SUCCESS);

    HANDLE handles[1] = { sig };
    s = NtWaitForMultipleObjectsEx(1, handles, WaitAny, UserMode, FALSE,
                                   (PLARGE_INTEGER)&ZERO_TIMEOUT);
    NtClose(sig);
    NtClose(wait);
    ASSERT_NTSTATUS(s, STATUS_WAIT_0);
    return true;
}

static bool t_nt_signal_and_wait_timeout(void)
{
    HANDLE sig = NULL, wait = NULL;
    NTSTATUS s = NtCreateEvent(&sig, NULL, NotificationEvent, FALSE);
    ASSERT_NTSTATUS(s, STATUS_SUCCESS);
    s = NtCreateEvent(&wait, NULL, NotificationEvent, FALSE);
    if (!NT_SUCCESS(s)) {
        NtClose(sig);
        FAIL_AND_RETURN("second NtCreateEvent -> 0x%08x", (unsigned)s);
    }

    s = NtSignalAndWaitForSingleObjectEx(sig, wait, UserMode, FALSE,
                                         (PLARGE_INTEGER)&SHORT_TIMEOUT);
    ASSERT_NTSTATUS(s, STATUS_TIMEOUT);

    /* The signal half still happened. */
    HANDLE handles[1] = { sig };
    s = NtWaitForMultipleObjectsEx(1, handles, WaitAny, UserMode, FALSE,
                                   (PLARGE_INTEGER)&ZERO_TIMEOUT);
    NtClose(sig);
    NtClose(wait);
    ASSERT_NTSTATUS(s, STATUS_WAIT_0);
    return true;
}

static const test_entry_t ke_waitmulti_entries[] = {
    {"any_picks_lowest_signaled",   t_ke_any_picks_lowest_signaled},
    {"all_drains_every_object",     t_ke_all_drains_every_object},
    {"all_unsatisfied_takes_nothing", t_ke_all_unsatisfied_takes_nothing},
    {"five_objects_with_block_array", t_ke_five_objects_with_block_array},
    {"timeout_expires",             t_ke_timeout_expires},
    {"nt_any_over_handles",         t_nt_any_over_handles},
    {"nt_all_timeout",              t_nt_all_timeout},
    {"nt_signal_and_wait",          t_nt_signal_and_wait},
    {"nt_signal_and_wait_timeout",  t_nt_signal_and_wait_timeout},
};

DEFINE_GROUP(ke_waitmulti, "ke/waitmulti");
