/*
 * KiUnlockDispatcherDatabase -- the way out of the dispatcher lock.
 *
 * There is no matching lock entry point: on a single processor holding
 * the dispatcher database is holding DISPATCH_LEVEL, so a caller raises
 * with KeRaiseIrqlToDpcLevel and hands the IRQL it got back to this
 * routine.  What the routine adds over a plain lower is the scheduling
 * work that was deferred while the lock was held, which for a title
 * shows up as the kernel APC that could not be delivered at DISPATCH
 * arriving as the IRQL comes back down.
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

static void queue_kernel_apc(KAPC *apc)
{
    g_apc_calls = 0;
    KeInitializeApc(apc, KeGetCurrentThread(), kernel_routine, NULL, NULL,
                    KernelMode, NULL);
    KeInsertQueueApc(apc, NULL, NULL, 0);
}

/* The IRQL it is handed is the IRQL it leaves behind. */
static bool t_it_lowers_to_the_irql_it_was_given(void)
{
    KIRQL old;

    ASSERT_EQ_U32(KeGetCurrentIrql(), PASSIVE_LEVEL);

    old = KeRaiseIrqlToDpcLevel();
    ASSERT_EQ_U32(old, PASSIVE_LEVEL);
    ASSERT_EQ_U32(KeGetCurrentIrql(), DISPATCH_LEVEL);

    KiUnlockDispatcherDatabase(old);
    ASSERT_EQ_U32(KeGetCurrentIrql(), PASSIVE_LEVEL);
    return true;
}

/* Handed DISPATCH_LEVEL it stays there -- the caller that was already
 * holding the lock keeps holding it. */
static bool t_it_leaves_the_irql_alone_when_told_to_stay(void)
{
    KIRQL outer, inner;

    outer = KeRaiseIrqlToDpcLevel();
    inner = KeRaiseIrqlToDpcLevel();
    if (inner != DISPATCH_LEVEL) {
        KfLowerIrql(outer);
        FAIL_AND_RETURN("nested raise reported irql %u", (unsigned)inner);
    }

    KiUnlockDispatcherDatabase(inner);
    if (KeGetCurrentIrql() != DISPATCH_LEVEL) {
        KfLowerIrql(outer);
        FAIL_AND_RETURN("irql %u after the unlock",
                        (unsigned)KeGetCurrentIrql());
    }

    KfLowerIrql(outer);
    ASSERT_EQ_U32(KeGetCurrentIrql(), PASSIVE_LEVEL);
    return true;
}

/* An APC queued while the lock is held cannot run there; the unlock is
 * where it arrives. */
static bool t_a_kernel_apc_queued_under_it_runs_on_the_way_out(void)
{
    KIRQL old;
    KAPC apc;
    LONG under_lock;

    old = KeRaiseIrqlToDpcLevel();
    queue_kernel_apc(&apc);
    under_lock = g_apc_calls;
    KiUnlockDispatcherDatabase(old);

    ASSERT_EQ_U32(KeGetCurrentIrql(), PASSIVE_LEVEL);
    ASSERT_EQ_U32(under_lock, 0);
    ASSERT_EQ_U32(g_apc_calls, 1);
    return true;
}

/* And an unlock that stays at DISPATCH leaves it waiting. */
static bool t_an_apc_waits_while_it_stays_at_dispatch(void)
{
    KIRQL outer, inner;
    KAPC apc;
    LONG still_held;

    outer = KeRaiseIrqlToDpcLevel();
    inner = KeRaiseIrqlToDpcLevel();
    queue_kernel_apc(&apc);
    KiUnlockDispatcherDatabase(inner);
    still_held = g_apc_calls;
    KfLowerIrql(outer);

    ASSERT_EQ_U32(KeGetCurrentIrql(), PASSIVE_LEVEL);
    ASSERT_EQ_U32(still_held, 0);
    ASSERT_EQ_U32(g_apc_calls, 1);
    return true;
}

static const test_entry_t ke_dispatcher_entries[] = {
    { "it_lowers_to_the_irql_it_was_given",
      t_it_lowers_to_the_irql_it_was_given, NULL },
    { "it_leaves_the_irql_alone_when_told_to_stay",
      t_it_leaves_the_irql_alone_when_told_to_stay, NULL },
    { "a_kernel_apc_queued_under_it_runs_on_the_way_out",
      t_a_kernel_apc_queued_under_it_runs_on_the_way_out, NULL },
    { "an_apc_waits_while_it_stays_at_dispatch",
      t_an_apc_waits_while_it_stays_at_dispatch, NULL },
};

DEFINE_GROUP(ke_dispatcher, "ke/dispatcher");
