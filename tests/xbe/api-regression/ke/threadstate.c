/*
 * Thread suspend / resume and the boost-disable flag.
 *
 * Both levels of the same counter are covered: the Nt pair works on a
 * handle and reports the count through a pointer, the Ke pair works on the
 * thread object and returns it.  A title reaches the object the way this
 * does -- ObReferenceObjectByHandle on a thread handle -- rather than
 * through KeGetCurrentThread, which hands back the Xbox-shaped shadow.
 */

#include "../harness.h"

#ifndef STATUS_THREAD_IS_TERMINATING
#define STATUS_THREAD_IS_TERMINATING ((NTSTATUS)0xC000004BL)
#endif

/* nxdk declares KeSetDisableBoostThread as returning LOGICAL; the console
 * returns a BOOLEAN, so only the low byte is meaningful. */
typedef BOOLEAN (NTAPI *disable_boost_fn)(PKTHREAD, BOOLEAN);

#define SPIN_SLICE_MS 2

static volatile LONG g_ticks;
static volatile LONG g_stop;
static KEVENT g_exited;

static void NTAPI test_system_routine(PKSTART_ROUTINE StartRoutine,
                                      PVOID StartContext)
{
    if (StartRoutine != NULL)
        StartRoutine(StartContext);
    PsTerminateSystemThread(STATUS_SUCCESS);
}

static void NTAPI spinner(PVOID arg)
{
    LARGE_INTEGER slice;

    (void)arg;
    slice.QuadPart = -(LONGLONG)(SPIN_SLICE_MS * 10000);
    while (InterlockedCompareExchange(&g_stop, 0, 0) == 0) {
        InterlockedIncrement(&g_ticks);
        KeDelayExecutionThread(KernelMode, FALSE, &slice);
    }
    KeSetEvent(&g_exited, 0, FALSE);
}

static void sleep_ms(ULONG ms)
{
    LARGE_INTEGER delay;

    delay.QuadPart = -(LONGLONG)(ms * 10000);
    KeDelayExecutionThread(KernelMode, FALSE, &delay);
}

/* A running spinner, and the thread object behind its handle. */
static NTSTATUS start_spinner(HANDLE *h, PKTHREAD *thread)
{
    NTSTATUS s;

    g_ticks = 0;
    g_stop = 0;
    KeInitializeEvent(&g_exited, NotificationEvent, FALSE);

    *h = NULL;
    *thread = NULL;
    s = PsCreateSystemThreadEx(h, 0, 0, 0, NULL, spinner, NULL, FALSE, FALSE,
                               test_system_routine);
    if (!NT_SUCCESS(s))
        return s;

    s = ObReferenceObjectByHandle(*h, &PsThreadObjectType, (PVOID *)thread);
    if (!NT_SUCCESS(s)) {
        NtClose(*h);
        return s;
    }
    return STATUS_SUCCESS;
}

static void stop_spinner(HANDLE h, PKTHREAD thread)
{
    LARGE_INTEGER timeout;

    InterlockedExchange(&g_stop, 1);
    timeout.QuadPart = -((LONGLONG)2 * 1000 * 10000);
    KeWaitForSingleObject(&g_exited, Executive, KernelMode, FALSE, &timeout);
    if (thread != NULL)
        ObfDereferenceObject(thread);
    NtClose(h);
}

static bool nt_suspend_and_resume_report_the_previous_count(void)
{
    HANDLE h;
    PKTHREAD thread;
    NTSTATUS s;
    ULONG prev;

    s = start_spinner(&h, &thread);
    if (!NT_SUCCESS(s))
        FAIL_AND_RETURN("start spinner: 0x%08x", (unsigned)s);

    prev = 0xEEEEEEEE;
    s = NtSuspendThread(h, &prev);
    if (!NT_SUCCESS(s) || prev != 0) {
        NtResumeThread(h, NULL);
        stop_spinner(h, thread);
        FAIL_AND_RETURN("first suspend: 0x%08x prev=%lu", (unsigned)s,
                        (unsigned long)prev);
    }

    prev = 0xEEEEEEEE;
    s = NtSuspendThread(h, &prev);
    if (!NT_SUCCESS(s) || prev != 1) {
        NtResumeThread(h, NULL);
        NtResumeThread(h, NULL);
        stop_spinner(h, thread);
        FAIL_AND_RETURN("second suspend: 0x%08x prev=%lu", (unsigned)s,
                        (unsigned long)prev);
    }

    prev = 0xEEEEEEEE;
    s = NtResumeThread(h, &prev);
    if (!NT_SUCCESS(s) || prev != 2) {
        NtResumeThread(h, NULL);
        stop_spinner(h, thread);
        FAIL_AND_RETURN("first resume: 0x%08x prev=%lu", (unsigned)s,
                        (unsigned long)prev);
    }

    prev = 0xEEEEEEEE;
    s = NtResumeThread(h, &prev);
    stop_spinner(h, thread);
    ASSERT_NTSTATUS(s, STATUS_SUCCESS);
    ASSERT_EQ_U32(prev, 1);
    return true;
}

