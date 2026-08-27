/*
 * Executive timer objects: create, arm, wait, query, cancel.
 *
 * The console's setter is NtSetTimerEx, which names the mode its APC is
 * delivered in instead of inheriting the caller's, and its creator takes no
 * access mask.  The timer object itself is waitable, so most of the contract
 * is observable by waiting on the handle.
 */

#include "../harness.h"

#define MS(n) (-(LONGLONG)(n) * 10000)     /* relative 100 ns units */

static NTSTATUS make_timer(TIMER_TYPE type, HANDLE *h)
{
    *h = NULL;
    return NtCreateTimer(h, NULL, type);
}

static NTSTATUS arm(HANDLE h, LONGLONG due, LONG period, PBOOLEAN prev)
{
    LARGE_INTEGER when;

    when.QuadPart = due;
    return NtSetTimerEx(h, &when, NULL, KernelMode, NULL, FALSE, period, prev);
}

static NTSTATUS wait_for(HANDLE h, ULONG ms)
{
    LARGE_INTEGER timeout;

    timeout.QuadPart = MS(ms);
    return NtWaitForSingleObject(h, FALSE, &timeout);
}

static bool a_notification_timer_stays_signalled(void)
{
    HANDLE h;
    NTSTATUS s;
    BOOLEAN prev = TRUE;

    s = make_timer(NotificationTimer, &h);
    if (!NT_SUCCESS(s))
        FAIL_AND_RETURN("NtCreateTimer: 0x%08x", (unsigned)s);

    s = arm(h, MS(50), 0, &prev);
    if (!NT_SUCCESS(s)) {
        NtClose(h);
        FAIL_AND_RETURN("NtSetTimerEx: 0x%08x", (unsigned)s);
    }
    if (prev != FALSE) {
        NtClose(h);
        FAIL_AND_RETURN("fresh timer reported previous state %u", prev);
    }

    s = wait_for(h, 2000);
    if (s != STATUS_SUCCESS) {
        NtClose(h);
        FAIL_AND_RETURN("wait for expiry: 0x%08x", (unsigned)s);
    }

    /* Notification: once signalled it stays signalled for every waiter. */
    s = wait_for(h, 0);
    NtClose(h);
    ASSERT_NTSTATUS(s, STATUS_SUCCESS);
    return true;
}

static bool a_synchronization_timer_auto_resets(void)
{
    HANDLE h;
    NTSTATUS s;

    s = make_timer(SynchronizationTimer, &h);
    if (!NT_SUCCESS(s))
        FAIL_AND_RETURN("NtCreateTimer: 0x%08x", (unsigned)s);

    s = arm(h, MS(50), 0, NULL);
    if (!NT_SUCCESS(s)) {
        NtClose(h);
        FAIL_AND_RETURN("NtSetTimerEx: 0x%08x", (unsigned)s);
    }

    s = wait_for(h, 2000);
    if (s != STATUS_SUCCESS) {
        NtClose(h);
        FAIL_AND_RETURN("wait for expiry: 0x%08x", (unsigned)s);
    }

    s = wait_for(h, 0);
    NtClose(h);
    ASSERT_NTSTATUS(s, STATUS_TIMEOUT);
    return true;
}

static bool a_periodic_timer_keeps_firing(void)
{
    HANDLE h;
    NTSTATUS s;
    int i;

    s = make_timer(SynchronizationTimer, &h);
    if (!NT_SUCCESS(s))
        FAIL_AND_RETURN("NtCreateTimer: 0x%08x", (unsigned)s);

    s = arm(h, MS(30), 30, NULL);
    if (!NT_SUCCESS(s)) {
        NtClose(h);
        FAIL_AND_RETURN("NtSetTimerEx: 0x%08x", (unsigned)s);
    }

    for (i = 0; i < 3; i++) {
        s = wait_for(h, 2000);
        if (s != STATUS_SUCCESS) {
            NtCancelTimer(h, NULL);
            NtClose(h);
            FAIL_AND_RETURN("period %d: 0x%08x", i, (unsigned)s);
        }
    }

    s = NtCancelTimer(h, NULL);
    NtClose(h);
    ASSERT_NTSTATUS(s, STATUS_SUCCESS);
    return true;
}

