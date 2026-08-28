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

/* Records a failure and evaluates to false, so a case that has already
 * moved the title's priority can note it and still put it back. */
#define SET_FAIL(fmt, ...) \
    (test_record_failure(__FILE__, __LINE__, fmt, ##__VA_ARGS__), false)

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

/* --- KeSetPriorityProcess -----------------------------------------------
 *
 * Every case here moves the running title's own priority, so each one
 * puts it back.  Only values inside 1..15 are used: outside that the
 * console stores the value but clamps the threads, and the clamp does
 * not undo -- a probe that went out of range left this title's threads
 * two priorities off and failed the next case in the group.  Those
 * shapes are written down in the handoff rather than exercised here.
 */
static const char *const NO_OTHER_THREADS =
    "only the calling thread follows the process: there is no list of the "
    "title's threads to walk";

/* It answers with the priority the process had, and takes the new one. */
static bool t_setting_the_priority_reports_the_old_one(void)
{
    PKTHREAD t = KeGetCurrentThread();
    PKPROCESS p = t->ApcState.Process;
    LONG original, ret;
    bool ok = true;

    ASSERT_NOT_NULL(p);
    original = p->BasePriority;
    ASSERT_TRUE(original >= 1 && original <= 14);

    ret = KeSetPriorityProcess(p, original + 1);
    if (ret != original)
        ok = SET_FAIL("set returned %d, the process was at %d", (int)ret,
                      (int)original);
    else if (p->BasePriority != original + 1)
        ok = SET_FAIL("the process reads %d after being set to %d",
                      (int)p->BasePriority, (int)(original + 1));

    ret = KeSetPriorityProcess(p, original);
    if (ok && ret != original + 1)
        ok = SET_FAIL("restore returned %d, the process was at %d",
                      (int)ret, (int)(original + 1));
    if (ok && p->BasePriority != original)
        ok = SET_FAIL("the process did not come back to %d", (int)original);
    return ok;
}

/* The thread that calls it moves with the process. */
static bool t_the_calling_thread_follows_the_process(void)
{
    PKTHREAD t = KeGetCurrentThread();
    PKPROCESS p = t->ApcState.Process;
    LONG original;
    bool ok = true;

    ASSERT_NOT_NULL(p);
    original = p->BasePriority;
    ASSERT_TRUE(original >= 2 && original <= 14);

    KeSetPriorityProcess(p, original + 1);
    if (t->Priority != original + 1 || t->BasePriority != original + 1)
        ok = SET_FAIL("thread at pri=%d base=%d after the process moved to %d",
                      (int)t->Priority, (int)t->BasePriority,
                      (int)(original + 1));

    KeSetPriorityProcess(p, original);
    if (ok && (t->Priority != original || t->BasePriority != original))
        ok = SET_FAIL("thread did not come back: pri=%d base=%d, wanted %d",
                      (int)t->Priority, (int)t->BasePriority, (int)original);
    return ok;
}

/* Setting it to what it already is changes nothing and still reports. */
static bool t_setting_the_same_priority_is_a_no_op(void)
{
    PKTHREAD t = KeGetCurrentThread();
    PKPROCESS p = t->ApcState.Process;
    LONG original = p->BasePriority;

    ASSERT_NOT_NULL(p);
    ASSERT_EQ_U32(KeSetPriorityProcess(p, original), (ULONG)original);
    ASSERT_EQ_U32(p->BasePriority, (ULONG)original);
    ASSERT_EQ_U32(t->BasePriority, (ULONG)original);
    return true;
}

/* On the console the whole title moves, not just whoever called. */
static volatile LONG g_worker_priority;
static KEVENT g_worker_done;

static void NTAPI procprio_system_routine(PKSTART_ROUTINE StartRoutine,
                                          PVOID StartContext)
{
    if (StartRoutine != NULL)
        StartRoutine(StartContext);
    PsTerminateSystemThread(STATUS_SUCCESS);
}

static VOID NTAPI priority_worker(PVOID ctx)
{
    (void)ctx;
    g_worker_priority = KeGetCurrentThread()->BasePriority;
    KeSetEvent(&g_worker_done, 0, FALSE);
}

static bool t_a_new_thread_starts_at_the_process_priority(void)
{
    PKTHREAD t = KeGetCurrentThread();
    PKPROCESS p = t->ApcState.Process;
    LARGE_INTEGER timeout;
    LONG original;
    HANDLE h = NULL;
    NTSTATUS s;
    bool ok = true;

    ASSERT_NOT_NULL(p);
    original = p->BasePriority;
    ASSERT_TRUE(original >= 2 && original <= 14);

    g_worker_priority = -1;
    KeInitializeEvent(&g_worker_done, NotificationEvent, FALSE);

    KeSetPriorityProcess(p, original + 1);
    s = PsCreateSystemThreadEx(&h, 0, 0, 0, NULL, priority_worker, NULL,
                               FALSE, FALSE,
                               procprio_system_routine);
    if (NT_SUCCESS(s)) {
        timeout.QuadPart = -((LONGLONG)2 * 1000 * 10000);
        KeWaitForSingleObject(&g_worker_done, Executive, KernelMode, FALSE,
                              &timeout);
        NtClose(h);
    }

    KeSetPriorityProcess(p, original);

    if (!NT_SUCCESS(s))
        FAIL_AND_RETURN("could not start a thread: 0x%08x", (unsigned)s);
    if (g_worker_priority != original + 1)
        ok = SET_FAIL("a thread started while the process was at %d came up "
                      "at %d", (int)(original + 1), (int)g_worker_priority);
    return ok;
}

static const test_entry_t ke_procprio_entries[] = {
    { "setting_the_priority_reports_the_old_one",
      t_setting_the_priority_reports_the_old_one, NULL },
    { "the_calling_thread_follows_the_process",
      t_the_calling_thread_follows_the_process, NULL },
    { "setting_the_same_priority_is_a_no_op",
      t_setting_the_same_priority_is_a_no_op, NULL },
    { "a_new_thread_starts_at_the_process_priority",
      t_a_new_thread_starts_at_the_process_priority, NO_OTHER_THREADS },
    { "the_thread_names_its_process", t_the_thread_names_its_process, NULL },
    { "the_process_has_the_console_layout",
      t_the_process_has_the_console_layout, NULL },
    { "the_thread_starts_at_the_process_base",
      t_the_thread_starts_at_the_process_base, NULL },
    { "the_process_lists_its_threads", t_the_process_lists_its_threads,
      NO_THREAD_LIST },
};

DEFINE_GROUP(ke_procprio, "ke/procprio");
