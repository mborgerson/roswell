/*
 * IoBuildAsynchronousFsdRequest: the fire-and-forget half of the
 * request builders io/fsdreq covers.
 *
 * The synchronous builder arms an event and queues the packet to the
 * calling thread; this one does neither, so the caller owns the packet
 * outright and disposes of it itself.  The cases below therefore never
 * let the packet reach the completion path: the driver records what it
 * was handed and returns without completing, and the title frees the
 * packet.  That is also what makes the test safe -- an async packet has
 * no thread to deliver a completion APC to.
 */

#include "../harness.h"
#include <string.h>

#define MJ_SLOTS 0x0E
#define FILE_DEVICE_UNKNOWN 0x00000022u

/* The console's compacted dispatch table. */
#define XBOX_IRP_MJ_READ  2
#define XBOX_IRP_MJ_WRITE 3

#define XFER_LEN    0x200
#define XFER_OFFSET 0x8000

static DRIVER_OBJECT g_driver;
static PDEVICE_OBJECT g_device;
static NTSTATUS g_create_status = STATUS_UNSUCCESSFUL;
static bool g_setup_done;
static UCHAR g_buf[XFER_LEN];

static ULONG g_seen_calls;
static UCHAR g_seen_major;
static PVOID g_seen_buffer;
static ULONG g_seen_length;

/* Records and returns WITHOUT completing: the packet stays the title's. */
static NTSTATUS NTAPI record_dispatch(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
    const ULONG *sp = (const ULONG *)Irp->Tail.Overlay.CurrentStackLocation;
    ULONG_PTR delta = (ULONG_PTR)sp - (ULONG_PTR)Irp;

    (void)DeviceObject;

    if (sp != NULL && delta >= 0x20 && delta <= 0x200) {
        g_seen_calls++;
        g_seen_major = (UCHAR)(sp[0] & 0xFF);
        g_seen_length = sp[1];
        g_seen_buffer = Irp->UserBuffer;
    }
    return STATUS_SUCCESS;
}

static bool setup(void)
{
    static const char DEVICE_NAME[] = "\\Device\\nxkrnlasyncreq";
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
        g_driver.MajorFunction[i] = record_dispatch;

    memset(g_buf, 0x7E, sizeof(g_buf));
    g_create_status = IoCreateDevice(&g_driver, 0, &name,
                                     FILE_DEVICE_UNKNOWN, FALSE, &g_device);
    return NT_SUCCESS(g_create_status) && g_device != NULL;
}

#define FAIL_SETUP() \
    FAIL_AND_RETURN("IoCreateDevice=0x%08x dev=%p", \
                    (unsigned)g_create_status, (void *)g_device)

static PIRP build(ULONG major, PIO_STATUS_BLOCK iosb)
{
    LARGE_INTEGER offset;

    offset.QuadPart = XFER_OFFSET;
    return IoBuildAsynchronousFsdRequest(major, g_device, g_buf, XFER_LEN,
                                         &offset, iosb);
}

static bool t_build_read(void)
{
    IO_STATUS_BLOCK iosb;
    PIRP irp;

    if (!setup())
        FAIL_SETUP();

    memset(&iosb, 0xEE, sizeof(iosb));
    irp = build(XBOX_IRP_MJ_READ, &iosb);
    ASSERT_NOT_NULL(irp);

    /* The device's stack depth, and no event: nothing is waiting. */
    ASSERT_EQ_U32(irp->StackCount, g_device->StackSize);
    ASSERT_EQ_PTR(irp->UserEvent, NULL);
    ASSERT_EQ_PTR(irp->UserIosb, &iosb);
    /* The console tags neither direction nor caching here: with a
     * device that asks for neither buffered nor direct IO the buffer is
     * simply handed through and Flags is left clear. */
    ASSERT_EQ_U32(irp->Flags, 0);

    IoFreeIrp(irp);
    return true;
}

static bool t_build_write(void)
{
    IO_STATUS_BLOCK iosb;
    PIRP irp;

    if (!setup())
        FAIL_SETUP();

    g_seen_calls = 0;
    g_seen_major = 0xFF;

    memset(&iosb, 0xEE, sizeof(iosb));
    irp = build(XBOX_IRP_MJ_WRITE, &iosb);
    ASSERT_NOT_NULL(irp);
    ASSERT_EQ_U32(irp->Flags, 0);
    ASSERT_EQ_PTR(irp->UserBuffer, g_buf);

    /* A write is filed as major 3 on the console, where NT uses 4. */
    ASSERT_NTSTATUS(IofCallDriver(g_device, irp), STATUS_SUCCESS);
    ASSERT_EQ_U32(g_seen_calls, 1);
    ASSERT_EQ_U32(g_seen_major, XBOX_IRP_MJ_WRITE);
    ASSERT_EQ_U32(g_seen_length, XFER_LEN);

    IoFreeIrp(irp);
    return true;
}

static bool t_no_iosb_is_accepted(void)
{
    PIRP irp;

    if (!setup())
        FAIL_SETUP();

    /* The status block is optional: a caller that does not want one
     * still gets a packet. */
    irp = build(XBOX_IRP_MJ_READ, NULL);
    ASSERT_NOT_NULL(irp);
    ASSERT_EQ_PTR(irp->UserIosb, NULL);

    IoFreeIrp(irp);
    return true;
}

static bool t_dispatch_reaches_the_driver(void)
{
    IO_STATUS_BLOCK iosb;
    PIRP irp;
    NTSTATUS s;

    if (!setup())
        FAIL_SETUP();

    g_seen_calls = 0;
    g_seen_major = 0xFF;
    g_seen_buffer = NULL;
    g_seen_length = 0;

    memset(&iosb, 0xEE, sizeof(iosb));
    irp = build(XBOX_IRP_MJ_READ, &iosb);
    ASSERT_NOT_NULL(irp);

    s = IofCallDriver(g_device, irp);
    ASSERT_NTSTATUS(s, STATUS_SUCCESS);

    ASSERT_EQ_U32(g_seen_calls, 1);
    ASSERT_EQ_U32(g_seen_major, XBOX_IRP_MJ_READ);
    ASSERT_EQ_U32(g_seen_length, XFER_LEN);
    ASSERT_EQ_PTR(g_seen_buffer, g_buf);

    IoFreeIrp(irp);
    return true;
}

static const test_entry_t io_asyncreq_entries[] = {
    {"build_read",                 t_build_read},
    {"build_write",                t_build_write},
    {"no_iosb_is_accepted",        t_no_iosb_is_accepted},
    {"dispatch_reaches_the_driver", t_dispatch_reaches_the_driver},
};

DEFINE_GROUP(io_asyncreq, "io/asyncreq");
