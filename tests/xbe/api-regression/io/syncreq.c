/*
 * The console's build-dispatch-wait helpers: IoSynchronousFsdRequest and
 * IoSynchronousDeviceIoControlRequest.
 *
 * Both are the whole round trip in one call -- they own the event and
 * the status block, so a caller that only wants the result never
 * handles the packet.  The driver here is the title's own, so what the
 * helper built is observed from inside the dispatch routine, and the
 * status it reports can be checked against what the driver returned.
 */

#include "../harness.h"
#include <string.h>

#define MJ_SLOTS 0x0E

#define XBOX_IRP_MJ_READ           2
#define XBOX_IRP_MJ_DEVICE_CONTROL 10

#define FILE_DEVICE_UNKNOWN 0x00000022u

/* METHOD_NEITHER, so the buffers reach the driver as the caller's. */
#define TEST_IOCTL   0x0022200Bu
#define READ_INFO    0x00004444u
#define IOCTL_INFO   0x00000030u   /* also the length the helper reports */
#define FAIL_STATUS  ((NTSTATUS)0xC000000DL)  /* STATUS_INVALID_PARAMETER */

#define XFER_LEN    0x100
#define XFER_OFFSET 0x8000

typedef struct {
    ULONG calls;
    UCHAR major;
    PVOID user_buffer;
    ULONG raw[6];
} obs_t;

static obs_t g_read_obs, g_ioctl_obs;

static DRIVER_OBJECT g_driver;
static PDEVICE_OBJECT g_device;
static UCHAR g_buf[XFER_LEN];
static UCHAR g_in_buf[16];
static UCHAR g_out_buf[64];

static bool g_setup_done, g_setup_ok;
static NTSTATUS g_create_dev_status = STATUS_UNSUCCESSFUL;
static NTSTATUS g_fsd_status = STATUS_UNSUCCESSFUL;
static NTSTATUS g_ioctl_status = STATUS_UNSUCCESSFUL;
static NTSTATUS g_fsd_fail_status = STATUS_UNSUCCESSFUL;
static ULONG g_returned_len = 0xFFFFFFFFu;

/* Set while the driver should fail the request instead of succeeding. */
static volatile bool g_fail_next;

static const char DEVICE_NAME[] = "\\Device\\nxkrnlsyncreq";

static NTSTATUS NTAPI test_dispatch(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
    const ULONG *sp = (const ULONG *)Irp->Tail.Overlay.CurrentStackLocation;
    ULONG_PTR delta = (ULONG_PTR)sp - (ULONG_PTR)Irp;
    UCHAR major = 0xFF;
    obs_t *o = NULL;

    (void)DeviceObject;

    if (sp != NULL && delta >= 0x20 && delta <= 0x200) {
        major = (UCHAR)(sp[0] & 0xFF);
        if (major == XBOX_IRP_MJ_READ)
            o = &g_read_obs;
        else if (major == XBOX_IRP_MJ_DEVICE_CONTROL)
            o = &g_ioctl_obs;

        if (o != NULL) {
            o->calls++;
            o->major = major;
            o->user_buffer = Irp->UserBuffer;
            memcpy(o->raw, sp, sizeof(o->raw));
        }
    }

    if (g_fail_next) {
        Irp->IoStatus.Status = FAIL_STATUS;
        Irp->IoStatus.Information = 0;
    } else {
        Irp->IoStatus.Status = STATUS_SUCCESS;
        Irp->IoStatus.Information =
            (major == XBOX_IRP_MJ_DEVICE_CONTROL) ? IOCTL_INFO : READ_INFO;
    }

    IofCompleteRequest(Irp, 0);
    return Irp->IoStatus.Status;
}

static bool setup(void)
{
    OBJECT_STRING name = {
        .Length        = (USHORT)(sizeof(DEVICE_NAME) - 1),
        .MaximumLength = sizeof(DEVICE_NAME),
        .Buffer        = (PCHAR)DEVICE_NAME,
    };
    LARGE_INTEGER offset;
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
    memset(g_buf, 0x7E, sizeof(g_buf));
    memset(g_in_buf, 0x5C, sizeof(g_in_buf));
    memset(g_out_buf, 0, sizeof(g_out_buf));

    g_fsd_status = IoSynchronousFsdRequest(XBOX_IRP_MJ_READ, g_device,
                                           g_buf, XFER_LEN, &offset);

    g_ioctl_status = IoSynchronousDeviceIoControlRequest(
                         TEST_IOCTL, g_device, g_in_buf, sizeof(g_in_buf),
                         g_out_buf, sizeof(g_out_buf), &g_returned_len,
                         FALSE);

    /* A failing driver must surface through the helper's return. */
    g_fail_next = true;
    g_fsd_fail_status = IoSynchronousFsdRequest(XBOX_IRP_MJ_READ, g_device,
                                                g_buf, XFER_LEN, &offset);
    g_fail_next = false;

    g_setup_ok = true;
    return true;
}

