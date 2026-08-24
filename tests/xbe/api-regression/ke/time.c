/*
 * KeQuerySystemTime / KeQueryPerformance{Counter,Frequency} / KeTickCount /
 * KeDelayExecutionThread / KeStallExecutionProcessor.
 */

#include "../harness.h"

static bool t_query_system_time_nonzero(void)
{
    LARGE_INTEGER t = { .QuadPart = 0 };
    KeQuerySystemTime(&t);
    ASSERT_TRUE(t.QuadPart > 0);
    return true;
}

static bool t_perf_frequency_xbox(void)
{
    /* Xbox perf counter rides on a CPU-TSC-derived divisor; the retail
     * kernel reports ~3,374,488 Hz (TSC/217). nxkrnl currently uses the
     * 3.579545 MHz ACPI PM timer. Accept anything in the 3-4 MHz band. */
    ULONGLONG f = KeQueryPerformanceFrequency();
    if (f < 3000000 || f > 4000000)
        FAIL_AND_RETURN("freq %llu Hz out of [3, 4] MHz", (unsigned long long)f);
    return true;
}

static bool t_perf_counter_monotonic(void)
{
    ULONGLONG a = KeQueryPerformanceCounter();
    for (volatile int i = 0; i < 200; i++) { /* burn a few ticks */ }
    ULONGLONG b = KeQueryPerformanceCounter();
    if (b <= a) {
        test_record_failure(__FILE__, __LINE__,
            "perf counter not monotonic: a=%llu b=%llu",
            (unsigned long long)a, (unsigned long long)b);
        return false;
    }
    return true;
}

static bool t_tick_count_advances(void)
{
    ULONG a = (ULONG)KeTickCount;
    LARGE_INTEGER d = { .QuadPart = -((LONGLONG)50 * 10000) }; /* 50 ms */
    NTSTATUS s = KeDelayExecutionThread(KernelMode, FALSE, &d);
    ASSERT_NTSTATUS(s, STATUS_SUCCESS);
    ULONG b = (ULONG)KeTickCount;
    if (b == a) {
        test_record_failure(__FILE__, __LINE__,
            "KeTickCount did not advance over 50ms: %lu", (unsigned long)a);
        return false;
    }
    return true;
}

static bool t_delay_returns_after_interval(void)
{
    ULONGLONG a = KeQueryPerformanceCounter();
    LARGE_INTEGER d = { .QuadPart = -((LONGLONG)100 * 10000) }; /* 100 ms */
    NTSTATUS s = KeDelayExecutionThread(KernelMode, FALSE, &d);
    ASSERT_NTSTATUS(s, STATUS_SUCCESS);
    ULONGLONG b = KeQueryPerformanceCounter();
    /* freq 3579545 Hz; 100 ms = ~357955 ticks. Accept >= 200000 to allow
     * timer-resolution slop. */
    if (b - a < 200000) {
        test_record_failure(__FILE__, __LINE__,
            "delay too short: %llu ticks", (unsigned long long)(b - a));
        return false;
    }
    return true;
}

static bool t_stall_returns(void)
{
    KeStallExecutionProcessor(50); /* 50 us */
    return true;
}

/* Earlier kernel iterations hung after ~500-1000 ms of accumulated KeDelay
 * (NxkMmEnsureXboxWindows clobbering MmSystemPteSpaceStart). Loop well past
 * that budget to make sure the wait/timer machinery stays alive. */
static bool t_delay_long_cumulative(void)
{
    LARGE_INTEGER d = { .QuadPart = -((LONGLONG)50 * 10000) }; /* 50 ms */
    ULONGLONG t0 = KeQueryPerformanceCounter();
    for (int i = 0; i < 24; i++) {  /* 1.2 s total */
        NTSTATUS s = KeDelayExecutionThread(KernelMode, FALSE, &d);
        if (!NT_SUCCESS(s)) FAIL_AND_RETURN("iter %d: status 0x%08x", i, (unsigned)s);
    }
    ULONGLONG t1 = KeQueryPerformanceCounter();
    /* Should have taken > 800 ms (perf freq 3579545 Hz). */
    if (t1 - t0 < 2800000) {
        FAIL_AND_RETURN("loop too fast: %llu ticks", (unsigned long long)(t1 - t0));
    }
    return true;
}


/* NtSetSystemTime drives the RTC write path.  Jump an hour ahead,
 * verify the clock followed and the previous time was returned, then
 * restore.  Tolerances are generous: the point is the path, not
 * precision. */
static bool t_set_system_time_roundtrip(void)
{
    LARGE_INTEGER before, target, prev = { .QuadPart = 0 }, now;
    KeQuerySystemTime(&before);

    target.QuadPart = before.QuadPart + (LONGLONG)3600 * 10000000;
    NTSTATUS s = NtSetSystemTime(&target, &prev);
    ASSERT_NTSTATUS(s, STATUS_SUCCESS);

    LONGLONG dprev = prev.QuadPart - before.QuadPart;
    if (dprev < 0)
        dprev = -dprev;

    KeQuerySystemTime(&now);
    LONGLONG dnow = now.QuadPart - target.QuadPart;
    if (dnow < 0)
        dnow = -dnow;

    /* Put the clock back before asserting anything. */
    s = NtSetSystemTime(&prev, NULL);
    ASSERT_NTSTATUS(s, STATUS_SUCCESS);

    ASSERT_TRUE(dprev < (LONGLONG)60 * 10000000);
    ASSERT_TRUE(dnow < (LONGLONG)60 * 10000000);
    return true;
}

static const test_entry_t ke_time_entries[] = {
    {"query_system_time_nonzero", t_query_system_time_nonzero},
    {"perf_frequency_xbox",       t_perf_frequency_xbox},
    {"perf_counter_monotonic",    t_perf_counter_monotonic},
    {"tick_count_advances",       t_tick_count_advances},
    {"delay_returns_after_interval", t_delay_returns_after_interval},
    {"delay_long_cumulative",     t_delay_long_cumulative},
    {"stall_returns",             t_stall_returns},
    {"set_system_time_roundtrip", t_set_system_time_roundtrip},
};

DEFINE_GROUP(ke_time, "ke/time");
