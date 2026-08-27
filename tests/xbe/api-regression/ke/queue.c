/*
 * The kernel queue object, as titles see it.
 *
 * A KQUEUE is a dispatcher object with a FIFO of list entries and a set
 * of threads associated with it: removing from an empty queue blocks the
 * caller until something is inserted.  The XAPI builds its worker-thread
 * pools on this, so both the structure and the ownership protocol are
 * title-visible.
 *
 * Every case that calls KeRemoveQueue must run the queue down before it
 * returns.  KeRemoveQueue associates the calling thread with the queue
 * for good, and the queues here live on the stack -- leaving the thread
 * pointing at a dead one would poison every later test.
 */

#include "../harness.h"
#include <string.h>

/* KOBJECTS: the value the retail kernel stamps into a queue. */
#define QUEUE_OBJECT 4

#define GUARD 0x5A5A5A5Au

#ifndef STATUS_TIMEOUT
#define STATUS_TIMEOUT ((NTSTATUS)0x00000102L)
#endif

typedef struct {
    KQUEUE queue;
    ULONG guard;
} guarded_queue_t;

static bool list_is_self(const LIST_ENTRY *head)
{
    return head->Flink == head && head->Blink == head;
}

static bool t_initialize(void)
{
    guarded_queue_t g;

    memset(&g, 0xCC, sizeof(g));
    g.guard = GUARD;

    KeInitializeQueue(&g.queue, 0);

    ASSERT_EQ_U32(sizeof(KQUEUE), 40);
    ASSERT_EQ_U32(g.queue.Header.Type, QUEUE_OBJECT);
    ASSERT_EQ_U32(g.queue.Header.Size, sizeof(KQUEUE) / sizeof(ULONG));
    ASSERT_EQ_U32(g.queue.Header.SignalState, 0);
    ASSERT_TRUE(list_is_self(&g.queue.Header.WaitListHead));
    ASSERT_TRUE(list_is_self(&g.queue.EntryListHead));
    ASSERT_TRUE(list_is_self(&g.queue.ThreadListHead));
    ASSERT_EQ_U32(g.queue.CurrentCount, 0);

    /* A zero count means one concurrent thread per processor. */
    ASSERT_EQ_U32(g.queue.MaximumCount, 1);

    ASSERT_EQ_U32(g.guard, GUARD);
    return true;
}

static bool t_initialize_explicit_count(void)
{
    KQUEUE q;

    KeInitializeQueue(&q, 3);
    ASSERT_EQ_U32(q.MaximumCount, 3);
    return true;
}

static bool t_insert_signals(void)
{
    KQUEUE q;
    LIST_ENTRY a, b;
    LONG prev_a, prev_b;

    KeInitializeQueue(&q, 0);
    prev_a = KeInsertQueue(&q, &a);
    prev_b = KeInsertQueue(&q, &b);

    /* Each insert returns the signal state before it, which counts the
     * entries waiting to be picked up. */
    ASSERT_EQ_U32(prev_a, 0);
    ASSERT_EQ_U32(prev_b, 1);
    ASSERT_EQ_U32(q.Header.SignalState, 2);
    ASSERT_EQ_PTR(q.EntryListHead.Flink, &a);
    ASSERT_EQ_PTR(q.EntryListHead.Blink, &b);

    KeRundownQueue(&q);
    return true;
}

static bool t_remove_is_fifo(void)
{
    KQUEUE q;
    LIST_ENTRY a, b;
    PLIST_ENTRY got_a, got_b;
    LARGE_INTEGER zero = { .QuadPart = 0 };

    KeInitializeQueue(&q, 0);
    KeInsertQueue(&q, &a);
    KeInsertQueue(&q, &b);

    got_a = KeRemoveQueue(&q, KernelMode, &zero);
    got_b = KeRemoveQueue(&q, KernelMode, &zero);

    ASSERT_EQ_PTR(got_a, &a);
    ASSERT_EQ_PTR(got_b, &b);
    ASSERT_EQ_U32(q.Header.SignalState, 0);

    KeRundownQueue(&q);
    return true;
}

static bool t_insert_head_jumps_the_line(void)
{
    KQUEUE q;
    LIST_ENTRY a, b;
    PLIST_ENTRY first;
    LARGE_INTEGER zero = { .QuadPart = 0 };

    KeInitializeQueue(&q, 0);
    KeInsertQueue(&q, &a);
    KeInsertHeadQueue(&q, &b);

    first = KeRemoveQueue(&q, KernelMode, &zero);
    ASSERT_EQ_PTR(first, &b);

    KeRundownQueue(&q);
    return true;
}

static bool t_remove_empty_times_out(void)
{
    KQUEUE q;
    LARGE_INTEGER zero = { .QuadPart = 0 };
    PLIST_ENTRY got;

    KeInitializeQueue(&q, 0);

    /* A zero timeout on an empty queue returns the status in place of an
     * entry rather than blocking. */
    got = KeRemoveQueue(&q, KernelMode, &zero);
    ASSERT_EQ_PTR(got, (PLIST_ENTRY)(ULONG_PTR)STATUS_TIMEOUT);

    KeRundownQueue(&q);
    return true;
}

static bool t_remove_associates_the_thread(void)
{
    KQUEUE q;
    LIST_ENTRY a;
    LARGE_INTEGER zero = { .QuadPart = 0 };

    KeInitializeQueue(&q, 0);
    KeInsertQueue(&q, &a);
    KeRemoveQueue(&q, KernelMode, &zero);

    /* The calling thread is now one of the queue's, and stays so until
     * the queue is run down. */
    ASSERT_TRUE(!list_is_self(&q.ThreadListHead));

    KeRundownQueue(&q);
    ASSERT_TRUE(list_is_self(&q.ThreadListHead));
    return true;
}

static bool t_rundown_returns_entries(void)
{
    KQUEUE q, empty;
    LIST_ENTRY a, b;
    PLIST_ENTRY left, none;

    KeInitializeQueue(&q, 0);
    KeInsertQueue(&q, &a);
    KeInsertQueue(&q, &b);

    /* Rundown hands back the entries still queued, as a list the caller
     * walks from the first one. */
    left = KeRundownQueue(&q);
    ASSERT_EQ_PTR(left, &a);
    ASSERT_EQ_PTR(left->Flink, &b);

    KeInitializeQueue(&empty, 0);
    none = KeRundownQueue(&empty);
    ASSERT_EQ_PTR(none, NULL);
    return true;
}

static const test_entry_t ke_queue_entries[] = {
    {"initialize",                t_initialize},
    {"initialize_explicit_count", t_initialize_explicit_count},
    {"insert_signals",            t_insert_signals},
    {"remove_is_fifo",            t_remove_is_fifo},
    {"insert_head_jumps_the_line", t_insert_head_jumps_the_line},
    {"remove_empty_times_out",    t_remove_empty_times_out},
    {"remove_associates_the_thread", t_remove_associates_the_thread},
    {"rundown_returns_entries",   t_rundown_returns_entries},
};

DEFINE_GROUP(ke_queue, "ke/queue");
