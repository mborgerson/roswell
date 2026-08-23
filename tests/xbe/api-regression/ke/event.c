/*
 * KeInitializeEvent + KeSetEvent + KeResetEvent + KeWaitForSingleObject.
 */

#include "../harness.h"

static bool t_init_notification_not_signaled(void)
{
    KEVENT e;
    KeInitializeEvent(&e, NotificationEvent, FALSE);
    LARGE_INTEGER zero = { .QuadPart = 0 };
    NTSTATUS s = KeWaitForSingleObject(&e, Executive, KernelMode, FALSE, &zero);
    ASSERT_NTSTATUS(s, STATUS_TIMEOUT);
    return true;
}

static bool t_init_notification_signaled(void)
{
    KEVENT e;
    KeInitializeEvent(&e, NotificationEvent, TRUE);
    LARGE_INTEGER zero = { .QuadPart = 0 };
    NTSTATUS s = KeWaitForSingleObject(&e, Executive, KernelMode, FALSE, &zero);
    ASSERT_NTSTATUS(s, STATUS_SUCCESS);
    /* Notification events stay signaled after a satisfying wait. */
    s = KeWaitForSingleObject(&e, Executive, KernelMode, FALSE, &zero);
    ASSERT_NTSTATUS(s, STATUS_SUCCESS);
    return true;
}

static bool t_set_then_wait(void)
{
    KEVENT e;
    KeInitializeEvent(&e, NotificationEvent, FALSE);
    LONG prev = KeSetEvent(&e, 0, FALSE);
    ASSERT_EQ_U32(prev, 0);

    LARGE_INTEGER zero = { .QuadPart = 0 };
    NTSTATUS s = KeWaitForSingleObject(&e, Executive, KernelMode, FALSE, &zero);
    ASSERT_NTSTATUS(s, STATUS_SUCCESS);
    return true;
}

static bool t_reset(void)
{
    KEVENT e;
    KeInitializeEvent(&e, NotificationEvent, TRUE);
    LONG prev = KeResetEvent(&e);
    ASSERT_EQ_U32(prev, 1);

    LARGE_INTEGER zero = { .QuadPart = 0 };
    NTSTATUS s = KeWaitForSingleObject(&e, Executive, KernelMode, FALSE, &zero);
    ASSERT_NTSTATUS(s, STATUS_TIMEOUT);
    return true;
}

static bool t_synchronization_event_auto_resets(void)
{
    KEVENT e;
    KeInitializeEvent(&e, SynchronizationEvent, TRUE);
    LARGE_INTEGER zero = { .QuadPart = 0 };

    NTSTATUS s = KeWaitForSingleObject(&e, Executive, KernelMode, FALSE, &zero);
    ASSERT_NTSTATUS(s, STATUS_SUCCESS);
    /* Second wait should time out because the first wait consumed it. */
    s = KeWaitForSingleObject(&e, Executive, KernelMode, FALSE, &zero);
    ASSERT_NTSTATUS(s, STATUS_TIMEOUT);
    return true;
}

static const test_entry_t ke_event_entries[] = {
    {"init_notification_not_signaled", t_init_notification_not_signaled},
    {"init_notification_signaled",     t_init_notification_signaled},
    {"set_then_wait",                  t_set_then_wait},
    {"reset",                          t_reset},
    {"synchronization_auto_resets",    t_synchronization_event_auto_resets},
};

DEFINE_GROUP(ke_event, "ke/event");
