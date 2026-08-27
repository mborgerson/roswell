/*
 * KeInsertQueueApc: the queueing half of a title-owned APC.
 *
 * Titles run in ring 0, so a "user mode" APC has no ring 3 to return to
 * and the console runs it inside the target thread's next alertable
 * UserMode wait -- the same delivery point NtQueueApcThread uses.  A
 * kernel-mode APC has no such gate: queueing one onto the running thread
 * at PASSIVE runs it before the caller's next statement.
 *
 * The routines are handed the caller's own KAPC pointer, so a title can
 * reach the structure it embedded the APC in.
 */

#include "../harness.h"
#include <string.h>

#ifndef STATUS_USER_APC
#define STATUS_USER_APC ((NTSTATUS)0x000000C0L)
#endif

#define SPIN_SLICE_MS 2

static volatile LONG g_kernel_calls;
static volatile LONG g_normal_calls;
static volatile LONG g_rundown_calls;
static PVOID volatile g_kernel_apc;
static PVOID volatile g_rundown_apc;
static PVOID volatile g_arg1;
static PVOID volatile g_arg2;
static PVOID volatile g_normal_ctx;
static PVOID volatile g_normal_arg1;
static PVOID volatile g_normal_arg2;
static volatile LONG g_seq[8];
static volatile LONG g_seq_len;
static volatile BOOLEAN g_cancel_normal;
static volatile BOOLEAN g_inserted_in_routine;

static void note(LONG what)
{
    LONG n = g_seq_len;

    if (n < 8) g_seq[n] = what;
    g_seq_len = n + 1;
}

static void reset(void)
{
    int i;

    g_kernel_calls = g_normal_calls = g_rundown_calls = 0;
    g_kernel_apc = g_rundown_apc = NULL;
    g_arg1 = g_arg2 = NULL;
    g_normal_ctx = g_normal_arg1 = g_normal_arg2 = NULL;
    g_seq_len = 0;
    for (i = 0; i < 8; i++) g_seq[i] = 0;
    g_cancel_normal = FALSE;
    g_inserted_in_routine = TRUE;
}

static void NTAPI kernel_routine(PKAPC Apc, PKNORMAL_ROUTINE *NormalRoutine,
                                 PVOID *NormalContext, PVOID *SystemArgument1,
                                 PVOID *SystemArgument2)
{
    (void)NormalContext;
    note(1);
    g_kernel_calls++;
    g_kernel_apc = Apc;
    g_arg1 = *SystemArgument1;
    g_arg2 = *SystemArgument2;
    g_inserted_in_routine = Apc->Inserted;
    if (g_cancel_normal)
        *NormalRoutine = NULL;
}

static void NTAPI normal_routine(PVOID NormalContext, PVOID SystemArgument1,
                                 PVOID SystemArgument2)
{
    note(2);
    g_normal_calls++;
    g_normal_ctx = NormalContext;
    g_normal_arg1 = SystemArgument1;
    g_normal_arg2 = SystemArgument2;
}

static void NTAPI rundown_routine(PKAPC Apc)
{
    note(3);
    g_rundown_calls++;
    g_rundown_apc = Apc;
}

static NTSTATUS alertable_wait(ULONG ms)
{
    LARGE_INTEGER interval;

    interval.QuadPart = -(LONGLONG)(ms * 10000);
    return KeDelayExecutionThread(UserMode, TRUE, &interval);
}

static void sleep_ms(ULONG ms)
{
    LARGE_INTEGER interval;

    interval.QuadPart = -(LONGLONG)(ms * 10000);
    KeDelayExecutionThread(KernelMode, FALSE, &interval);
}

/* A kernel-mode APC with no normal routine, queued onto the running
 * thread at PASSIVE, has run by the time the insert returns -- and the
 * kernel routine is handed the caller's own KAPC. */