static bool a_cancelled_timer_never_fires(void)
{
    HANDLE h;
    NTSTATUS s;

    s = make_timer(NotificationTimer, &h);
    if (!NT_SUCCESS(s))
        FAIL_AND_RETURN("NtCreateTimer: 0x%08x", (unsigned)s);

    s = arm(h, MS(100), 0, NULL);
    if (!NT_SUCCESS(s)) {
        NtClose(h);
        FAIL_AND_RETURN("NtSetTimerEx: 0x%08x", (unsigned)s);
    }

    s = NtCancelTimer(h, NULL);
    if (!NT_SUCCESS(s)) {
        NtClose(h);
        FAIL_AND_RETURN("NtCancelTimer: 0x%08x", (unsigned)s);
    }

    s = wait_for(h, 300);
    NtClose(h);
    ASSERT_NTSTATUS(s, STATUS_TIMEOUT);
    return true;
}

/* Both the query and the cancel report the timer's SIGNAL state -- not
 * whether it was armed -- and re-arming reports the state it is replacing. */

static bool query_reports_the_time_remaining(void)
{
    TIMER_BASIC_INFORMATION info;
    HANDLE h;
    NTSTATUS s;

    s = make_timer(NotificationTimer, &h);
    if (!NT_SUCCESS(s))
        FAIL_AND_RETURN("NtCreateTimer: 0x%08x", (unsigned)s);

    memset(&info, 0xEE, sizeof(info));
    s = NtQueryTimer(h, &info);
    if (!NT_SUCCESS(s) || info.TimerState != FALSE) {
        NtClose(h);
        FAIL_AND_RETURN("query idle: 0x%08x state=%u", (unsigned)s,
                        info.TimerState);
    }

    s = arm(h, MS(2000), 0, NULL);
    if (!NT_SUCCESS(s)) {
        NtClose(h);
        FAIL_AND_RETURN("NtSetTimerEx: 0x%08x", (unsigned)s);
    }

    memset(&info, 0xEE, sizeof(info));
    s = NtQueryTimer(h, &info);
    if (!NT_SUCCESS(s) || info.TimerState != FALSE) {
        NtCancelTimer(h, NULL);
        NtClose(h);
        FAIL_AND_RETURN("query armed: 0x%08x state=%u", (unsigned)s,
                        info.TimerState);
    }
    /* Counts down towards zero from the 2 s that were asked for. */
    if (info.RemainingTime.QuadPart <= 0 ||
        info.RemainingTime.QuadPart > 2000 * 10000LL ||
        info.RemainingTime.QuadPart < 1500 * 10000LL) {
        NtCancelTimer(h, NULL);
        NtClose(h);
        FAIL_AND_RETURN("remaining %08lx:%08lx out of range",
                        (unsigned long)info.RemainingTime.HighPart,
                        (unsigned long)info.RemainingTime.LowPart);
    }

    NtCancelTimer(h, NULL);
    s = arm(h, MS(20), 0, NULL);
    if (NT_SUCCESS(s))
        s = wait_for(h, 2000);
    if (s != STATUS_SUCCESS) {
        NtClose(h);
        FAIL_AND_RETURN("wait for expiry: 0x%08x", (unsigned)s);
    }

    memset(&info, 0xEE, sizeof(info));
    s = NtQueryTimer(h, &info);
    NtClose(h);
    ASSERT_NTSTATUS(s, STATUS_SUCCESS);
    ASSERT_EQ_U32(info.TimerState, TRUE);
    return true;
}

static bool cancel_reports_the_signal_state(void)
{
    HANDLE h;
    NTSTATUS s;
    BOOLEAN armed = 0xEE, idle = 0xEE;

    s = make_timer(NotificationTimer, &h);
    if (!NT_SUCCESS(s))
        FAIL_AND_RETURN("NtCreateTimer: 0x%08x", (unsigned)s);

    s = arm(h, MS(5000), 0, NULL);
    if (NT_SUCCESS(s))
        s = NtCancelTimer(h, &armed);
    if (NT_SUCCESS(s))
        s = NtCancelTimer(h, &idle);
    NtClose(h);

    ASSERT_NTSTATUS(s, STATUS_SUCCESS);
    /* An armed-but-unexpired timer is unsignalled, so both read FALSE; a
     * "was it running" reading would have made the first TRUE. */
    ASSERT_EQ_U32(armed, FALSE);
    ASSERT_EQ_U32(idle, FALSE);
    return true;
}

