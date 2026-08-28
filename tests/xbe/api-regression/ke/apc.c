/*
 * NtQueueApcThread: the APC a title queues onto a thread of its own.
 *
 * There is no ring 3 here, so the routine cannot be dispatched on a
 * user return the way NT does it.  What the console does instead is
 * run it inside the target thread's next alertable wait, which breaks
 * out with STATUS_USER_APC -- so the wait, not the queueing, is the
 * delivery point, and every queued routine runs before that wait
 * returns.
 */

#include "../harness.h"

#ifndef STATUS_USER_APC
#define STATUS_USER_APC ((NTSTATUS)0x000000C0L)
#endif
#ifndef STATUS_TIMEOUT
#define STATUS_TIMEOUT ((NTSTATUS)0x00000102L)
#endif

#define SELF_THREAD ((HANDLE)(LONG_PTR)-2)

static volatile LONG g_calls;
static void *volatile g_a1;
static void *volatile g_a2;
static void *volatile g_a3;
static volatile LONG g_seq[8];

static VOID NTAPI apc_routine(PVOID a1, PVOID a2, PVOID a3)
{
    LONG n = g_calls;

    if (n < 8) g_seq[n] = (LONG)(ULONG_PTR)a1;
    g_calls = n + 1;
    g_a1 = a1;
    g_a2 = a2;
    g_a3 = a3;
}

static void reset(void)
{
    int i;

    g_calls = 0;
    g_a1 = g_a2 = g_a3 = NULL;
    for (i = 0; i < 8; i++) g_seq[i] = 0;
}

static NTSTATUS delay(ULONG ms, KPROCESSOR_MODE mode, BOOLEAN alertable)
{
    LARGE_INTEGER interval;

    interval.QuadPart = -(LONGLONG)(ms * 10000);
    return KeDelayExecutionThread(mode, alertable, &interval);
}

static bool t_the_wait_is_the_delivery_point(void)
{
    NTSTATUS s, waited;

    reset();
    s = NtQueueApcThread(SELF_THREAD, apc_routine, (PVOID)0xD4D4D4D4,
                         (PVOID)0xE5E5E5E5, (PVOID)0xF6F6F6F6);
    ASSERT_NTSTATUS(s, STATUS_SUCCESS);

    /* Queueing alone runs nothing, even across a plain delay. */
    delay(10, KernelMode, FALSE);
    if (g_calls != 0)
        FAIL_AND_RETURN("ran before any alertable wait (%ld)", (long)g_calls);

    waited = delay(20, UserMode, TRUE);
    ASSERT_NTSTATUS(waited, STATUS_USER_APC);
    ASSERT_EQ_U32(g_calls, 1);
    ASSERT_EQ_PTR(g_a1, (PVOID)0xD4D4D4D4);
    ASSERT_EQ_PTR(g_a2, (PVOID)0xE5E5E5E5);
    ASSERT_EQ_PTR(g_a3, (PVOID)0xF6F6F6F6);

    /* And it is consumed: the next wait sleeps its interval out. */
    waited = delay(10, UserMode, TRUE);
    ASSERT_NTSTATUS(waited, STATUS_SUCCESS);
    ASSERT_EQ_U32(g_calls, 1);
    return true;
}

static bool t_one_wait_drains_them_all_in_order(void)
{
    NTSTATUS waited;
    int i;

    reset();
    for (i = 1; i <= 3; i++) {
        NTSTATUS s = NtQueueApcThread(SELF_THREAD, apc_routine,
                                      (PVOID)(ULONG_PTR)i, NULL, NULL);
        if (!NT_SUCCESS(s)) FAIL_AND_RETURN("queue %d -> 0x%08x", i,
                                            (unsigned)s);
    }

    waited = delay(20, UserMode, TRUE);
    ASSERT_NTSTATUS(waited, STATUS_USER_APC);
    ASSERT_EQ_U32(g_calls, 3);
    ASSERT_EQ_U32(g_seq[0], 1);
    ASSERT_EQ_U32(g_seq[1], 2);
    ASSERT_EQ_U32(g_seq[2], 3);

    waited = delay(10, UserMode, TRUE);
    ASSERT_NTSTATUS(waited, STATUS_SUCCESS);
    ASSERT_EQ_U32(g_calls, 3);
    return true;
}