static bool a_suspended_thread_makes_no_progress(void)
{
    HANDLE h;
    PKTHREAD thread;
    NTSTATUS s;
    LONG before, after;

    s = start_spinner(&h, &thread);
    if (!NT_SUCCESS(s))
        FAIL_AND_RETURN("start spinner: 0x%08x", (unsigned)s);

    sleep_ms(20);
    s = NtSuspendThread(h, NULL);
    if (!NT_SUCCESS(s)) {
        stop_spinner(h, thread);
        FAIL_AND_RETURN("suspend: 0x%08x", (unsigned)s);
    }

    /* Let any in-flight slice finish before the sample window opens. */
    sleep_ms(20);
    before = InterlockedCompareExchange(&g_ticks, 0, 0);
    sleep_ms(60);
    after = InterlockedCompareExchange(&g_ticks, 0, 0);
    if (before != after) {
        NtResumeThread(h, NULL);
        stop_spinner(h, thread);
        FAIL_AND_RETURN("suspended thread ticked %ld -> %ld", before, after);
    }

    s = NtResumeThread(h, NULL);
    if (!NT_SUCCESS(s)) {
        stop_spinner(h, thread);
        FAIL_AND_RETURN("resume: 0x%08x", (unsigned)s);
    }
    sleep_ms(60);
    after = InterlockedCompareExchange(&g_ticks, 0, 0);
    stop_spinner(h, thread);
    if (after == before)
        FAIL_AND_RETURN("thread never resumed (still %ld)", before);
    return true;
}

static bool ke_suspend_and_resume_return_the_previous_count(void)
{
    HANDLE h;
    PKTHREAD thread;
    NTSTATUS s;
    ULONG first, second, third, fourth;

    s = start_spinner(&h, &thread);
    if (!NT_SUCCESS(s))
        FAIL_AND_RETURN("start spinner: 0x%08x", (unsigned)s);

    first  = KeSuspendThread(thread);
    second = KeSuspendThread(thread);
    third  = KeResumeThread(thread);
    fourth = KeResumeThread(thread);
    stop_spinner(h, thread);

    ASSERT_EQ_U32(first, 0);
    ASSERT_EQ_U32(second, 1);
    ASSERT_EQ_U32(third, 2);
    ASSERT_EQ_U32(fourth, 1);
    return true;
}

static bool suspending_a_terminated_thread_is_refused(void)
{
    HANDLE h;
    PKTHREAD thread;
    LARGE_INTEGER timeout;
    NTSTATUS s;
    ULONG prev;

    s = start_spinner(&h, &thread);
    if (!NT_SUCCESS(s))
        FAIL_AND_RETURN("start spinner: 0x%08x", (unsigned)s);

    InterlockedExchange(&g_stop, 1);
    timeout.QuadPart = -((LONGLONG)2 * 1000 * 10000);
    s = NtWaitForSingleObject(h, FALSE, &timeout);
    if (!NT_SUCCESS(s)) {
        stop_spinner(h, thread);
        FAIL_AND_RETURN("wait for exit: 0x%08x", (unsigned)s);
    }

    prev = 0xEEEEEEEE;
    s = NtSuspendThread(h, &prev);
    ObfDereferenceObject(thread);
    NtClose(h);

    ASSERT_NTSTATUS(s, STATUS_THREAD_IS_TERMINATING);
    ASSERT_EQ_U32(prev, 0xEEEEEEEE);
    return true;
}

static bool disable_boost_returns_the_previous_setting(void)
{
    disable_boost_fn set_boost = (disable_boost_fn)KeSetDisableBoostThread;
    HANDLE h;
    PKTHREAD thread;
    NTSTATUS s;
    BOOLEAN a, b, c, d;

    s = start_spinner(&h, &thread);
    if (!NT_SUCCESS(s))
        FAIL_AND_RETURN("start spinner: 0x%08x", (unsigned)s);

    a = set_boost(thread, TRUE);
    b = set_boost(thread, TRUE);
    c = set_boost(thread, FALSE);
    d = set_boost(thread, FALSE);
    stop_spinner(h, thread);

    ASSERT_EQ_U32(a, FALSE);
    ASSERT_EQ_U32(b, TRUE);
    ASSERT_EQ_U32(c, TRUE);
    ASSERT_EQ_U32(d, FALSE);
    return true;
}

