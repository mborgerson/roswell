/*
 * Thread suspend and resume.
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

static const test_entry_t ke_threadstate_entries[] = {
    { "nt_suspend_and_resume_report_the_previous_count",
      nt_suspend_and_resume_report_the_previous_count, NULL },
    { "a_suspended_thread_makes_no_progress",
      a_suspended_thread_makes_no_progress, NULL },
    { "ke_suspend_and_resume_return_the_previous_count",
      ke_suspend_and_resume_return_the_previous_count, NULL },
    { "suspending_a_terminated_thread_is_refused",
      suspending_a_terminated_thread_is_refused, NULL },
};

DEFINE_GROUP(ke_threadstate, "ke/threadstate");
