/*
 * The Xbox kernel's timebase DATA exports: KeTimeIncrement (the nominal
 * 100ns period of a clock tick), and the live KeInterruptTime / KeSystemTime
 * KSYSTEM_TIME counters that the clock interrupt advances. Titles read these
 * directly (there is no shared user-data page on the console), so a stubbed
 * export reading zero would freeze a title's sense of time. Values and
 * behavior verified on the retail kernel (1.0.4627) via
 * api-regression-run --official.
 */

#include "../harness.h"

/* Tear-free read of a volatile KSYSTEM_TIME (the same High1/Low/High2
 * protocol the kernel writes with). */
static ULONGLONG read_systime(volatile KSYSTEM_TIME *t)
{
    for (;;) {
        LONG  high1 = t->High1Time;
        ULONG low   = t->LowPart;
        LONG  high2 = t->High2Time;
        if (high1 == high2)
            return ((ULONGLONG)(ULONG)high1 << 32) | low;
    }
}

static bool t_time_increment(void)
{
    /* Retail reports exactly 10000 (1 ms in 100ns units). */
    ASSERT_EQ_U32(KeTimeIncrement, 10000);
    return true;
}

static bool t_interrupt_time_advances(void)
{
    ULONGLONG a = read_systime(&KeInterruptTime);
    ASSERT_TRUE(a > 0);

    LARGE_INTEGER d = { .QuadPart = -((LONGLONG)50 * 10000) }; /* 50 ms */
    ASSERT_NTSTATUS(KeDelayExecutionThread(KernelMode, FALSE, &d),
                    STATUS_SUCCESS);

    ULONGLONG b = read_systime(&KeInterruptTime);
    ULONGLONG delta = b - a;
    /* ~50 ms elapsed; allow generous slop for tick granularity + overhead. */
    if (delta < 40 * 10000 || delta > 500 * 10000)
        FAIL_AND_RETURN("interrupt-time delta %llu out of [40,500] ms",
                        (unsigned long long)delta);
    return true;
}

static bool t_system_time_matches_query(void)
{
    ULONGLONG s = read_systime(&KeSystemTime);
    ASSERT_TRUE(s > 0);

    LARGE_INTEGER q = { .QuadPart = 0 };
    KeQuerySystemTime(&q);
    ULONGLONG diff = ((ULONGLONG)q.QuadPart >= s)
                     ? ((ULONGLONG)q.QuadPart - s) : (s - (ULONGLONG)q.QuadPart);
    if (diff > 50 * 10000)
        FAIL_AND_RETURN("KeSystemTime %llu vs query %llu differ %llu ms",
                        (unsigned long long)s, (unsigned long long)q.QuadPart,
                        (unsigned long long)(diff / 10000));
    return true;
}

static const test_entry_t ke_timebase_entries[] = {
    {"time_increment",            t_time_increment},
    {"interrupt_time_advances",   t_interrupt_time_advances},
    {"system_time_matches_query", t_system_time_matches_query},
};

DEFINE_GROUP(ke_timebase, "ke/timebase");