static bool rearming_reports_the_previous_signal_state(void)
{
    HANDLE h;
    NTSTATUS s;
    BOOLEAN prev = 0xEE;

    s = make_timer(NotificationTimer, &h);
    if (!NT_SUCCESS(s))
        FAIL_AND_RETURN("NtCreateTimer: 0x%08x", (unsigned)s);

    s = arm(h, MS(20), 0, NULL);
    if (NT_SUCCESS(s))
        s = wait_for(h, 2000);
    if (s != STATUS_SUCCESS) {
        NtClose(h);
        FAIL_AND_RETURN("wait for expiry: 0x%08x", (unsigned)s);
    }

    s = arm(h, MS(5000), 0, &prev);
    NtCancelTimer(h, NULL);
    NtClose(h);
    ASSERT_NTSTATUS(s, STATUS_SUCCESS);
    ASSERT_EQ_U32(prev, TRUE);
    return true;
}

static ULONG g_apc_calls;
static PVOID g_apc_context;

static VOID NTAPI timer_apc(PVOID TimerContext, ULONG Low, LONG High)
{
    (void)Low;
    (void)High;
    g_apc_calls++;
    g_apc_context = TimerContext;
}

/* The APC runs once per expiry, with the context the timer was armed with,
 * whichever mode the caller names.  Only that it runs is asserted: with no
 * ring 3 to return to, where a UserMode APC is delivered from cannot be the
 * same as on a console that has one. */
static bool apc_on_expiry(KPROCESSOR_MODE mode)
{
    static int marker;
    LARGE_INTEGER when, wait;
    HANDLE h, ev;
    NTSTATUS s;

    s = NtCreateEvent(&ev, NULL, NotificationEvent, FALSE);
    if (!NT_SUCCESS(s))
        FAIL_AND_RETURN("NtCreateEvent: 0x%08x", (unsigned)s);
    s = make_timer(NotificationTimer, &h);
    if (!NT_SUCCESS(s)) {
        NtClose(ev);
        FAIL_AND_RETURN("NtCreateTimer: 0x%08x", (unsigned)s);
    }

    g_apc_calls = 0;
    g_apc_context = NULL;
    when.QuadPart = MS(50);
    s = NtSetTimerEx(h, &when, timer_apc, mode, &marker, FALSE, 0, NULL);
    if (!NT_SUCCESS(s)) {
        NtClose(h);
        NtClose(ev);
        FAIL_AND_RETURN("NtSetTimerEx: 0x%08x", (unsigned)s);
    }

    /* Alertable, so a queued APC has somewhere to run. */
    wait.QuadPart = MS(500);
    NtWaitForSingleObjectEx(ev, UserMode, TRUE, &wait);

    NtCancelTimer(h, NULL);
    NtClose(h);
    NtClose(ev);

    ASSERT_EQ_U32(g_apc_calls, 1);
    ASSERT_EQ_PTR(g_apc_context, &marker);
    return true;
}

static bool a_kernel_mode_apc_runs_on_expiry(void)
{
    return apc_on_expiry(KernelMode);
}

static bool a_user_mode_apc_runs_on_expiry(void)
{
    return apc_on_expiry(UserMode);
}

static const test_entry_t ex_timer_entries[] = {
    { "a_notification_timer_stays_signalled",
      a_notification_timer_stays_signalled, NULL },
    { "a_synchronization_timer_auto_resets",
      a_synchronization_timer_auto_resets, NULL },
    { "a_periodic_timer_keeps_firing", a_periodic_timer_keeps_firing, NULL },
    { "a_cancelled_timer_never_fires", a_cancelled_timer_never_fires, NULL },
    { "query_reports_the_time_remaining",
      query_reports_the_time_remaining, NULL },
    { "cancel_reports_the_signal_state",
      cancel_reports_the_signal_state, NULL },
    { "rearming_reports_the_previous_signal_state",
      rearming_reports_the_previous_signal_state, NULL },
    { "a_kernel_mode_apc_runs_on_expiry",
      a_kernel_mode_apc_runs_on_expiry, NULL },
    { "a_user_mode_apc_runs_on_expiry",
      a_user_mode_apc_runs_on_expiry, NULL },
};

DEFINE_GROUP(ex_timer, "ex/timer");
