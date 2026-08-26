/*
 * RtlWalkFrameChain / RtlCaptureStackBackTrace / RtlGetCallersAddress --
 * the EBP-chain stack walkers.  The ordinals were unmapped bugcheck stubs.
 * The routines were already compiled but the kernel-mode walk stopped at
 * the first frame on the console: the generic ReactOS walker treats a
 * return address with a cleared top bit as a user-mode frame, and titles
 * load low, so the walk returned zero frames.  The console runs one flat
 * ring-0 space, so that gate is disabled for it and the surrounding
 * stack-bounds checks terminate the walk instead.
 *
 * The chains below are built from non-inlined helpers so the return
 * addresses are real; each captured frame must land inside the helper
 * that made the call.  Assertions are behavioural (non-zero frame counts,
 * addresses within the expected caller) so the same test proves the
 * routine on the retail oracle and on nxkrnl.
 */

#include "../harness.h"

/* A captured return address must fall inside the function that made the
 * call; helpers are small, so a generous window off the entry suffices. */
#define IN_FN(addr, fn) \
    ((ULONG_PTR)(addr) >= (ULONG_PTR)(fn) && \
     (ULONG_PTR)(addr) < (ULONG_PTR)(fn) + 0x400)

/* ---- RtlWalkFrameChain: a three-deep chain, walked from the bottom ---- */

static ULONG g_wfc_count;
static PVOID g_wfc_frames[16];

static __attribute__((noinline)) void wfc_leaf(void)
{
    g_wfc_count = RtlWalkFrameChain(g_wfc_frames, 16, 0);
}
static __attribute__((noinline)) void wfc_mid(void) { wfc_leaf(); }
static __attribute__((noinline)) void wfc_top(void) { wfc_mid(); }

static bool t_walk_frame_chain(void)
{
    g_wfc_count = 0;
    wfc_top();

    /* The walk must escape the first frame (the console-specific fix). */
    ASSERT_TRUE(g_wfc_count >= 3);

    /* The innermost return addresses climb wfc_leaf -> wfc_mid -> wfc_top. */
    ASSERT_TRUE(IN_FN(g_wfc_frames[0], wfc_leaf));
    ASSERT_TRUE(IN_FN(g_wfc_frames[1], wfc_mid));
    ASSERT_TRUE(IN_FN(g_wfc_frames[2], wfc_top));
    return true;
}

/* ---- RtlCaptureStackBackTrace: skip self, capture the callers ---- */

static ULONG g_csb_count;
static PVOID g_csb_frames[16];

static __attribute__((noinline)) void csb_leaf(void)
{
    /* Skip 0 -> the first captured frame is csb_leaf itself.  The hash out
     * pointer must be non-NULL: the retail routine writes through it
     * unconditionally (it faults on NULL, unlike the guarded NT form). */
    ULONG hash;
    g_csb_count = RtlCaptureStackBackTrace(0, 8, g_csb_frames, &hash);
}
static __attribute__((noinline)) void csb_mid(void) { csb_leaf(); }
static __attribute__((noinline)) void csb_top(void) { csb_mid(); }

static bool t_capture_stack_backtrace(void)
{
    g_csb_count = 0;
    csb_top();

    ASSERT_TRUE(g_csb_count >= 3);
    ASSERT_TRUE(IN_FN(g_csb_frames[0], csb_leaf));
    ASSERT_TRUE(IN_FN(g_csb_frames[1], csb_mid));
    ASSERT_TRUE(IN_FN(g_csb_frames[2], csb_top));
    return true;
}

static bool t_capture_backtrace_hash(void)
{
    PVOID frames[8];
    ULONG hash = 0;
    USHORT n = RtlCaptureStackBackTrace(0, 8, frames, &hash);

    /* Frames were captured and the caller-supplied hash was populated from
     * their (non-zero) code addresses. */
    ASSERT_TRUE(n >= 1);
    ASSERT_TRUE(hash != 0);
    return true;
}

/* ---- RtlGetCallersAddress: caller and caller's caller ---- */

static PVOID g_gca_addr, g_gca_caller;

static __attribute__((noinline)) void gca_leaf(void)
{
    RtlGetCallersAddress(&g_gca_addr, &g_gca_caller);
}
static __attribute__((noinline)) void gca_mid(void) { gca_leaf(); }
static __attribute__((noinline)) void gca_top(void) { gca_mid(); }

static bool t_get_callers_address(void)
{
    g_gca_addr = g_gca_caller = NULL;
    gca_top();

    /* CallersAddress is gca_leaf's caller (gca_mid); CallersCaller its
     * caller (gca_top). */
    ASSERT_NOT_NULL(g_gca_addr);
    ASSERT_NOT_NULL(g_gca_caller);
    ASSERT_TRUE(IN_FN(g_gca_addr, gca_mid));
    ASSERT_TRUE(IN_FN(g_gca_caller, gca_top));
    return true;
}

static const test_entry_t rtl_framewalk_entries[] = {
    {"walk_frame_chain", t_walk_frame_chain},
    {"capture_stack_backtrace", t_capture_stack_backtrace},
    {"capture_backtrace_hash", t_capture_backtrace_hash},
    {"get_callers_address", t_get_callers_address},
};

DEFINE_GROUP(rtl_framewalk, "rtl/framewalk");
