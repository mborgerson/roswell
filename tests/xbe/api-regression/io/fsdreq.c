/*
 * Title-built synchronous requests: IoBuildSynchronousFsdRequest paired
 * with IofCallDriver.
 *
 * This is the shape Halo 2 uses to reach a device it did not open a
 * handle to -- build a read/write packet against the device object,
 * dispatch it, wait for the completion event.  The driver here is the
 * title's own, so the request is observed from inside the dispatch
 * routine: the major function it was filed under, the parameters the
 * builder wrote into the stack location, and the buffer it carried.
 *
 * The major function doubles as a check on the console's compacted
 * numbering -- a read is filed as 2, a write as 3, where NT uses 3 and
 * 4.  No file handle is involved, so the device's flags are left
 * exactly as the kernel stamped them.
 */

#include "../harness.h"
#include <string.h>

#define MJ_SLOTS 0x0E

/* The console's compacted dispatch table. */
#define XBOX_IRP_MJ_READ  2
#define XBOX_IRP_MJ_WRITE 3

#define FILE_DEVICE_UNKNOWN 0x00000022u

#define READ_INFO  0x00001111u
#define WRITE_INFO 0x00002222u

#define XFER_LEN    0x200
#define XFER_OFFSET 0x4000

typedef struct {
    ULONG calls;
    UCHAR major;
    PVOID irp;
    PVOID user_buffer;
    ULONG length;         /* stack Parameters.Read.Length      (+0x04) */
    ULONG off_low;        /* stack Parameters.Read.ByteOffset  (+0x0c) */
    ULONG off_high;       /*                                   (+0x10) */
    PVOID stack_device;   /* stack DeviceObject                (+0x14) */
} obs_t;

static obs_t g_read_obs, g_write_obs;

static DRIVER_OBJECT g_driver;
static PDEVICE_OBJECT g_device;
static UCHAR g_buf[XFER_LEN];

static bool g_setup_done, g_setup_ok;
static NTSTATUS g_create_dev_status = STATUS_UNSUCCESSFUL;
static PIRP g_read_irp, g_write_irp;
static NTSTATUS g_read_call_status = STATUS_UNSUCCESSFUL;
static NTSTATUS g_write_call_status = STATUS_UNSUCCESSFUL;
static NTSTATUS g_read_wait_status = STATUS_UNSUCCESSFUL;
static IO_STATUS_BLOCK g_read_iosb, g_write_iosb;

static const char DEVICE_NAME[] = "\\Device\\nxkrnlfsdreq";

static NTSTATUS NTAPI test_dispatch(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
    const ULONG *sp = (const ULONG *)Irp->Tail.Overlay.CurrentStackLocation;
    ULONG_PTR delta = (ULONG_PTR)sp - (ULONG_PTR)Irp;
    UCHAR major = 0xFF;
    obs_t *o = NULL;

    (void)DeviceObject;

    /* Only trust a stack pointer that lands inside the packet. */
    if (sp != NULL && delta >= 0x20 && delta <= 0x200) {
        major = (UCHAR)(sp[0] & 0xFF);
        if (major == XBOX_IRP_MJ_READ)
            o = &g_read_obs;
        else if (major == XBOX_IRP_MJ_WRITE)
            o = &g_write_obs;

        if (o != NULL) {
            o->calls++;
            o->major = major;
            o->irp = Irp;
            o->user_buffer = Irp->UserBuffer;
            o->length = sp[1];
            o->off_low = sp[3];
            o->off_high = sp[4];
            o->stack_device = (PVOID)sp[5];
        }
    }

    Irp->IoStatus.Status = STATUS_SUCCESS;
    Irp->IoStatus.Information =
        (major == XBOX_IRP_MJ_WRITE) ? WRITE_INFO : READ_INFO;

    IofCompleteRequest(Irp, 0);
    return STATUS_SUCCESS;
}

