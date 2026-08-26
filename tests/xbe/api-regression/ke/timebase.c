/*
 * The Xbox kernel's timebase DATA exports. KeTimeIncrement is the nominal
 * 100ns period of a clock tick; titles multiply their tick deltas by it to
 * convert to time, so a stubbed export reading zero would break that math.
 * Value verified on the retail kernel (1.0.4627) via
 * api-regression-run --official.
 */

#include "../harness.h"

static bool t_time_increment(void)
{
    /* Retail reports exactly 10000 (1 ms in 100ns units). */
    ASSERT_EQ_U32(KeTimeIncrement, 10000);
    return true;
}

static const test_entry_t ke_timebase_entries[] = {
    {"time_increment", t_time_increment},
};

DEFINE_GROUP(ke_timebase, "ke/timebase");