/* --- alerts --------------------------------------------------------------
 *
 * An alert is the flag an alertable wait consults; setting it on a
 * thread that never waits alertably is inert, which is what lets these
 * cases use the spinner without disturbing it.
 */

static bool alerting_reports_the_previous_state(void)
{
    HANDLE h;
    PKTHREAD thread;
    NTSTATUS s;
    BOOLEAN first, second;

    s = start_spinner(&h, &thread);
    if (!NT_SUCCESS(s))
        FAIL_AND_RETURN("start spinner: 0x%08x", (unsigned)s);

    first = KeAlertThread(thread, KernelMode);
    second = KeAlertThread(thread, KernelMode);
    stop_spinner(h, thread);

    ASSERT_EQ_U32(first, FALSE);
    ASSERT_EQ_U32(second, TRUE);
    return true;
}

/* An alerting resume is a resume: it returns the previous suspend count
 * and the thread runs again. */
static bool an_alerting_resume_returns_the_suspend_count(void)
{
    HANDLE h;
    PKTHREAD thread;
    NTSTATUS s;
    LONG stalled;
    ULONG prev;

    s = start_spinner(&h, &thread);
    if (!NT_SUCCESS(s))
        FAIL_AND_RETURN("start spinner: 0x%08x", (unsigned)s);

    s = NtSuspendThread(h, NULL);
    if (!NT_SUCCESS(s)) {
        stop_spinner(h, thread);
        FAIL_AND_RETURN("suspend: 0x%08x", (unsigned)s);
    }
    sleep_ms(SPIN_SLICE_MS * 8);
    stalled = InterlockedCompareExchange(&g_ticks, 0, 0);

    prev = KeAlertResumeThread(thread);
    if (prev != 1) {
        NtResumeThread(h, NULL);
        stop_spinner(h, thread);
        FAIL_AND_RETURN("previous suspend count = %lu", (unsigned long)prev);
    }

    sleep_ms(SPIN_SLICE_MS * 8);
    {
        LONG resumed = InterlockedCompareExchange(&g_ticks, 0, 0);
        stop_spinner(h, thread);
        if (resumed <= stalled)
            FAIL_AND_RETURN("no progress after the alerting resume (%ld)",
                            (long)resumed);
    }
    return true;
}

/* Testing the alert reports it and consumes it.  The caller reaches its
 * own thread object through the current-thread pseudo handle, which the
 * object manager resolves without going near the Xbox-shaped shadow. */
static bool testing_an_alert_consumes_it(void)
{
    PKTHREAD self = NULL;
    NTSTATUS s;
    BOOLEAN before, first, second;

    s = ObReferenceObjectByHandle((HANDLE)(LONG_PTR)-2 /* current thread */,
                                  &PsThreadObjectType, (PVOID *)&self);
    if (!NT_SUCCESS(s))
        FAIL_AND_RETURN("reference the current thread: 0x%08x", (unsigned)s);
    ASSERT_NOT_NULL(self);

    before = KeAlertThread(self, KernelMode);
    first = KeTestAlertThread(KernelMode);
    second = KeTestAlertThread(KernelMode);
    ObfDereferenceObject(self);

    ASSERT_EQ_U32(before, FALSE);
    ASSERT_EQ_U32(first, TRUE);
    ASSERT_EQ_U32(second, FALSE);
    return true;
}

static const test_entry_t ke_threadstate_entries[] = {
    { "nt_suspend_and_resume_report_the_previous_count",
      nt_suspend_and_resume_report_the_previous_count, NULL },
    { "a_suspended_thread_makes_no_progress",
      a_suspended_thread_makes_no_progress, NULL },
    { "ke_suspend_and_resume_return_the_previous_count",
      ke_suspend_and_resume_return_the_previous_count, NULL },
    { "suspending_a_terminated_thread_is_refused",
      suspending_a_terminated_thread_is_refused, NULL },
    { "disable_boost_returns_the_previous_setting",
      disable_boost_returns_the_previous_setting, NULL },
    { "alerting_reports_the_previous_state",
      alerting_reports_the_previous_state, NULL },
    { "an_alerting_resume_returns_the_suspend_count",
      an_alerting_resume_returns_the_suspend_count, NULL },
    { "testing_an_alert_consumes_it", testing_an_alert_consumes_it, NULL },
};

DEFINE_GROUP(ke_threadstate, "ke/threadstate");
