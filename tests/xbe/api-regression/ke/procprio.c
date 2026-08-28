/*
 * The process object a title reaches through its own thread.
 *
 * The console publishes KPROCESS to titles: the current thread's APC
 * state names it, and its base priority is what every thread in the
 * title starts from.  It is also the only handle a title has on the
 * process, so it is what 147 KeSetPriorityProcess would be called with.
 *
 * Our KTHREAD is still NT's rather than the console's, so a title
 * reading these fields reads the wrong offsets and finds zeros.  The
 * cases below assert what the console does and are marked TODO for
 * that: they are the gate on mapping KeSetPriorityProcess, which cannot
 * be reached with a usable pointer until the thread layout lands.
 */

#include "../harness.h"

static const char *const NO_KTHREAD_LAYOUT =
    "our KTHREAD is NT's, so the console's offsets read as zero";

/* The thread names the process it belongs to. */
static bool t_the_thread_names_its_process(void)
{
    PKTHREAD t = KeGetCurrentThread();

    ASSERT_NOT_NULL(t);
    ASSERT_NOT_NULL(t->ApcState.Process);
    return true;
}

/* And the process has the console's shape: an empty ready list, at
 * least this thread's stack, a quantum, and a base priority inside the
 * dynamic range. */
static bool t_the_process_has_the_console_layout(void)
{
    PKTHREAD t = KeGetCurrentThread();
    PKPROCESS p = t->ApcState.Process;

    ASSERT_NOT_NULL(p);
    ASSERT_EQ_PTR(p->ReadListHead.Flink, &p->ReadListHead);
    ASSERT_EQ_PTR(p->ReadListHead.Blink, &p->ReadListHead);
    ASSERT_TRUE(p->ThreadListHead.Flink != &p->ThreadListHead);
    ASSERT_TRUE(p->StackCount >= 1);
    ASSERT_TRUE(p->ThreadQuantum > 0);
    ASSERT_TRUE(p->BasePriority >= 1 && p->BasePriority <= 15);
    return true;
}

/* A title's thread runs at the process' base priority until something
 * moves it. */
static bool t_the_thread_starts_at_the_process_base(void)
{
    PKTHREAD t = KeGetCurrentThread();
    PKPROCESS p = t->ApcState.Process;

    ASSERT_NOT_NULL(p);
    ASSERT_EQ_U32(t->BasePriority, p->BasePriority);
    ASSERT_EQ_U32(t->Priority, p->BasePriority);
    return true;
}

static const test_entry_t ke_procprio_entries[] = {
    { "the_thread_names_its_process", t_the_thread_names_its_process,
      NO_KTHREAD_LAYOUT },
    { "the_process_has_the_console_layout",
      t_the_process_has_the_console_layout, NO_KTHREAD_LAYOUT },
    { "the_thread_starts_at_the_process_base",
      t_the_thread_starts_at_the_process_base, NO_KTHREAD_LAYOUT },
};

DEFINE_GROUP(ke_procprio, "ke/procprio");
