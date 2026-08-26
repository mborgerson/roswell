/*
 * ExRaiseStatus + ExRaiseException.
 *
 * The Xbox aliases for RtlRaiseStatus / RtlRaiseException: they build a
 * software EXCEPTION_RECORD and drive it through the same dispatch chain
 * a hardware trap uses, so a title's frame-based (__try/__except) handler
 * catches it.  We verify the exception code the handler observes matches
 * what was raised, and that the raise never returns to the caller.
 */

#include "../harness.h"
#include <excpt.h>
#include <string.h>

#ifndef STATUS_INVALID_PARAMETER
#define STATUS_INVALID_PARAMETER ((NTSTATUS)0xC000000DL)
#endif

static volatile ULONG g_caught;
static volatile int   g_returned;

static bool t_raise_status(void)
{
    g_caught = 0;
    g_returned = 0;
    __try {
        ExRaiseStatus(STATUS_INVALID_PARAMETER);
        g_returned = 1;
    } __except (g_caught = (ULONG)_exception_code(),
                EXCEPTION_EXECUTE_HANDLER) {
    }
    ASSERT_EQ_U32(g_returned, 0);
    ASSERT_EQ_U32(g_caught, (ULONG)STATUS_INVALID_PARAMETER);
    return true;
}

static bool t_raise_exception(void)
{
    EXCEPTION_RECORD rec;

    memset(&rec, 0, sizeof(rec));
    rec.ExceptionCode = (NTSTATUS)0xE0DEAD42;
    rec.NumberParameters = 2;
    rec.ExceptionInformation[0] = 0xAAAA5555;
    rec.ExceptionInformation[1] = 0x12345678;

    g_caught = 0;
    g_returned = 0;
    __try {
        ExRaiseException(&rec);
        g_returned = 1;
    } __except (g_caught = (ULONG)_exception_code(),
                EXCEPTION_EXECUTE_HANDLER) {
    }
    ASSERT_EQ_U32(g_returned, 0);
    ASSERT_EQ_U32(g_caught, 0xE0DEAD42);
    return true;
}

static const test_entry_t ex_raise_entries[] = {
    {"raise_status",    t_raise_status},
    {"raise_exception", t_raise_exception},
};

DEFINE_GROUP(ex_raise, "ex/raise");
