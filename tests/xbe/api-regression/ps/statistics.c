/*
 * PsQueryStatistics -- the two counters a title can read about itself.
 *
 * The structure is caller-sized: Length goes in, the thread and handle
 * counts come out.  Absolute values depend on what the kernel itself is
 * running, so every case here measures a DELTA it caused: threads it
 * started, handles it opened.
 */

#include "../harness.h"
#include <string.h>

static KEVENT g_park;
static KEVENT g_parked;
static KEVENT g_exited;
static volatile LONG g_running;

static void NTAPI test_system_routine(PKSTART_ROUTINE StartRoutine,
                                      PVOID StartContext)
{
    if (StartRoutine != NULL)
        StartRoutine(StartContext);
    PsTerminateSystemThread(STATUS_SUCCESS);
}

static void NTAPI parker(PVOID arg)
{
    LARGE_INTEGER timeout;

    (void)arg;
    timeout.QuadPart = -((LONGLONG)10 * 1000 * 10000);
    if (InterlockedDecrement(&g_running) == 0)
        KeSetEvent(&g_parked, 0, FALSE);
    KeWaitForSingleObject(&g_park, Executive, KernelMode, FALSE, &timeout);
    KeSetEvent(&g_exited, 0, FALSE);
}

static NTSTATUS query(PS_STATISTICS *out)
{
    memset(out, 0xCC, sizeof(*out));
    out->Length = sizeof(*out);
    return PsQueryStatistics(out);
}

static bool t_the_counts_come_back(void)
{
    PS_STATISTICS stats;
    NTSTATUS s;

    s = query(&stats);
    ASSERT_NTSTATUS(s, STATUS_SUCCESS);
    ASSERT_EQ_U32(stats.Length, sizeof(stats));
    /* This thread is one of them, and it holds handles. */
    ASSERT_TRUE(stats.ThreadCount > 0);
    ASSERT_TRUE(stats.HandleCount > 0);
    return true;
}

/* A length the kernel does not recognise is refused, and nothing is
 * written past it. */
static bool t_a_wrong_length_is_refused(void)
{
    PS_STATISTICS stats;
    NTSTATUS s;

    memset(&stats, 0xCC, sizeof(stats));
    stats.Length = sizeof(stats) - 1;
    s = PsQueryStatistics(&stats);
    ASSERT_NTSTATUS(s, STATUS_INVALID_PARAMETER);
    ASSERT_EQ_U32(stats.ThreadCount, 0xCCCCCCCC);
    ASSERT_EQ_U32(stats.HandleCount, 0xCCCCCCCC);

    memset(&stats, 0xCC, sizeof(stats));
    stats.Length = sizeof(stats) + 4;
    s = PsQueryStatistics(&stats);
    ASSERT_NTSTATUS(s, STATUS_INVALID_PARAMETER);
    return true;
}

/* Three threads parked on an event, and the count that follows them. */
static bool t_running_threads_are_counted(void)
{
    enum { N = 3 };
    HANDLE h[N];
    PS_STATISTICS before, during, after;
    LARGE_INTEGER timeout;
    NTSTATUS s;
    int i, started = 0;

    KeInitializeEvent(&g_park, NotificationEvent, FALSE);
    KeInitializeEvent(&g_parked, NotificationEvent, FALSE);
    KeInitializeEvent(&g_exited, NotificationEvent, FALSE);
    g_running = N;

    s = query(&before);
    if (!NT_SUCCESS(s))
        FAIL_AND_RETURN("first query -> 0x%08x", (unsigned)s);

    for (i = 0; i < N; i++) {
        s = PsCreateSystemThreadEx(&h[i], 0, 0, 0, NULL, parker, NULL, FALSE,
                                   FALSE, test_system_routine);
        if (!NT_SUCCESS(s))
            break;
        started++;
    }
    if (started != N) {
        KeSetEvent(&g_park, 0, FALSE);
        for (i = 0; i < started; i++)
            NtClose(h[i]);
        FAIL_AND_RETURN("create thread %d -> 0x%08x", started, (unsigned)s);
    }

    timeout.QuadPart = -((LONGLONG)5 * 1000 * 10000);
    s = KeWaitForSingleObject(&g_parked, Executive, KernelMode, FALSE,
                              &timeout);
    if (s != STATUS_SUCCESS) {
        KeSetEvent(&g_park, 0, FALSE);
        for (i = 0; i < N; i++)
            NtClose(h[i]);
        FAIL_AND_RETURN("threads never parked: 0x%08x", (unsigned)s);
    }

    s = query(&during);

    KeSetEvent(&g_park, 0, FALSE);
    for (i = 0; i < N; i++) {
        NtWaitForSingleObjectEx(h[i], KernelMode, FALSE, &timeout);
        NtClose(h[i]);
    }

    if (!NT_SUCCESS(s))
        FAIL_AND_RETURN("second query -> 0x%08x", (unsigned)s);
    ASSERT_EQ_U32(during.ThreadCount, before.ThreadCount + N);

    /* And back down once they are gone. */
    s = query(&after);
    ASSERT_NTSTATUS(s, STATUS_SUCCESS);
    ASSERT_EQ_U32(after.ThreadCount, before.ThreadCount);
    return true;
}

static bool t_open_handles_are_counted(void)
{
    enum { N = 4 };
    HANDLE h[N];
    PS_STATISTICS before, during, after;
    NTSTATUS s;
    int i, opened = 0;

    s = query(&before);
    if (!NT_SUCCESS(s))
        FAIL_AND_RETURN("first query -> 0x%08x", (unsigned)s);

    for (i = 0; i < N; i++) {
        s = NtCreateEvent(&h[i], NULL, NotificationEvent, FALSE);
        if (!NT_SUCCESS(s))
            break;
        opened++;
    }
    if (opened != N) {
        for (i = 0; i < opened; i++)
            NtClose(h[i]);
        FAIL_AND_RETURN("create event %d -> 0x%08x", opened, (unsigned)s);
    }

    s = query(&during);
    for (i = 0; i < N; i++)
        NtClose(h[i]);

    if (!NT_SUCCESS(s))
        FAIL_AND_RETURN("second query -> 0x%08x", (unsigned)s);
    ASSERT_EQ_U32(during.HandleCount, before.HandleCount + N);

    s = query(&after);
    ASSERT_NTSTATUS(s, STATUS_SUCCESS);
    ASSERT_EQ_U32(after.HandleCount, before.HandleCount);
    return true;
}

static const test_entry_t ps_statistics_entries[] = {
    { "the_counts_come_back", t_the_counts_come_back, NULL },
    { "a_wrong_length_is_refused", t_a_wrong_length_is_refused, NULL },
    { "running_threads_are_counted", t_running_threads_are_counted, NULL },
    { "open_handles_are_counted", t_open_handles_are_counted, NULL },
};

DEFINE_GROUP(ps_statistics, "ps/statistics");