/* --- delivery to another thread ------------------------------------------ */

static volatile LONG g_worker_stop;
static volatile LONG g_worker_user_apcs;
static KEVENT g_worker_done;

static void NTAPI worker_system_routine(PKSTART_ROUTINE StartRoutine,
                                        PVOID StartContext)
{
    if (StartRoutine != NULL) StartRoutine(StartContext);
    PsTerminateSystemThread(STATUS_SUCCESS);
}

static void NTAPI worker(PVOID arg)
{
    (void)arg;
    while (InterlockedCompareExchange(&g_worker_stop, 0, 0) == 0) {
        if (delay(5, UserMode, TRUE) == STATUS_USER_APC)
            InterlockedIncrement(&g_worker_user_apcs);
    }
    KeSetEvent(&g_worker_done, 0, FALSE);
}

static bool t_another_thread_receives_it(void)
{
    LARGE_INTEGER timeout;
    HANDLE h = NULL;
    NTSTATUS s;

    reset();
    g_worker_stop = 0;
    g_worker_user_apcs = 0;
    KeInitializeEvent(&g_worker_done, NotificationEvent, FALSE);

    s = PsCreateSystemThreadEx(&h, 0, 0, 0, NULL, worker, NULL, FALSE, FALSE,
                               worker_system_routine);
    if (!NT_SUCCESS(s))
        FAIL_AND_RETURN("create worker: 0x%08x", (unsigned)s);

    s = NtQueueApcThread(h, apc_routine, (PVOID)0x1A1A1A1A,
                         (PVOID)0x2B2B2B2B, (PVOID)0x3C3C3C3C);
    if (!NT_SUCCESS(s)) {
        InterlockedExchange(&g_worker_stop, 1);
        timeout.QuadPart = -((LONGLONG)2 * 1000 * 10000);
        KeWaitForSingleObject(&g_worker_done, Executive, KernelMode, FALSE,
                              &timeout);
        NtClose(h);
        FAIL_AND_RETURN("queue -> 0x%08x", (unsigned)s);
    }

    delay(60, KernelMode, FALSE);

    InterlockedExchange(&g_worker_stop, 1);
    timeout.QuadPart = -((LONGLONG)2 * 1000 * 10000);
    KeWaitForSingleObject(&g_worker_done, Executive, KernelMode, FALSE,
                          &timeout);
    NtClose(h);

    ASSERT_EQ_U32(g_calls, 1);
    ASSERT_EQ_U32(g_worker_user_apcs, 1);
    ASSERT_EQ_PTR(g_a1, (PVOID)0x1A1A1A1A);
    ASSERT_EQ_PTR(g_a2, (PVOID)0x2B2B2B2B);
    ASSERT_EQ_PTR(g_a3, (PVOID)0x3C3C3C3C);
    return true;
}

/* Only a UserMode alertable wait is a delivery point: an alertable
 * KernelMode wait and a non-alertable UserMode one both sleep their
 * interval out with the APC still queued. */
static bool t_only_a_user_alertable_wait_delivers(void)
{
    NTSTATUS s, k, n, u;

    reset();
    s = NtQueueApcThread(SELF_THREAD, apc_routine, (PVOID)7, NULL, NULL);
    if (!NT_SUCCESS(s)) FAIL_AND_RETURN("queue -> 0x%08x", (unsigned)s);

    k = delay(10, KernelMode, TRUE);
    n = delay(10, UserMode, FALSE);
    if (k != STATUS_SUCCESS || n != STATUS_SUCCESS || g_calls != 0) {
        while (delay(10, UserMode, TRUE) == STATUS_USER_APC) { }
        FAIL_AND_RETURN("kernel=0x%08x nonalertable=0x%08x calls=%ld",
                        (unsigned)k, (unsigned)n, (long)g_calls);
    }

    u = delay(10, UserMode, TRUE);
    ASSERT_NTSTATUS(u, STATUS_USER_APC);
    ASSERT_EQ_U32(g_calls, 1);
    return true;
}

