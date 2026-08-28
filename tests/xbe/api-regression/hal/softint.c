/*
 * HalRequestSoftwareInterrupt / HalClearSoftwareInterrupt -- the two
 * ends of the pending-interrupt bit the IRQL machinery runs on.
 *
 * APC and DPC delivery is not driven by the queues themselves but by a
 * request the HAL records and honours as soon as the IRQL drops below
 * the requested level.  So a title can watch both halves directly:
 * clearing the request under a raised IRQL leaves the queued work
 * sitting there when the IRQL comes back down, and asking for it again
 * runs it on the spot.
 *
 * Only the APC side is poked here.  Clearing the dispatch request with
 * a DPC already queued takes the console down: the queue has no other
 * way to be noticed, and nothing puts the request back.  Every case
 * below restores what it cleared before it returns.
 */

#include "../harness.h"

static volatile LONG g_apc_calls;

static void NTAPI kernel_routine(PKAPC Apc, PKNORMAL_ROUTINE *NormalRoutine,
                                 PVOID *NormalContext, PVOID *SystemArgument1,
                                 PVOID *SystemArgument2)
{
    (void)Apc;
    (void)NormalRoutine;
    (void)NormalContext;
    (void)SystemArgument1;
    (void)SystemArgument2;
    g_apc_calls++;
}

/* A kernel APC queued at DISPATCH with the request cleared underneath
 * it does not arrive when the IRQL drops -- and does as soon as the
 * request is put back. */
static bool t_a_cleared_request_holds_an_apc_back(void)
{
    KIRQL old;
    KAPC apc;
    LONG held;

    g_apc_calls = 0;
    old = KeRaiseIrqlToDpcLevel();
    KeInitializeApc(&apc, KeGetCurrentThread(), kernel_routine, NULL, NULL,
                    KernelMode, NULL);
    KeInsertQueueApc(&apc, NULL, NULL, 0);
    HalClearSoftwareInterrupt(APC_LEVEL);
    KfLowerIrql(old);
    held = g_apc_calls;

    HalRequestSoftwareInterrupt(APC_LEVEL);

    ASSERT_EQ_U32(KeGetCurrentIrql(), PASSIVE_LEVEL);
    ASSERT_EQ_U32(held, 0);
    ASSERT_EQ_U32(g_apc_calls, 1);
    return true;
}

/* A request the current IRQL already covers is not honoured until the
 * IRQL comes back down. */
static bool t_a_request_above_the_current_irql_waits(void)
{
    KIRQL old;
    KAPC apc;
    LONG under_lock;

    g_apc_calls = 0;
    old = KeRaiseIrqlToDpcLevel();
    KeInitializeApc(&apc, KeGetCurrentThread(), kernel_routine, NULL, NULL,
                    KernelMode, NULL);
    KeInsertQueueApc(&apc, NULL, NULL, 0);
    HalRequestSoftwareInterrupt(APC_LEVEL);
    under_lock = g_apc_calls;
    KfLowerIrql(old);

    ASSERT_EQ_U32(under_lock, 0);
    ASSERT_EQ_U32(g_apc_calls, 1);
    return true;
}

/* Asking for one with nothing queued costs nothing and changes no
 * IRQL. */
static bool t_a_request_with_nothing_queued_is_harmless(void)
{
    g_apc_calls = 0;

    HalRequestSoftwareInterrupt(APC_LEVEL);

    ASSERT_EQ_U32(KeGetCurrentIrql(), PASSIVE_LEVEL);
    ASSERT_EQ_U32(g_apc_calls, 0);
    return true;
}

static const test_entry_t hal_softint_entries[] = {
    { "a_cleared_request_holds_an_apc_back",
      t_a_cleared_request_holds_an_apc_back, NULL },
    { "a_request_above_the_current_irql_waits",
      t_a_request_above_the_current_irql_waits, NULL },
    { "a_request_with_nothing_queued_is_harmless",
      t_a_request_with_nothing_queued_is_harmless, NULL },
};

DEFINE_GROUP(hal_softint, "hal/softint");
