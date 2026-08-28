/*
 * KePulseEvent -- the object-level pulse.
 *
 * A pulse signals the event just long enough to release whoever is
 * already waiting on it and then resets it, so a waiter that arrives
 * afterwards misses it entirely.  The return value is the state the
 * event was in before the pulse, the same value KeSetEvent reports.
 *
 * The Nt-level twin (NtPulseEvent) works on a handle and is covered by
 * ob/named; this group works on the KEVENT itself, which is what a
 * driver inside a title does.
 */

#include "../harness.h"

static void NTAPI test_system_routine(PKSTART_ROUTINE StartRoutine,
                                      PVOID StartContext)
{
    if (StartRoutine != NULL)
        StartRoutine(StartContext);
    PsTerminateSystemThread(STATUS_SUCCESS);
}

static void sleep_ms(ULONG ms)
{
    LARGE_INTEGER delay;

    delay.QuadPart = -(LONGLONG)(ms * 10000);
    KeDelayExecutionThread(KernelMode, FALSE, &delay);
}

static KEVENT g_target;
static KEVENT g_running;
static KEVENT g_done;
static NTSTATUS g_wait_status;

/* Announces that it is about to block, then waits for the pulse. */
static void NTAPI waiter(PVOID arg)
{
    LARGE_INTEGER timeout;

    (void)arg;
    timeout.QuadPart = -((LONGLONG)5 * 1000 * 10000);
    KeSetEvent(&g_running, 0, FALSE);
    g_wait_status = KeWaitForSingleObject(&g_target, Executive, KernelMode,
                                          FALSE, &timeout);
    KeSetEvent(&g_done, 0, FALSE);
}

static bool t_the_previous_state_comes_back(void)
{
    KEVENT ev;
    LONG from_clear, from_set;

    KeInitializeEvent(&ev, NotificationEvent, FALSE);
    from_clear = KePulseEvent(&ev, 0, FALSE);

    KeSetEvent(&ev, 0, FALSE);
    from_set = KePulseEvent(&ev, 0, FALSE);

    ASSERT_EQ_U32(from_clear, 0);
    ASSERT_EQ_U32(from_set, 1);
    return true;
}

/* Whatever the event was, a pulse leaves it clear. */
static bool t_the_event_is_left_clear(void)
{
    KEVENT ev;
    LARGE_INTEGER timeout;
    NTSTATUS s;

    KeInitializeEvent(&ev, NotificationEvent, TRUE);
    KePulseEvent(&ev, 0, FALSE);

    timeout.QuadPart = 0;
    s = KeWaitForSingleObject(&ev, Executive, KernelMode, FALSE, &timeout);

    ASSERT_NTSTATUS(s, STATUS_TIMEOUT);
    ASSERT_EQ_U32(KeResetEvent(&ev), 0);
    return true;
}

/* A synchronization event behaves the same way: the pulse reports the
 * old state and the event comes out clear. */
static bool t_a_synchronization_event_pulses_the_same_way(void)
{
    KEVENT ev;
    LONG previous;

    KeInitializeEvent(&ev, SynchronizationEvent, TRUE);
    previous = KePulseEvent(&ev, 0, FALSE);

    ASSERT_EQ_U32(previous, 1);
    ASSERT_EQ_U32(KeResetEvent(&ev), 0);
    return true;
}

/* The point of a pulse: a thread already blocked on the event is
 * released, and the event is clear again by the time it runs. */
static bool t_a_blocked_waiter_is_released(void)
{
    HANDLE h = NULL;
    NTSTATUS s;
    LARGE_INTEGER timeout;
    LONG previous;

    KeInitializeEvent(&g_target, NotificationEvent, FALSE);
    KeInitializeEvent(&g_running, NotificationEvent, FALSE);
    KeInitializeEvent(&g_done, NotificationEvent, FALSE);
    g_wait_status = STATUS_UNSUCCESSFUL;

    s = PsCreateSystemThreadEx(&h, 0, 0, 0, NULL, waiter, NULL, FALSE, FALSE,
                               test_system_routine);
    if (!NT_SUCCESS(s))
        FAIL_AND_RETURN("create waiter: 0x%08x", (unsigned)s);

    timeout.QuadPart = -((LONGLONG)2 * 1000 * 10000);
    s = KeWaitForSingleObject(&g_running, Executive, KernelMode, FALSE,
                              &timeout);
    if (s != STATUS_SUCCESS) {
        NtClose(h);
        FAIL_AND_RETURN("waiter never started: 0x%08x", (unsigned)s);
    }
    /* KeSetEvent above is raised before the wait itself begins. */
    sleep_ms(50);

    previous = KePulseEvent(&g_target, 0, FALSE);

    s = KeWaitForSingleObject(&g_done, Executive, KernelMode, FALSE, &timeout);
    NtClose(h);
    if (s != STATUS_SUCCESS)
        FAIL_AND_RETURN("waiter did not finish: 0x%08x", (unsigned)s);

    ASSERT_EQ_U32(previous, 0);
    ASSERT_NTSTATUS(g_wait_status, STATUS_SUCCESS);
    ASSERT_EQ_U32(KeResetEvent(&g_target), 0);
    return true;
}

/* And the other half of it: nobody waiting means the signal is gone. */
static bool t_a_pulse_with_nobody_waiting_is_lost(void)
{
    KEVENT ev;
    LARGE_INTEGER timeout;
    NTSTATUS s;

    KeInitializeEvent(&ev, NotificationEvent, FALSE);
    KePulseEvent(&ev, 0, FALSE);

    timeout.QuadPart = 0;
    s = KeWaitForSingleObject(&ev, Executive, KernelMode, FALSE, &timeout);

    ASSERT_NTSTATUS(s, STATUS_TIMEOUT);
    return true;
}

static const test_entry_t ke_pulse_entries[] = {
    { "the_previous_state_comes_back", t_the_previous_state_comes_back, NULL },
    { "the_event_is_left_clear", t_the_event_is_left_clear, NULL },
    { "a_synchronization_event_pulses_the_same_way",
      t_a_synchronization_event_pulses_the_same_way, NULL },
    { "a_blocked_waiter_is_released", t_a_blocked_waiter_is_released, NULL },
    { "a_pulse_with_nobody_waiting_is_lost",
      t_a_pulse_with_nobody_waiting_is_lost, NULL },
};

DEFINE_GROUP(ke_pulse, "ke/pulse");