static bool t_a_kernel_apc_runs_before_the_insert_returns(void)
{
    KAPC apc;
    BOOLEAN inserted;

    reset();
    KeInitializeApc(&apc, KeGetCurrentThread(), kernel_routine, NULL, NULL,
                    KernelMode, NULL);
    inserted = KeInsertQueueApc(&apc, (PVOID)0x11111111, (PVOID)0x22222222, 0);

    ASSERT_EQ_U32(inserted, TRUE);
    ASSERT_EQ_U32(g_kernel_calls, 1);
    ASSERT_EQ_PTR(g_kernel_apc, &apc);
    ASSERT_EQ_PTR(g_arg1, (PVOID)0x11111111);
    ASSERT_EQ_PTR(g_arg2, (PVOID)0x22222222);
    /* Dequeued before the routine runs, and visible as such. */
    ASSERT_EQ_U32(g_inserted_in_routine, FALSE);
    ASSERT_EQ_U32(apc.Inserted, FALSE);
    /* The arguments reach the structure the title owns. */
    ASSERT_EQ_PTR(apc.SystemArgument1, (PVOID)0x11111111);
    ASSERT_EQ_PTR(apc.SystemArgument2, (PVOID)0x22222222);
    return true;
}

/* With a normal routine, both run -- kernel first -- and the normal
 * routine receives the context from initialisation plus the arguments
 * from insertion. */
static bool t_the_normal_routine_runs_after_the_kernel_one(void)
{
    KAPC apc;

    reset();
    KeInitializeApc(&apc, KeGetCurrentThread(), kernel_routine, NULL,
                    normal_routine, KernelMode, (PVOID)0x33333333);
    ASSERT_EQ_U32(KeInsertQueueApc(&apc, (PVOID)0x44444444,
                                   (PVOID)0x55555555, 0), TRUE);

    ASSERT_EQ_U32(g_kernel_calls, 1);
    ASSERT_EQ_U32(g_normal_calls, 1);
    ASSERT_EQ_U32(g_seq_len, 2);
    ASSERT_EQ_U32(g_seq[0], 1);
    ASSERT_EQ_U32(g_seq[1], 2);
    ASSERT_EQ_PTR(g_normal_ctx, (PVOID)0x33333333);
    ASSERT_EQ_PTR(g_normal_arg1, (PVOID)0x44444444);
    ASSERT_EQ_PTR(g_normal_arg2, (PVOID)0x55555555);
    return true;
}

/* The kernel routine owns the decision: clearing the normal routine
 * through its out-parameter cancels it. */
static bool t_the_kernel_routine_can_cancel_the_normal_one(void)
{
    KAPC apc;

    reset();
    g_cancel_normal = TRUE;
    KeInitializeApc(&apc, KeGetCurrentThread(), kernel_routine, NULL,
                    normal_routine, KernelMode, NULL);
    ASSERT_EQ_U32(KeInsertQueueApc(&apc, NULL, NULL, 0), TRUE);

    ASSERT_EQ_U32(g_kernel_calls, 1);
    ASSERT_EQ_U32(g_normal_calls, 0);
    return true;
}

/* A user-mode APC waits for an alertable UserMode wait, which breaks out
 * with STATUS_USER_APC.  Until then it stays queued. */
static bool t_a_user_apc_waits_for_an_alertable_wait(void)
{
    KAPC apc;
    NTSTATUS waited;

    reset();
    KeInitializeApc(&apc, KeGetCurrentThread(), kernel_routine, NULL,
                    normal_routine, UserMode, (PVOID)0x66666666);
    ASSERT_EQ_U32(KeInsertQueueApc(&apc, (PVOID)0x77777777,
                                   (PVOID)0x78787878, 0), TRUE);
    ASSERT_EQ_U32(apc.Inserted, TRUE);
    ASSERT_EQ_U32(g_kernel_calls, 0);

    /* A non-alertable wait is not a delivery point. */
    sleep_ms(10);
    ASSERT_EQ_U32(g_kernel_calls, 0);

    waited = alertable_wait(50);
    ASSERT_NTSTATUS(waited, STATUS_USER_APC);
    ASSERT_EQ_U32(g_kernel_calls, 1);
    ASSERT_EQ_U32(g_normal_calls, 1);
    ASSERT_EQ_PTR(g_kernel_apc, &apc);
    ASSERT_EQ_PTR(g_arg1, (PVOID)0x77777777);
    ASSERT_EQ_PTR(g_arg2, (PVOID)0x78787878);
    ASSERT_EQ_PTR(g_normal_ctx, (PVOID)0x66666666);
    ASSERT_EQ_PTR(g_normal_arg1, (PVOID)0x77777777);
    ASSERT_EQ_PTR(g_normal_arg2, (PVOID)0x78787878);
    ASSERT_EQ_U32(apc.Inserted, FALSE);

    /* Delivered, so the same structure can be queued again. */
    ASSERT_EQ_U32(KeInsertQueueApc(&apc, NULL, NULL, 0), TRUE);
    ASSERT_NTSTATUS(alertable_wait(50), STATUS_USER_APC);
    ASSERT_EQ_U32(g_kernel_calls, 2);
    return true;
}