/* --- delivery into a wait that is already blocked ------------------------ */

/*
 * The group above queues into a thread that then enters a wait.  The
 * other order -- the thread already blocked in its alertable wait when
 * the APC arrives -- breaks that wait from the outside, and the question
 * is whether the routine has run by the time the wait returns.  The
 * console runs it there: the wait that breaks with STATUS_USER_APC is
 * the delivery point either way.
 */

static KEVENT g_solo_running;
static KEVENT g_solo_done;
static volatile LONG g_solo_status;
static volatile LONG g_solo_calls_at_return;

static void NTAPI solo_worker(PVOID arg)
{
    (void)arg;
    KeSetEvent(&g_solo_running, 0, FALSE);
    g_solo_status = delay(500, UserMode, TRUE);
    g_solo_calls_at_return = g_calls;
    KeSetEvent(&g_solo_done, 0, FALSE);
}

static bool t_a_blocked_wait_runs_the_routine_before_returning(void)
{
    LARGE_INTEGER timeout;
    HANDLE h = NULL;
    NTSTATUS s;

    reset();
    g_solo_status = 0;
    g_solo_calls_at_return = -1;
    KeInitializeEvent(&g_solo_running, NotificationEvent, FALSE);
    KeInitializeEvent(&g_solo_done, NotificationEvent, FALSE);

    s = PsCreateSystemThreadEx(&h, 0, 0, 0, NULL, solo_worker, NULL, FALSE,
                               FALSE, worker_system_routine);
    if (!NT_SUCCESS(s))
        FAIL_AND_RETURN("create worker: 0x%08x", (unsigned)s);

    /* Let it get well inside the wait before the APC arrives. */
    timeout.QuadPart = -((LONGLONG)2 * 1000 * 10000);
    KeWaitForSingleObject(&g_solo_running, Executive, KernelMode, FALSE,
                          &timeout);
    delay(50, KernelMode, FALSE);

    s = NtQueueApcThread(h, apc_routine, (PVOID)0x11223344, NULL, NULL);
    if (!NT_SUCCESS(s)) {
        KeWaitForSingleObject(&g_solo_done, Executive, KernelMode, FALSE,
                              &timeout);
        NtClose(h);
        FAIL_AND_RETURN("queue -> 0x%08x", (unsigned)s);
    }

    timeout.QuadPart = -((LONGLONG)2 * 1000 * 10000);
    s = KeWaitForSingleObject(&g_solo_done, Executive, KernelMode, FALSE,
                              &timeout);
    NtClose(h);
    if (s != STATUS_SUCCESS) FAIL_AND_RETURN("worker never finished");

    ASSERT_NTSTATUS((NTSTATUS)g_solo_status, STATUS_USER_APC);
    ASSERT_EQ_U32(g_solo_calls_at_return, 1);
    return true;
}

static const test_entry_t ke_apc_entries[] = {
    { "the_wait_is_the_delivery_point", t_the_wait_is_the_delivery_point,
      NULL },
    { "one_wait_drains_them_all_in_order",
      t_one_wait_drains_them_all_in_order, NULL },
    { "another_thread_receives_it", t_another_thread_receives_it, NULL },
    { "a_blocked_wait_runs_the_routine_before_returning",
      t_a_blocked_wait_runs_the_routine_before_returning, NULL },
    { "only_a_user_alertable_wait_delivers",
      t_only_a_user_alertable_wait_delivers, NULL },
};

DEFINE_GROUP(ke_apc, "ke/apc");
