/*
 * The kernel device queue, as titles see it.
 *
 * Titles carry block-device drivers of their own, and every device
 * object a title creates embeds a KDEVICE_QUEUE at offset 0x28, so both
 * the 12-byte size and the packed Type/Size/Busy header are contract.
 * The guard word behind the caller's queue in t_initialize is the point
 * of that case: a 16- or 20-byte initializer would write a spin lock
 * past the end of the title's structure.
 *
 * The insert/remove routines run at DISPATCH_LEVEL, which is also the
 * whole exclusion on a uniprocessor console, so every case raises IRQL
 * around the queue operations.
 */

#include "../harness.h"
#include <string.h>

#define FILE_DEVICE_UNKNOWN 0x00000022u
#define MJ_SLOTS 0x0E

/* KOBJECTS: the value the retail kernel stamps into a device queue. */
#define DEVICE_QUEUE_OBJECT 20

#define GUARD 0xA5A5A5A5u

typedef struct {
    KDEVICE_QUEUE queue;
    ULONG guard;
} guarded_queue_t;

/* Raise to dispatch level for the duration of a queue operation. */
#define AT_DISPATCH(body) do { \
    KIRQL _old = KeRaiseIrqlToDpcLevel(); \
    body; \
    KfLowerIrql(_old); \
} while (0)

static bool list_is_self(const LIST_ENTRY *head)
{
    return head->Flink == head && head->Blink == head;
}

static bool t_initialize(void)
{
    guarded_queue_t g;

    memset(&g, 0xCC, sizeof(g));
    g.guard = GUARD;

    KeInitializeDeviceQueue(&g.queue);

    ASSERT_EQ_U32(g.queue.Type, DEVICE_QUEUE_OBJECT);
    ASSERT_EQ_U32(g.queue.Size, sizeof(KDEVICE_QUEUE));
    ASSERT_EQ_U32(sizeof(KDEVICE_QUEUE), 12);
    ASSERT_EQ_U32(g.queue.Busy, 0);
    ASSERT_TRUE(list_is_self(&g.queue.DeviceListHead));

    /* Nothing may be written past the title's 12-byte queue. */
    ASSERT_EQ_U32(g.guard, GUARD);
    return true;
}

static bool t_insert_remove(void)
{
    KDEVICE_QUEUE q;
    KDEVICE_QUEUE_ENTRY e1, e2;
    BOOLEAN ins1, ins2;
    PKDEVICE_QUEUE_ENTRY got1, got2;

    KeInitializeDeviceQueue(&q);
    memset(&e1, 0, sizeof(e1));
    memset(&e2, 0, sizeof(e2));

    AT_DISPATCH({
        ins1 = KeInsertDeviceQueue(&q, &e1);
        ins2 = KeInsertDeviceQueue(&q, &e2);
        got1 = KeRemoveDeviceQueue(&q);
        got2 = KeRemoveDeviceQueue(&q);
    });

    /* An idle queue takes ownership instead of queueing: the caller is
     * told to start the request itself. */
    ASSERT_EQ_U32(ins1, FALSE);
    ASSERT_EQ_U32(e1.Inserted, FALSE);

    /* The queue is now busy, so the second request really is queued. */
    ASSERT_EQ_U32(ins2, TRUE);

    /* ...and comes back out, marked non-inserted. */
    ASSERT_EQ_PTR(got1, &e2);
    ASSERT_EQ_U32(e2.Inserted, FALSE);

    /* Draining an empty queue returns nothing and releases it. */
    ASSERT_EQ_PTR(got2, NULL);
    ASSERT_EQ_U32(q.Busy, FALSE);
    ASSERT_TRUE(list_is_self(&q.DeviceListHead));
    return true;
}

static bool t_busy_flag_position(void)
{
    KDEVICE_QUEUE q;
    KDEVICE_QUEUE_ENTRY e;
    UCHAR busy_held, busy_drained;

    KeInitializeDeviceQueue(&q);
    memset(&e, 0, sizeof(e));

    AT_DISPATCH({
        KeInsertDeviceQueue(&q, &e);
        busy_held = ((const UCHAR *)&q)[3];
        KeRemoveDeviceQueue(&q);
        busy_drained = ((const UCHAR *)&q)[3];
    });

    /* Busy is the fourth byte of the header, not a separate word behind
     * the list head. */
    ASSERT_EQ_U32(busy_held, 1);
    ASSERT_EQ_U32(busy_drained, 0);
    return true;
}