#define FAIL_SETUP() \
    FAIL_AND_RETURN("setup failed: IoCreateDevice=0x%08x dev=%p", \
                    (unsigned)g_create_dev_status, (void *)g_device)

/* The helper dispatches once and reports the driver's status. */
static bool t_fsd_request_round_trip(void)
{
    if (!setup())
        FAIL_SETUP();
    if (g_read_obs.calls == 0)
        FAIL_AND_RETURN("read was never dispatched");
    ASSERT_EQ_U32(g_read_obs.major, XBOX_IRP_MJ_READ);
    ASSERT_NTSTATUS(g_fsd_status, STATUS_SUCCESS);
    return true;
}

/* It builds the same stack location the explicit builder would. */
static bool t_fsd_request_parameters(void)
{
    if (!setup())
        FAIL_SETUP();
    if (g_read_obs.calls == 0)
        FAIL_AND_RETURN("read was never dispatched");
    ASSERT_EQ_U32(g_read_obs.raw[1], XFER_LEN);      /* Length      */
    ASSERT_EQ_U32(g_read_obs.raw[3], XFER_OFFSET);   /* ByteOffset  */
    ASSERT_EQ_U32(g_read_obs.raw[4], 0);
    ASSERT_EQ_PTR(g_read_obs.user_buffer, g_buf);
    return true;
}

static bool t_ioctl_request_round_trip(void)
{
    if (!setup())
        FAIL_SETUP();
    if (g_ioctl_obs.calls == 0)
        FAIL_AND_RETURN("ioctl was never dispatched");
    ASSERT_EQ_U32(g_ioctl_obs.major, XBOX_IRP_MJ_DEVICE_CONTROL);
    ASSERT_NTSTATUS(g_ioctl_status, STATUS_SUCCESS);
    return true;
}

static bool t_ioctl_request_parameters(void)
{
    if (!setup())
        FAIL_SETUP();
    if (g_ioctl_obs.calls == 0)
        FAIL_AND_RETURN("ioctl was never dispatched");
    ASSERT_EQ_U32(g_ioctl_obs.raw[1], sizeof(g_out_buf));
    if ((PVOID)g_ioctl_obs.raw[2] != (PVOID)g_in_buf)
        FAIL_AND_RETURN("InputBuffer at +0x08: got 0x%08x expected %p",
                        (unsigned)g_ioctl_obs.raw[2], (void *)g_in_buf);
    ASSERT_EQ_U32(g_ioctl_obs.raw[3], sizeof(g_in_buf));
    ASSERT_EQ_U32(g_ioctl_obs.raw[4], TEST_IOCTL);
    ASSERT_EQ_PTR(g_ioctl_obs.user_buffer, g_out_buf);
    return true;
}

/* The transferred length comes back through its own out-parameter,
 * separately from the status. */
static bool t_ioctl_returns_length(void)
{
    if (!setup())
        FAIL_SETUP();
    ASSERT_EQ_U32(g_returned_len, IOCTL_INFO);
    return true;
}

/* A driver failure reaches the caller as the helper's return value. */
static bool t_driver_failure_surfaces(void)
{
    if (!setup())
        FAIL_SETUP();
    ASSERT_NTSTATUS(g_fsd_fail_status, FAIL_STATUS);
    return true;
}

static const test_entry_t io_syncreq_entries[] = {
    {"fsd_request_round_trip", t_fsd_request_round_trip},
    {"fsd_request_parameters", t_fsd_request_parameters},
    {"ioctl_request_round_trip", t_ioctl_request_round_trip},
    {"ioctl_request_parameters", t_ioctl_request_parameters},
    {"ioctl_returns_length", t_ioctl_returns_length},
    {"driver_failure_surfaces", t_driver_failure_surfaces},
};

DEFINE_GROUP(io_syncreq, "io/syncreq");
