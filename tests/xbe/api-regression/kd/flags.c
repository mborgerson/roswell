/*
 * Kd debugger-presence DATA-ordinal exports. The retail Xbox kernel keeps
 * its debug-print machinery compiled in, so both flags read TRUE on a cold
 * boot with no remote debugger: KdDebuggerEnabled reports that debug output
 * is active, KdDebuggerNotPresent that nothing is attached to break into.
 * Titles gate their break-on-assert on the pair, so a stubbed export reading
 * zero would silently change that decision. Values verified on the retail
 * kernel (1.0.4627) via api-regression-run --official.
 */

#include "../harness.h"

static bool t_debugger_enabled(void)
{
    if (KdDebuggerEnabled != TRUE)
        FAIL_AND_RETURN("KdDebuggerEnabled: got %u expected TRUE",
                        (unsigned)KdDebuggerEnabled);
    return true;
}

static bool t_debugger_not_present(void)
{
    if (KdDebuggerNotPresent != TRUE)
        FAIL_AND_RETURN("KdDebuggerNotPresent: got %u expected TRUE",
                        (unsigned)KdDebuggerNotPresent);
    return true;
}

static const test_entry_t kd_flags_entries[] = {
    {"debugger_enabled",      t_debugger_enabled},
    {"debugger_not_present",  t_debugger_not_present},
};

DEFINE_GROUP(kd_flags, "kd/flags");
