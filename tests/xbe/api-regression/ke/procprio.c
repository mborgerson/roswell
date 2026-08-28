/*
 * The process object a title reaches through its own thread.
 *
 * The console publishes KPROCESS to titles: the current thread's APC
 * state names it, and its base priority is what every thread in the
 * title starts from.  It is also the only handle a title has on the
 * process, so it is what 147 KeSetPriorityProcess would be called with.
 *
 * Our threads are NT's, so what a title sees is an Xbox-shaped shadow
 * that the kernel fills at the console's offsets (xb/procobj.c).  The
 * one thing it does not fill is the process' thread list: linking into
 * it needs a field past the end of the shadow and a hook on every way
 * out of a thread, so it is published empty and the case that expects
 * it non-empty stays a documented divergence rather than a fiction.
 */

#include "../harness.h"

static const char *const NO_THREAD_LIST =
    "the process' thread list is published empty: linking into it needs "
    "KTHREAD.ThreadListEntry, past the end of the shadow";

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
    ASSERT_TRUE(p->StackCount >= 1);
    ASSERT_TRUE(p->ThreadQuantum > 0);
    ASSERT_TRUE(p->BasePriority >= 1 && p->BasePriority <= 15);
    return true;
}

/* The console links every thread of the title into the process, so the
 * list is never empty while a thread is running to read it. */
static bool t_the_process_lists_its_threads(void)
{
    PKTHREAD t = KeGetCurrentThread();
    PKPROCESS p = t->ApcState.Process;

    ASSERT_NOT_NULL(p);
    ASSERT_TRUE(p->ThreadListHead.Flink != &p->ThreadListHead);
    ASSERT_TRUE(p->ThreadListHead.Blink != &p->ThreadListHead);
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
    { "the_thread_names_its_process", t_the_thread_names_its_process, NULL },
    { "the_process_has_the_console_layout",
      t_the_process_has_the_console_layout, NULL },
    { "the_thread_starts_at_the_process_base",
      t_the_thread_starts_at_the_process_base, NULL },
    { "the_process_lists_its_threads", t_the_process_lists_its_threads,
      NO_THREAD_LIST },
};

DEFINE_GROUP(ke_procprio, "ke/procprio");
