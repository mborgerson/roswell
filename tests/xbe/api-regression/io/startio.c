/*
 * IoStartPacket / IoStartNextPacket / IoStartNextPacketByKey: the
 * serialised request queue a title's own driver runs on.
 *
 * The first packet goes straight to the driver's StartIo routine and
 * marks the device busy; everything after it queues, sorted by key,
 * until the driver asks for the next one.  By-key is the interesting
 * half: it does not take the head of the queue but the first packet
 * whose key is at or past the one asked for, which is how a disk driver
 * sweeps in one direction.
 */

#include "../harness.h"
#include <string.h>

#define FILE_DEVICE_UNKNOWN 0x00000022u
#define MJ_SLOTS 0x0E
#define DO_READY_SET   0x00000004u
#define DO_READY_CLEAR 0x00000010u

#define MAX_PACKETS 8

static const char DEVICE_NAME[] = "\\Device\\nxkrnl-api-startio";

static DRIVER_OBJECT g_driver;
static PIRP g_packets[MAX_PACKETS];
static volatile LONG g_order[MAX_PACKETS];
static volatile LONG g_started;

/* StartIo runs at DISPATCH_LEVEL, so it only records. */
static void NTAPI start_io(PDEVICE_OBJECT dev, PIRP irp)
{
    LONG n = g_started;
    int i;

    (void)dev;
    for (i = 0; i < MAX_PACKETS; i++) {
        if (g_packets[i] == irp) {
            if (n < MAX_PACKETS) g_order[n] = i;
            break;
        }
    }
    g_started = n + 1;
}

static NTSTATUS NTAPI pass_dispatch(PDEVICE_OBJECT dev, PIRP irp)
{
    (void)dev;
    irp->IoStatus.Status = STATUS_SUCCESS;
    irp->IoStatus.Information = 0;
    IofCompleteRequest(irp, IO_NO_INCREMENT);
    return STATUS_SUCCESS;
}

static NTSTATUS make_device(PDEVICE_OBJECT *out)
{
    OBJECT_STRING name = {
        .Length        = (USHORT)(sizeof(DEVICE_NAME) - 1),
        .MaximumLength = (USHORT)sizeof(DEVICE_NAME),
        .Buffer        = (PCHAR)DEVICE_NAME,
    };
    NTSTATUS s;
    int i;

    for (i = 0; i < MJ_SLOTS; i++)
        g_driver.MajorFunction[i] = pass_dispatch;
    g_driver.DriverStartIo = start_io;

    *out = NULL;
    s = IoCreateDevice(&g_driver, 0, &name, FILE_DEVICE_UNKNOWN, FALSE, out);
    if (NT_SUCCESS(s) && *out != NULL) {
        (*out)->Flags |= DO_READY_SET;
        (*out)->Flags &= ~DO_READY_CLEAR;
    }
    return s;
}

static void reset(void)
{
    int i;

    g_started = 0;
    for (i = 0; i < MAX_PACKETS; i++) {
        g_packets[i] = NULL;
        g_order[i] = -1;
    }
}

/* A packet the queue can carry: nothing dispatches it, so one stack
 * location and no completion routine is enough. */
static PIRP make_packet(PDEVICE_OBJECT dev, int slot)
{
    PIRP irp = IoAllocateIrp(dev->StackSize);

    if (irp != NULL)
        g_packets[slot] = irp;
    return irp;
}

static void free_packets(void)
{
    int i;

    for (i = 0; i < MAX_PACKETS; i++) {
        if (g_packets[i] != NULL) {
            IoFreeIrp(g_packets[i]);
            g_packets[i] = NULL;
        }
    }
}

/* The first packet runs at once; the second waits for it to finish. */
static bool t_the_first_packet_starts_the_device(void)
{
    PDEVICE_OBJECT dev;
    NTSTATUS s;

    reset();
    s = make_device(&dev);
    if (!NT_SUCCESS(s)) FAIL_AND_RETURN("create -> 0x%08x", (unsigned)s);

    if (make_packet(dev, 0) == NULL || make_packet(dev, 1) == NULL) {
        free_packets();
        IoDeleteDevice(dev);
        FAIL_AND_RETURN("out of packets");
    }

    IoStartPacket(dev, g_packets[0], NULL);
    if (g_started != 1) {
        free_packets();
        IoDeleteDevice(dev);
        FAIL_AND_RETURN("first packet: %ld started", (long)g_started);
    }

    IoStartPacket(dev, g_packets[1], NULL);
    if (g_started != 1) {
        free_packets();
        IoDeleteDevice(dev);
        FAIL_AND_RETURN("second packet ran early");
    }

    /* Releasing the device hands the queued one over. */
    IoStartNextPacket(dev);
    if (g_started != 2 || g_order[1] != 1) {
        free_packets();
        IoDeleteDevice(dev);
        FAIL_AND_RETURN("after next: %ld started, order %ld",
                        (long)g_started, (long)g_order[1]);
    }

    /* Nothing left to hand over, and the device goes idle. */
    IoStartNextPacket(dev);
    free_packets();
    IoDeleteDevice(dev);
    ASSERT_EQ_U32(g_started, 2);
    return true;
}

/* By-key takes the first packet at or past the key it is given, and
 * wraps to the head of the queue once nothing is left past it. */
static bool t_by_key_sweeps_forward(void)
{
    PDEVICE_OBJECT dev;
    ULONG keys[4] = { 10, 40, 20, 30 };
    NTSTATUS s;
    int i;

    reset();
    s = make_device(&dev);
    if (!NT_SUCCESS(s)) FAIL_AND_RETURN("create -> 0x%08x", (unsigned)s);

    for (i = 0; i < 5; i++) {
        if (make_packet(dev, i) == NULL) {
            free_packets();
            IoDeleteDevice(dev);
            FAIL_AND_RETURN("out of packets");
        }
    }

    /* Slot 4 is the packet that makes the device busy. */
    IoStartPacket(dev, g_packets[4], NULL);
    for (i = 0; i < 4; i++)
        IoStartPacket(dev, g_packets[i], &keys[i]);

    if (g_started != 1) {
        free_packets();
        IoDeleteDevice(dev);
        FAIL_AND_RETURN("%ld started before any release", (long)g_started);
    }

    /* Keys queued 10, 40, 20, 30.  Asking from 25 skips 10 and 20. */
    IoStartNextPacketByKey(dev, 25);
    IoStartNextPacketByKey(dev, 35);
    /* Nothing left past 35, so the sweep restarts at the lowest key. */
    IoStartNextPacketByKey(dev, 35);
    IoStartNextPacketByKey(dev, 35);
    IoStartNextPacket(dev);

    free_packets();
    IoDeleteDevice(dev);

    ASSERT_EQ_U32(g_started, 5);
    ASSERT_EQ_U32(g_order[0], 4);
    ASSERT_EQ_U32(g_order[1], 3);   /* key 30 */
    ASSERT_EQ_U32(g_order[2], 1);   /* key 40 */
    ASSERT_EQ_U32(g_order[3], 0);   /* key 10 */
    ASSERT_EQ_U32(g_order[4], 2);   /* key 20 */
    return true;
}

static const test_entry_t io_startio_entries[] = {
    { "the_first_packet_starts_the_device",
      t_the_first_packet_starts_the_device, NULL },
    { "by_key_sweeps_forward", t_by_key_sweeps_forward, NULL },
};

DEFINE_GROUP(io_startio, "io/startio");
