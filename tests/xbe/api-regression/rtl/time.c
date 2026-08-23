/*
 * RtlTimeFieldsToTime / RtlTimeToTimeFields round-trip.
 */

#include "../harness.h"

static bool t_known_epoch(void)
{
    /* Jan 1 1601 UTC == 0 ticks since the Windows epoch. */
    TIME_FIELDS tf = { .Year = 1601, .Month = 1, .Day = 1,
                       .Hour = 0, .Minute = 0, .Second = 0,
                       .Milliseconds = 0, .Weekday = 0 };
    LARGE_INTEGER t;
    ASSERT_TRUE(RtlTimeFieldsToTime(&tf, &t));
    ASSERT_EQ_U32(t.LowPart, 0);
    ASSERT_EQ_U32(t.HighPart, 0);
    return true;
}

static bool t_roundtrip(void)
{
    TIME_FIELDS tf_in = { .Year = 2026, .Month = 6, .Day = 8,
                          .Hour = 12, .Minute = 34, .Second = 56,
                          .Milliseconds = 0, .Weekday = 0 };
    LARGE_INTEGER t;
    ASSERT_TRUE(RtlTimeFieldsToTime(&tf_in, &t));

    TIME_FIELDS tf_out;
    RtlTimeToTimeFields(&t, &tf_out);
    ASSERT_EQ_U32(tf_out.Year,   tf_in.Year);
    ASSERT_EQ_U32(tf_out.Month,  tf_in.Month);
    ASSERT_EQ_U32(tf_out.Day,    tf_in.Day);
    ASSERT_EQ_U32(tf_out.Hour,   tf_in.Hour);
    ASSERT_EQ_U32(tf_out.Minute, tf_in.Minute);
    ASSERT_EQ_U32(tf_out.Second, tf_in.Second);
    return true;
}

static bool t_invalid(void)
{
    TIME_FIELDS bad = { .Year = 1500, .Month = 1, .Day = 1 };
    LARGE_INTEGER t;
    ASSERT_TRUE(!RtlTimeFieldsToTime(&bad, &t));
    return true;
}

static const test_entry_t rtl_time_entries[] = {
    {"known_epoch", t_known_epoch},
    {"roundtrip",   t_roundtrip},
    {"invalid",     t_invalid},
};

DEFINE_GROUP(rtl_time, "rtl/time");