static bool t_insert_by_key_orders(void)
{
    KDEVICE_QUEUE q;
    KDEVICE_QUEUE_ENTRY own, e10, e20, e30;
    PKDEVICE_QUEUE_ENTRY out[4];

    KeInitializeDeviceQueue(&q);
    memset(&own, 0, sizeof(own));
    memset(&e10, 0, sizeof(e10));
    memset(&e20, 0, sizeof(e20));
    memset(&e30, 0, sizeof(e30));

    AT_DISPATCH({
        /* Take ownership so the rest really queue. */
        KeInsertByKeyDeviceQueue(&q, &own, 0);
        KeInsertByKeyDeviceQueue(&q, &e30, 30);
        KeInsertByKeyDeviceQueue(&q, &e10, 10);
        KeInsertByKeyDeviceQueue(&q, &e20, 20);
        out[0] = KeRemoveDeviceQueue(&q);
        out[1] = KeRemoveDeviceQueue(&q);
        out[2] = KeRemoveDeviceQueue(&q);
        out[3] = KeRemoveDeviceQueue(&q);
    });

    ASSERT_EQ_U32(e30.SortKey, 30);
    ASSERT_EQ_PTR(out[0], &e10);
    ASSERT_EQ_PTR(out[1], &e20);
    ASSERT_EQ_PTR(out[2], &e30);
    ASSERT_EQ_PTR(out[3], NULL);
    return true;
}

static bool t_remove_by_key(void)
{
    KDEVICE_QUEUE q;
    KDEVICE_QUEUE_ENTRY own, e10, e20, e30;
    PKDEVICE_QUEUE_ENTRY exact, above, rest;

    KeInitializeDeviceQueue(&q);
    memset(&own, 0, sizeof(own));
    memset(&e10, 0, sizeof(e10));
    memset(&e20, 0, sizeof(e20));
    memset(&e30, 0, sizeof(e30));

    AT_DISPATCH({
        KeInsertByKeyDeviceQueue(&q, &own, 0);
        KeInsertByKeyDeviceQueue(&q, &e10, 10);
        KeInsertByKeyDeviceQueue(&q, &e20, 20);
        KeInsertByKeyDeviceQueue(&q, &e30, 30);
        /* The lowest entry at or above the requested key. */
        exact = KeRemoveByKeyDeviceQueue(&q, 20);
        /* A key past the highest queued one wraps to the head. */
        above = KeRemoveByKeyDeviceQueue(&q, 99);
        rest = KeRemoveByKeyDeviceQueue(&q, 0);
    });

    ASSERT_EQ_PTR(exact, &e20);
    ASSERT_EQ_PTR(above, &e10);
    ASSERT_EQ_PTR(rest, &e30);
    return true;
}

static bool t_remove_entry(void)
{
    KDEVICE_QUEUE q;
    KDEVICE_QUEUE_ENTRY own, e;
    BOOLEAN first, second;

    KeInitializeDeviceQueue(&q);
    memset(&own, 0, sizeof(own));
    memset(&e, 0, sizeof(e));

    AT_DISPATCH({
        KeInsertDeviceQueue(&q, &own);
        KeInsertDeviceQueue(&q, &e);
        first = KeRemoveEntryDeviceQueue(&q, (PKDEVICE_QUEUE)&e);
        second = KeRemoveEntryDeviceQueue(&q, (PKDEVICE_QUEUE)&e);
    });

    ASSERT_EQ_U32(first, TRUE);
    ASSERT_EQ_U32(e.Inserted, FALSE);
    /* A second removal of the same entry is a no-op, not a corruption. */
    ASSERT_EQ_U32(second, FALSE);
    ASSERT_TRUE(list_is_self(&q.DeviceListHead));
    return true;
}

/* --- the queue the kernel embeds in a device object ---------------- */

static DRIVER_OBJECT g_driver;
static PDEVICE_OBJECT g_device;
static NTSTATUS g_create_status = STATUS_UNSUCCESSFUL;
static bool g_setup_done;

static NTSTATUS NTAPI null_dispatch(PDEVICE_OBJECT dev, PIRP irp)
{
    (void)dev;
    (void)irp;
    return STATUS_SUCCESS;
}