static bool setup(void)
{
    OBJECT_STRING name = {
        .Length        = (USHORT)(sizeof(DEVICE_NAME) - 1),
        .MaximumLength = sizeof(DEVICE_NAME),
        .Buffer        = (PCHAR)DEVICE_NAME,
    };
    LARGE_INTEGER offset;
    LARGE_INTEGER timeout;
    KEVENT event;
    int i;

    if (g_setup_done)
        return g_setup_ok;
    g_setup_done = true;

    for (i = 0; i < MJ_SLOTS; i++)
        g_driver.MajorFunction[i] = test_dispatch;

    g_create_dev_status = IoCreateDevice(&g_driver, 0, &name,
                                         FILE_DEVICE_UNKNOWN, FALSE,
                                         &g_device);
    if (!NT_SUCCESS(g_create_dev_status) || g_device == NULL)
        return false;

    offset.QuadPart = XFER_OFFSET;
    timeout.QuadPart = -((LONGLONG)5 * 1000 * 10000);
    memset(g_buf, 0x3C, sizeof(g_buf));

    /* Read: the builder arms the event, the dispatch completes inline. */
    KeInitializeEvent(&event, NotificationEvent, FALSE);
    memset(&g_read_iosb, 0xEE, sizeof(g_read_iosb));
    g_read_irp = IoBuildSynchronousFsdRequest(XBOX_IRP_MJ_READ, g_device,
                                              g_buf, XFER_LEN, &offset,
                                              &event, &g_read_iosb);
    if (g_read_irp == NULL)
        return false;
    g_read_call_status = IofCallDriver(g_device, g_read_irp);
    g_read_wait_status = KeWaitForSingleObject(&event, Executive, KernelMode,
                                               FALSE, &timeout);

    /* Write: same path, so the major function is the only difference. */
    KeInitializeEvent(&event, NotificationEvent, FALSE);
    memset(&g_write_iosb, 0xEE, sizeof(g_write_iosb));
    g_write_irp = IoBuildSynchronousFsdRequest(XBOX_IRP_MJ_WRITE, g_device,
                                               g_buf, XFER_LEN, &offset,
                                               &event, &g_write_iosb);
    if (g_write_irp != NULL) {
        g_write_call_status = IofCallDriver(g_device, g_write_irp);
        KeWaitForSingleObject(&event, Executive, KernelMode, FALSE, &timeout);
    }

    g_setup_ok = true;
    return true;
}

#define FAIL_SETUP() \
    FAIL_AND_RETURN("setup failed: IoCreateDevice=0x%08x dev=%p irp=%p", \
                    (unsigned)g_create_dev_status, (void *)g_device, \
                    (void *)g_read_irp)

static bool t_build_returns_irp(void)
{
    if (!setup())
        FAIL_SETUP();
    ASSERT_NOT_NULL(g_read_irp);
    ASSERT_NOT_NULL(g_write_irp);
    return true;
}

/* A read is filed as major 2 on the console, not NT's 3. */
static bool t_read_major_is_two(void)
{
    if (!setup())
        FAIL_SETUP();
    if (g_read_obs.calls != 1)
        FAIL_AND_RETURN("read dispatched %u times, expected 1",
                        (unsigned)g_read_obs.calls);
    ASSERT_EQ_U32(g_read_obs.major, XBOX_IRP_MJ_READ);
    return true;
}

/* ...and a write as 3, not NT's 4. */
static bool t_write_major_is_three(void)
{
    if (!setup())
        FAIL_SETUP();
    if (g_write_obs.calls != 1)
        FAIL_AND_RETURN("write dispatched %u times, expected 1",
                        (unsigned)g_write_obs.calls);
    ASSERT_EQ_U32(g_write_obs.major, XBOX_IRP_MJ_WRITE);
    return true;
}

/* The builder fills the stack location the driver will read. */
static bool t_stack_parameters(void)
{
    if (!setup())
        FAIL_SETUP();
    if (g_read_obs.calls == 0)
        FAIL_AND_RETURN("read was never dispatched");
    ASSERT_EQ_U32(g_read_obs.length, XFER_LEN);
    ASSERT_EQ_U32(g_read_obs.off_low, XFER_OFFSET);
    ASSERT_EQ_U32(g_read_obs.off_high, 0);
    ASSERT_EQ_PTR(g_read_obs.stack_device, g_device);
    return true;
}

/* With neither buffering flag set the caller's buffer is carried
 * straight through -- the console's IRP has nowhere else to put it. */
static bool t_buffer_passed_through(void)
{
    if (!setup())
        FAIL_SETUP();
    if (g_read_obs.calls == 0)
        FAIL_AND_RETURN("read was never dispatched");
    ASSERT_EQ_PTR(g_read_obs.user_buffer, g_buf);
    return true;
}

/* IofCallDriver hands back what the dispatch routine returned. */
static bool t_call_driver_returns_status(void)
{
    if (!setup())
        FAIL_SETUP();
    ASSERT_NTSTATUS(g_read_call_status, STATUS_SUCCESS);
    ASSERT_NTSTATUS(g_write_call_status, STATUS_SUCCESS);
    return true;
}

/* Completion signals the event the builder was given and fills the
 * caller's status block. */
static bool t_completion_signals_and_reports(void)
{
    if (!setup())
        FAIL_SETUP();
    ASSERT_NTSTATUS(g_read_wait_status, STATUS_SUCCESS);
    ASSERT_NTSTATUS(g_read_iosb.Status, STATUS_SUCCESS);
    ASSERT_EQ_U32(g_read_iosb.Information, READ_INFO);
    ASSERT_EQ_U32(g_write_iosb.Information, WRITE_INFO);
    return true;
}

static const test_entry_t io_fsdreq_entries[] = {
    {"build_returns_irp", t_build_returns_irp},
    {"read_major_is_two", t_read_major_is_two},
    {"write_major_is_three", t_write_major_is_three},
    {"stack_parameters", t_stack_parameters},
    {"buffer_passed_through", t_buffer_passed_through},
    {"call_driver_returns_status", t_call_driver_returns_status},
    {"completion_signals_and_reports", t_completion_signals_and_reports},
};

DEFINE_GROUP(io_fsdreq, "io/fsdreq");