/* A second insert of an APC that is still queued is refused -- but the
 * system arguments are stamped into the structure before the refusal, so
 * the queued APC is left carrying the rejected caller's arguments. */
static bool t_a_second_insert_is_refused(void)
{
    KAPC apc;

    reset();
    KeInitializeApc(&apc, KeGetCurrentThread(), kernel_routine, NULL,
                    normal_routine, UserMode, NULL);
    ASSERT_EQ_U32(KeInsertQueueApc(&apc, (PVOID)0x11223344,
                                   (PVOID)0x55667788, 0), TRUE);
    ASSERT_EQ_PTR(apc.SystemArgument1, (PVOID)0x11223344);

    ASSERT_EQ_U32(KeInsertQueueApc(&apc, (PVOID)0x99AABBCC,
                                   (PVOID)0xDDEEFF00, 0), FALSE);
    ASSERT_EQ_U32(apc.Inserted, TRUE);
    ASSERT_EQ_PTR(apc.SystemArgument1, (PVOID)0x99AABBCC);
    ASSERT_EQ_PTR(apc.SystemArgument2, (PVOID)0xDDEEFF00);

    /* Drain it: what the routines receive is what the structure now says. */
    ASSERT_NTSTATUS(alertable_wait(50), STATUS_USER_APC);
    ASSERT_EQ_U32(g_kernel_calls, 1);
    ASSERT_EQ_PTR(g_arg1, (PVOID)0x99AABBCC);
    ASSERT_EQ_PTR(g_arg2, (PVOID)0xDDEEFF00);
    return true;
}

static volatile LONG g_stop;
static KEVENT g_exited;

static void NTAPI test_system_routine(PKSTART_ROUTINE StartRoutine,
                                      PVOID StartContext)
{
    if (StartRoutine != NULL)
        StartRoutine(StartContext);
    PsTerminateSystemThread(STATUS_SUCCESS);
}

/* Sits in alertable waits so a user APC queued onto it is delivered. */
static void NTAPI alertable_spinner(PVOID arg)
{
    (void)arg;
    while (InterlockedCompareExchange(&g_stop, 0, 0) == 0)
        alertable_wait(SPIN_SLICE_MS);
    KeSetEvent(&g_exited, 0, FALSE);
}

/* Never waits alertably, so a user APC queued onto it survives until the
 * thread exits and is run down instead of delivered. */
static void NTAPI sleepy_thread(PVOID arg)
{
    (void)arg;
    while (InterlockedCompareExchange(&g_stop, 0, 0) == 0)
        sleep_ms(SPIN_SLICE_MS);
    KeSetEvent(&g_exited, 0, FALSE);
}

static NTSTATUS start_thread(PKSTART_ROUTINE routine, PHANDLE handle)
{
    g_stop = 0;
    KeInitializeEvent(&g_exited, NotificationEvent, FALSE);
    return PsCreateSystemThreadEx(handle, 0, 0, 0, NULL, routine, NULL,
                                  FALSE, FALSE, test_system_routine);
}

static void stop_thread(HANDLE handle)
{
    LARGE_INTEGER timeout;

    InterlockedExchange(&g_stop, 1);
    timeout.QuadPart = -(LONGLONG)(2000 * 10000);
    KeWaitForSingleObject(&g_exited, Executive, KernelMode, FALSE, &timeout);
    sleep_ms(10);
    if (handle != NULL) NtClose(handle);
}