static bool setup_device(void)
{
    static const char DEVICE_NAME[] = "\\Device\\nxkrnlDevQueue";
    OBJECT_STRING name = {
        .Length        = (USHORT)(sizeof(DEVICE_NAME) - 1),
        .MaximumLength = sizeof(DEVICE_NAME),
        .Buffer        = (PCHAR)DEVICE_NAME,
    };
    int i;

    if (g_setup_done)
        return NT_SUCCESS(g_create_status) && g_device != NULL;
    g_setup_done = true;

    for (i = 0; i < MJ_SLOTS; i++)
        g_driver.MajorFunction[i] = null_dispatch;

    g_create_status = IoCreateDevice(&g_driver, 0, &name,
                                     FILE_DEVICE_UNKNOWN, FALSE, &g_device);
    return NT_SUCCESS(g_create_status) && g_device != NULL;
}

static bool t_device_object_queue(void)
{
    if (!setup_device())
        FAIL_AND_RETURN("IoCreateDevice=0x%08x dev=%p",
                        (unsigned)g_create_status, (void *)g_device);

    /* IoCreateDevice initializes the embedded queue, so reading it back
     * through the title's own structure pins its offset as well as its
     * contents. */
    ASSERT_EQ_U32(g_device->DeviceQueue.Type, DEVICE_QUEUE_OBJECT);
    ASSERT_EQ_U32(g_device->DeviceQueue.Size, sizeof(KDEVICE_QUEUE));
    ASSERT_EQ_U32(g_device->DeviceQueue.Busy, 0);
    ASSERT_TRUE(list_is_self(&g_device->DeviceQueue.DeviceListHead));
    return true;
}

/* A disk-class device is the one IoCreateDevice initializes DeviceLock
 * for, and DeviceLock is the field immediately behind the embedded
 * queue -- reading it back at the console's offset is what proves the
 * queue is 12 bytes inside the kernel too, not just in this header. */
static DRIVER_OBJECT g_disk_driver;
static PDEVICE_OBJECT g_disk_device;
static NTSTATUS g_disk_status = STATUS_UNSUCCESSFUL;
static bool g_disk_setup_done;

#define FILE_DEVICE_DISK 0x00000007u

static bool setup_disk_device(void)
{
    static const char DISK_NAME[] = "\\Device\\nxkrnlDevQueueDisk";
    OBJECT_STRING name = {
        .Length        = (USHORT)(sizeof(DISK_NAME) - 1),
        .MaximumLength = sizeof(DISK_NAME),
        .Buffer        = (PCHAR)DISK_NAME,
    };
    int i;

    if (g_disk_setup_done)
        return NT_SUCCESS(g_disk_status) && g_disk_device != NULL;
    g_disk_setup_done = true;

    for (i = 0; i < MJ_SLOTS; i++)
        g_disk_driver.MajorFunction[i] = null_dispatch;

    g_disk_status = IoCreateDevice(&g_disk_driver, 0, &name,
                                   FILE_DEVICE_DISK, FALSE, &g_disk_device);
    return NT_SUCCESS(g_disk_status) && g_disk_device != NULL;
}

static bool t_device_lock_follows_queue(void)
{
    if (!setup_disk_device())
        FAIL_AND_RETURN("IoCreateDevice(disk)=0x%08x dev=%p",
                        (unsigned)g_disk_status, (void *)g_disk_device);

    /* Initialized as a signalled synchronization event. */
    ASSERT_EQ_U32(g_disk_device->DeviceLock.Header.Type, 1);
    ASSERT_EQ_U32(g_disk_device->DeviceLock.Header.Size, sizeof(KEVENT) / 4);
    ASSERT_EQ_U32(g_disk_device->DeviceLock.Header.SignalState, 1);
    ASSERT_TRUE(list_is_self(&g_disk_device->DeviceLock.Header.WaitListHead));
    return true;
}

static const test_entry_t ke_devqueue_entries[] = {
    {"initialize",                  t_initialize},
    {"insert_remove",               t_insert_remove},
    {"busy_flag_position",          t_busy_flag_position},
    {"insert_by_key_orders",        t_insert_by_key_orders},
    {"remove_by_key",               t_remove_by_key},
    {"remove_entry",                t_remove_entry},
    {"device_object_queue",         t_device_object_queue},
    {"device_lock_follows_queue",    t_device_lock_follows_queue},
};

DEFINE_GROUP(ke_devqueue, "ke/devqueue");
