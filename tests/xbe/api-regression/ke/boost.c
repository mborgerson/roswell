/*
 * KeBoostPriorityThread -- the scheduler's temporary lift.
 *
 * The boost is computed from the thread's BASE priority, not its
 * current one: it raises the thread to base+increment, and only when
 * that is higher than where the thread already is.  It stops short of
 * the real-time range and leaves a thread that is already in that range
 * alone.
 *
 * Nothing exported reads a thread's current priority back, so every
 * case reads it the way a title would: KeSetPriorityThread returns the
 * priority the thread was at, which is what the boost moved.  The
 * target is a thread parked on an event so that no wait completion of
 * its own moves the priority underneath the measurement.
 */

#include "../harness.h"

#define LOW_REALTIME_PRIORITY 16

/* nxdk declares KeSetDisableBoostThread as returning LOGICAL; the console
 * returns a BOOLEAN, so only the low byte is meaningful. */
typedef BOOLEAN (NTAPI *disable_boost_fn)(PKTHREAD, BOOLEAN);

static KEVENT g_park;
static KEVENT g_parked;
static KEVENT g_exited;

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

static void NTAPI parker(PVOID arg)
{
    LARGE_INTEGER timeout;

    (void)arg;
    timeout.QuadPart = -((LONGLONG)10 * 1000 * 10000);
    KeSetEvent(&g_parked, 0, FALSE);
    KeWaitForSingleObject(&g_park, Executive, KernelMode, FALSE, &timeout);
    KeSetEvent(&g_exited, 0, FALSE);
}

/* A thread blocked on g_park, and the object behind its handle. */
static NTSTATUS start_parker(HANDLE *h, PKTHREAD *thread)
{
    LARGE_INTEGER timeout;
    NTSTATUS s;

    KeInitializeEvent(&g_park, NotificationEvent, FALSE);
    KeInitializeEvent(&g_parked, NotificationEvent, FALSE);
    KeInitializeEvent(&g_exited, NotificationEvent, FALSE);

    *h = NULL;
    *thread = NULL;
    s = PsCreateSystemThreadEx(h, 0, 0, 0, NULL, parker, NULL, FALSE, FALSE,
                               test_system_routine);
    if (!NT_SUCCESS(s))
        return s;

    timeout.QuadPart = -((LONGLONG)2 * 1000 * 10000);
    s = KeWaitForSingleObject(&g_parked, Executive, KernelMode, FALSE,
                              &timeout);
    if (s != STATUS_SUCCESS) {
        KeSetEvent(&g_park, 0, FALSE);
        NtClose(*h);
        return STATUS_UNSUCCESSFUL;
    }
    /* The announcement is raised before the wait itself begins. */
    sleep_ms(50);

    s = ObReferenceObjectByHandle(*h, &PsThreadObjectType, (PVOID *)thread);
    if (!NT_SUCCESS(s)) {
        KeSetEvent(&g_park, 0, FALSE);
        NtClose(*h);
        return s;
    }
    return STATUS_SUCCESS;
}

static void stop_parker(HANDLE h, PKTHREAD thread)
{
    LARGE_INTEGER timeout;

    KeSetEvent(&g_park, 0, FALSE);
    timeout.QuadPart = -((LONGLONG)2 * 1000 * 10000);
    KeWaitForSingleObject(&g_exited, Executive, KernelMode, FALSE, &timeout);
    if (thread != NULL)
        ObfDereferenceObject(thread);
    NtClose(h);
}

/* A boost of zero pulls the thread up to its base priority, which is
 * how the rest of the cases learn what that base is. */
static KPRIORITY discover_base(PKTHREAD thread)
{
    KeSetPriorityThread(thread, 1);
    KeBoostPriorityThread(thread, 0);
    return KeSetPriorityThread(thread, 1);
}

static bool t_the_boost_is_measured_from_the_base_priority(void)
{
    HANDLE h;
    PKTHREAD thread;
    NTSTATUS s;
    KPRIORITY base, boosted;

    s = start_parker(&h, &thread);
    if (!NT_SUCCESS(s))
        FAIL_AND_RETURN("start parker: 0x%08x", (unsigned)s);

    base = discover_base(thread);
    KeBoostPriorityThread(thread, 2);
    boosted = KeSetPriorityThread(thread, base);
    stop_parker(h, thread);

    if (base < 2 || base >= LOW_REALTIME_PRIORITY)
        FAIL_AND_RETURN("base priority %ld is outside the dynamic range",
                        (long)base);
    ASSERT_EQ_U32(boosted, base + 2);
    return true;
}

