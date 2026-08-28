/*
 * IoQueueThreadIrp -- the hook a title driver uses to make a packet it
 * dispatched itself cancellable with the thread that owns it.
 *
 * The packet carries the thread in Tail.Overlay.Thread and the list
 * membership in its own ThreadListEntry, so the ordering can be pinned
 * without knowing where the list head sits inside the thread object:
 * queue two packets and read which way the links run.  Everything here
 * is undone before the test returns -- a packet left on a live thread's
 * list outlives the memory it is freed from.
 */

#include "../harness.h"
#include <string.h>

#define SELF_THREAD ((HANDLE)(LONG_PTR)-2)

static PETHREAD self_thread(void)
{
    PETHREAD t = NULL;
    if (!NT_SUCCESS(ObReferenceObjectByHandle(SELF_THREAD, &PsThreadObjectType,
                                              (PVOID *)&t)))
        return NULL;
    return t;
}

static bool queued(PIRP irp)
{
    return irp->ThreadListEntry.Flink != &irp->ThreadListEntry;
}

/* Plain list surgery on the packet's own entry.  Nothing else on this
 * thread has I/O in flight while the test runs, so there is no list to
 * race with. */
static void unqueue(PIRP irp)
{
    if (queued(irp)) {
        irp->ThreadListEntry.Blink->Flink = irp->ThreadListEntry.Flink;
        irp->ThreadListEntry.Flink->Blink = irp->ThreadListEntry.Blink;
    }
    irp->ThreadListEntry.Flink = &irp->ThreadListEntry;
    irp->ThreadListEntry.Blink = &irp->ThreadListEntry;
}

static bool t_fresh_packet_is_not_on_a_list(void)
{
    PIRP irp = IoAllocateIrp(1);

    ASSERT_NOT_NULL(irp);
    /* IoAllocateIrp leaves the entry empty: it points at itself. */
    if (queued(irp)) {
        IoFreeIrp(irp);
        FAIL_AND_RETURN("a fresh packet was already linked");
    }
    ASSERT_EQ_PTR(irp->ThreadListEntry.Blink, &irp->ThreadListEntry);
    IoFreeIrp(irp);
    return true;
}

static bool t_queue_links_the_packet(void)
{
    PETHREAD self = self_thread();
    PIRP irp;

    ASSERT_NOT_NULL(self);
    irp = IoAllocateIrp(1);
    ASSERT_NOT_NULL(irp);

    irp->Tail.Overlay.Thread = self;
    IoQueueThreadIrp(irp);

    bool linked = queued(irp);
    /* The neighbours must agree the packet is between them. */
    bool consistent = irp->ThreadListEntry.Flink->Blink ==
                          &irp->ThreadListEntry &&
                      irp->ThreadListEntry.Blink->Flink ==
                          &irp->ThreadListEntry;
    unqueue(irp);
    IoFreeIrp(irp);

    if (!linked) FAIL_AND_RETURN("the packet was not linked");
    if (!consistent) FAIL_AND_RETURN("the list links disagree");
    return true;
}

/* Two packets pin the insertion end without naming the list head. */
static bool t_second_queue_goes_in_front(void)
{
    PETHREAD self = self_thread();
    PIRP a, b;

    ASSERT_NOT_NULL(self);
    a = IoAllocateIrp(1);
    ASSERT_NOT_NULL(a);
    b = IoAllocateIrp(1);
    if (b == NULL) { IoFreeIrp(a); FAIL_AND_RETURN("second allocation failed"); }

    a->Tail.Overlay.Thread = self;
    b->Tail.Overlay.Thread = self;
    IoQueueThreadIrp(a);
    IoQueueThreadIrp(b);

    /* Head insertion puts the newest first, so b's forward link is a. */
    PLIST_ENTRY b_next = b->ThreadListEntry.Flink;
    PLIST_ENTRY a_prev = a->ThreadListEntry.Blink;
    unqueue(b);
    unqueue(a);
    IoFreeIrp(b);
    IoFreeIrp(a);

    ASSERT_EQ_PTR(b_next, &a->ThreadListEntry);
    ASSERT_EQ_PTR(a_prev, &b->ThreadListEntry);
    return true;
}

/* Queueing does not disturb the rest of the header. */
static bool t_queue_touches_nothing_else(void)
{
    PETHREAD self = self_thread();
    PIRP irp;

    ASSERT_NOT_NULL(self);
    irp = IoAllocateIrp(1);
    ASSERT_NOT_NULL(irp);

    irp->Tail.Overlay.Thread = self;
    irp->IoStatus.Status = (NTSTATUS)0x5A5A5A5A;
    irp->IoStatus.Information = 0xA5A5A5A5;
    CSHORT type = irp->Type;
    USHORT size = irp->Size;
    CHAR loc = irp->CurrentLocation;

    IoQueueThreadIrp(irp);

    bool ok = irp->Type == type && irp->Size == size &&
              irp->CurrentLocation == loc &&
              irp->IoStatus.Status == (NTSTATUS)0x5A5A5A5A &&
              irp->IoStatus.Information == 0xA5A5A5A5 &&
              irp->Tail.Overlay.Thread == self;
    unqueue(irp);
    IoFreeIrp(irp);

    if (!ok) FAIL_AND_RETURN("queueing rewrote a header field");
    return true;
}

static const test_entry_t io_threadirp_entries[] = {
    { "fresh_packet_is_not_on_a_list", t_fresh_packet_is_not_on_a_list, NULL },
    { "queue_links_the_packet", t_queue_links_the_packet, NULL },
    { "second_queue_goes_in_front", t_second_queue_goes_in_front, NULL },
    { "queue_touches_nothing_else", t_queue_touches_nothing_else, NULL },
};

DEFINE_GROUP(io_threadirp, "io/threadirp");