/* The APC runs on the thread it names, not on the one that queued it. */
static bool t_it_runs_on_the_target_thread(void)
{
    KAPC apc;
    HANDLE handle = NULL;
    PVOID target = NULL;
    NTSTATUS s;
    int i;

    reset();
    s = start_thread(alertable_spinner, &handle);
    if (!NT_SUCCESS(s)) FAIL_AND_RETURN("create -> 0x%08x", (unsigned)s);

    s = ObReferenceObjectByHandle(handle, &PsThreadObjectType, &target);
    if (!NT_SUCCESS(s)) {
        stop_thread(handle);
        FAIL_AND_RETURN("reference -> 0x%08x", (unsigned)s);
    }

    KeInitializeApc(&apc, (PKTHREAD)target, kernel_routine, NULL,
                    normal_routine, UserMode, (PVOID)0x88888888);
    if (!KeInsertQueueApc(&apc, (PVOID)0x99999999, NULL, 0)) {
        ObfDereferenceObject(target);
        stop_thread(handle);
        FAIL_AND_RETURN("insert refused");
    }

    for (i = 0; i < 100 && g_normal_calls == 0; i++)
        sleep_ms(5);

    ObfDereferenceObject(target);
    stop_thread(handle);

    ASSERT_EQ_U32(g_kernel_calls, 1);
    ASSERT_EQ_U32(g_normal_calls, 1);
    ASSERT_EQ_PTR(g_kernel_apc, &apc);
    ASSERT_EQ_PTR(g_normal_ctx, (PVOID)0x88888888);
    ASSERT_EQ_PTR(g_normal_arg1, (PVOID)0x99999999);
    return true;
}

/* A user APC still queued when its thread exits is run down rather than
 * delivered, and the rundown routine gets the caller's KAPC too. */
static bool t_an_undelivered_apc_is_run_down(void)
{
    KAPC apc;
    HANDLE handle = NULL;
    PVOID target = NULL;
    NTSTATUS s;

    reset();
    s = start_thread(sleepy_thread, &handle);
    if (!NT_SUCCESS(s)) FAIL_AND_RETURN("create -> 0x%08x", (unsigned)s);

    s = ObReferenceObjectByHandle(handle, &PsThreadObjectType, &target);
    if (!NT_SUCCESS(s)) {
        stop_thread(handle);
        FAIL_AND_RETURN("reference -> 0x%08x", (unsigned)s);
    }

    KeInitializeApc(&apc, (PKTHREAD)target, kernel_routine, rundown_routine,
                    normal_routine, UserMode, NULL);
    if (!KeInsertQueueApc(&apc, NULL, NULL, 0)) {
        ObfDereferenceObject(target);
        stop_thread(handle);
        FAIL_AND_RETURN("insert refused");
    }

    stop_thread(handle);
    ObfDereferenceObject(target);

    ASSERT_EQ_U32(g_kernel_calls, 0);
    ASSERT_EQ_U32(g_normal_calls, 0);
    ASSERT_EQ_U32(g_rundown_calls, 1);
    ASSERT_EQ_PTR(g_rundown_apc, &apc);
    return true;
}

static const test_entry_t ke_apcqueue_entries[] = {
    { "a_kernel_apc_runs_before_the_insert_returns",
      t_a_kernel_apc_runs_before_the_insert_returns, NULL },
    { "the_normal_routine_runs_after_the_kernel_one",
      t_the_normal_routine_runs_after_the_kernel_one, NULL },
    { "the_kernel_routine_can_cancel_the_normal_one",
      t_the_kernel_routine_can_cancel_the_normal_one, NULL },
    { "a_user_apc_waits_for_an_alertable_wait",
      t_a_user_apc_waits_for_an_alertable_wait, NULL },
    { "a_second_insert_is_refused", t_a_second_insert_is_refused, NULL },
    { "it_runs_on_the_target_thread", t_it_runs_on_the_target_thread, NULL },
    { "an_undelivered_apc_is_run_down", t_an_undelivered_apc_is_run_down,
      NULL },
};

DEFINE_GROUP(ke_apcqueue, "ke/apcqueue");