/* Only upwards: a boost that lands below where the thread already is
 * leaves it where it was. */
static bool t_a_boost_below_the_current_priority_does_nothing(void)
{
    HANDLE h;
    PKTHREAD thread;
    NTSTATUS s;
    KPRIORITY base, after;

    s = start_parker(&h, &thread);
    if (!NT_SUCCESS(s))
        FAIL_AND_RETURN("start parker: 0x%08x", (unsigned)s);

    base = discover_base(thread);
    KeSetPriorityThread(thread, base + 4);
    KeBoostPriorityThread(thread, 1);
    after = KeSetPriorityThread(thread, base);
    stop_parker(h, thread);

    ASSERT_EQ_U32(after, base + 4);
    return true;
}

/* The boost stays out of the real-time range: an increment that would
 * cross into it stops one below. */
static bool t_a_boost_stops_below_the_real_time_range(void)
{
    HANDLE h;
    PKTHREAD thread;
    NTSTATUS s;
    KPRIORITY base, boosted;

    s = start_parker(&h, &thread);
    if (!NT_SUCCESS(s))
        FAIL_AND_RETURN("start parker: 0x%08x", (unsigned)s);

    base = discover_base(thread);
    KeSetPriorityThread(thread, 1);
    KeBoostPriorityThread(thread, 100);
    boosted = KeSetPriorityThread(thread, base);
    stop_parker(h, thread);

    ASSERT_EQ_U32(boosted, LOW_REALTIME_PRIORITY - 1);
    return true;
}

/* A thread already in the real-time range is not the scheduler's to
 * move: the boost is dropped whatever the increment. */
static bool t_a_real_time_thread_is_left_alone(void)
{
    HANDLE h;
    PKTHREAD thread;
    NTSTATUS s;
    KPRIORITY base, after;

    s = start_parker(&h, &thread);
    if (!NT_SUCCESS(s))
        FAIL_AND_RETURN("start parker: 0x%08x", (unsigned)s);

    base = discover_base(thread);
    KeSetPriorityThread(thread, LOW_REALTIME_PRIORITY + 4);
    KeBoostPriorityThread(thread, 8);
    after = KeSetPriorityThread(thread, base);
    stop_parker(h, thread);

    ASSERT_EQ_U32(after, LOW_REALTIME_PRIORITY + 4);
    return true;
}

/* The boost-disable flag is the wait path's, not this call's: an
 * explicit boost goes through on a thread that has boosting off. */
static bool t_the_boost_disable_flag_does_not_hold_it_back(void)
{
    HANDLE h;
    PKTHREAD thread;
    NTSTATUS s;
    KPRIORITY base, boosted;
    disable_boost_fn set_disable = (disable_boost_fn)KeSetDisableBoostThread;

    s = start_parker(&h, &thread);
    if (!NT_SUCCESS(s))
        FAIL_AND_RETURN("start parker: 0x%08x", (unsigned)s);

    base = discover_base(thread);
    set_disable(thread, TRUE);
    KeSetPriorityThread(thread, 1);
    KeBoostPriorityThread(thread, 3);
    boosted = KeSetPriorityThread(thread, base);
    set_disable(thread, FALSE);
    stop_parker(h, thread);

    ASSERT_EQ_U32(boosted, base + 3);
    return true;
}

static const test_entry_t ke_boost_entries[] = {
    { "the_boost_is_measured_from_the_base_priority",
      t_the_boost_is_measured_from_the_base_priority, NULL },
    { "a_boost_below_the_current_priority_does_nothing",
      t_a_boost_below_the_current_priority_does_nothing, NULL },
    { "a_boost_stops_below_the_real_time_range",
      t_a_boost_stops_below_the_real_time_range, NULL },
    { "a_real_time_thread_is_left_alone",
      t_a_real_time_thread_is_left_alone, NULL },
    { "the_boost_disable_flag_does_not_hold_it_back",
      t_the_boost_disable_flag_does_not_hold_it_back, NULL },
};

DEFINE_GROUP(ke_boost, "ke/boost");
