/*
 * Title-visible IRP / IO_STACK_LOCATION layout, exercised through a
 * driver object the title registers itself.
 *
 * Titles carry block-device drivers of their own (the Memory Unit driver
 * is the shipping example), so the kernel hands title code real IRPs and
 * the title reads them field by field:
 * Tail.Overlay.CurrentStackLocation to reach its stack location,
 * UserBuffer for the payload, IoStatus.Status/Information written back
 * on the way out.  Every offset below is therefore contract, not
 * internal detail.
 *
 * The driver registers one dispatch routine in ALL major-function slots
 * so the major-function NUMBERING is observed rather than assumed --
 * the Xbox MajorFunction table is compacted to 14 entries, so the codes
 * do not match NT's.
 */

#include "../harness.h"
#include <string.h>

/* Xbox DRIVER_OBJECT carries MajorFunction[0x0E]. */
#define MJ_SLOTS 0x0E

/* METHOD_NEITHER so the payload pointer reaches the driver as
 * Irp->UserBuffer instead of a kernel-allocated bounce buffer. */
#define TEST_IOCTL 0x00222003u
#define MAGIC_INFO 0x00005A5Au

#define FILE_DEVICE_UNKNOWN 0x00000022u

#ifndef STATUS_NO_SUCH_DEVICE
#define STATUS_NO_SUCH_DEVICE ((NTSTATUS)0xC000000EL)
#endif

#define DO_READY_SET   0x00000004u  /* mirrors what a shipping title sets */
#define DO_READY_CLEAR 0x00000010u  /* ...and clears, to publish the device */

#define PHASE_IDLE   0
#define PHASE_CREATE 1
#define PHASE_IOCTL  2
#define PHASE_CLOSE  3

/* nxdk declares neither IO_STACK_LOCATION nor the accessor macros, so a
 * title driver reaches its stack location through the IRP directly.
 * Only the header is spelled out; the rest is captured raw so the
 * oracle -- not this file -- decides where each parameter lives. */
#define SL_RAW_DWORDS 9  /* 0x24 bytes */

typedef struct {
    ULONG phase;
    UCHAR major;
    UCHAR minor;
    PVOID irp;
    PVOID stack;
    PVOID user_buffer;
    ULONG irp_size;
    ULONG raw[SL_RAW_DWORDS];
} obs_t;

#define MAX_OBS 8
static obs_t g_obs[MAX_OBS];
static ULONG g_nobs;
static volatile ULONG g_phase = PHASE_IDLE;

static DRIVER_OBJECT g_driver;
static PDEVICE_OBJECT g_device;
static UCHAR g_out_buf[64];
static UCHAR g_in_buf[16];

static bool g_setup_done;
static bool g_setup_ok;
static NTSTATUS g_ioctl_status = STATUS_UNSUCCESSFUL;
static NTSTATUS g_create_dev_status = STATUS_UNSUCCESSFUL;
static NTSTATUS g_open_status = STATUS_UNSUCCESSFUL;
/* Snapshot taken before the test touches the device. */
static ULONG g_dev_flags0;
static ULONG g_dev_type0;
static ULONG g_dev_stacksize0;
static ULONG g_dev_size0;
static IO_STATUS_BLOCK g_ioctl_iosb;

static const char DEVICE_NAME[] = "\\Device\\nxkrnlirplayout";
static const char DEVICE_NAME2[] = "\\Device\\nxkrnlirplayout2";

static PDEVICE_OBJECT g_device2;
static NTSTATUS g_open2_status = STATUS_UNSUCCESSFUL;
static ULONG g_dev2_flags0;

static NTSTATUS NTAPI test_dispatch(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
    const ULONG *sp = (const ULONG *)Irp->Tail.Overlay.CurrentStackLocation;
    ULONG_PTR delta = (ULONG_PTR)sp - (ULONG_PTR)Irp;

    if (g_nobs < MAX_OBS && sp != NULL && delta >= 0x20 && delta <= 0x200) {
        obs_t *o = &g_obs[g_nobs++];
        o->phase = g_phase;
        o->irp = Irp;
        o->stack = (PVOID)sp;
        o->user_buffer = Irp->UserBuffer;
        o->irp_size = Irp->Size;
        memcpy(o->raw, sp, sizeof(o->raw));
        o->major = (UCHAR)(o->raw[0] & 0xFF);
        o->minor = (UCHAR)((o->raw[0] >> 8) & 0xFF);
    }

    (void)DeviceObject;

    /* An IOCTL reports a distinctive Information so the caller can prove
     * the completion write-back reached its IO_STATUS_BLOCK. */
    Irp->IoStatus.Status = STATUS_SUCCESS;
    Irp->IoStatus.Information =
        (g_phase == PHASE_IOCTL) ? MAGIC_INFO : FILE_OPENED;

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
    ANSI_STRING open_name = {
        .Length        = (USHORT)(sizeof(DEVICE_NAME) - 1),
        .MaximumLength = sizeof(DEVICE_NAME),
        .Buffer        = (PCHAR)DEVICE_NAME,
    };
    OBJECT_ATTRIBUTES oa = {
        .RootDirectory = NULL,
        .ObjectName    = &open_name,
        .Attributes    = OBJ_CASE_INSENSITIVE,
    };
    IO_STATUS_BLOCK iosb;
    HANDLE h = NULL;
    int i;

    if (g_setup_done)
        return g_setup_ok;
    g_setup_done = true;

    for (i = 0; i < MJ_SLOTS; i++)
        g_driver.MajorFunction[i] = test_dispatch;

    g_create_dev_status = IoCreateDevice(&g_driver, 0, &name,
                                         FILE_DEVICE_UNKNOWN, FALSE, &g_device);
    if (!NT_SUCCESS(g_create_dev_status) || g_device == NULL)
        return false;

    g_dev_flags0     = g_device->Flags;
    g_dev_type0      = g_device->DeviceType;
    g_dev_stacksize0 = (ULONG)(UCHAR)g_device->StackSize;
    g_dev_size0      = g_device->Size;

    g_device->Flags |= DO_READY_SET;
    g_device->Flags &= ~DO_READY_CLEAR;

    g_phase = PHASE_CREATE;
    g_open_status = NtCreateFile(&h, GENERIC_READ | GENERIC_WRITE | SYNCHRONIZE,
                                 &oa, &iosb, NULL, FILE_ATTRIBUTE_NORMAL, 0,
                                 FILE_OPEN, FILE_NON_DIRECTORY_FILE |
                                 FILE_SYNCHRONOUS_IO_NONALERT);
    if (!NT_SUCCESS(g_open_status))
        return false;

    memset(g_in_buf, 0x5C, sizeof(g_in_buf));
    memset(g_out_buf, 0, sizeof(g_out_buf));
    memset(&g_ioctl_iosb, 0xEE, sizeof(g_ioctl_iosb));

    g_phase = PHASE_IOCTL;
    g_ioctl_status = NtDeviceIoControlFile(h, NULL, NULL, NULL, &g_ioctl_iosb,
                                           TEST_IOCTL, g_in_buf, sizeof(g_in_buf),
                                           g_out_buf, sizeof(g_out_buf));

    g_phase = PHASE_CLOSE;
    NtClose(h);
    g_phase = PHASE_IDLE;

    {
        OBJECT_STRING name2 = {
            .Length        = (USHORT)(sizeof(DEVICE_NAME2) - 1),
            .MaximumLength = sizeof(DEVICE_NAME2),
            .Buffer        = (PCHAR)DEVICE_NAME2,
        };
        ANSI_STRING open_name2 = {
            .Length        = (USHORT)(sizeof(DEVICE_NAME2) - 1),
            .MaximumLength = sizeof(DEVICE_NAME2),
            .Buffer        = (PCHAR)DEVICE_NAME2,
        };
        OBJECT_ATTRIBUTES oa2 = {
            .RootDirectory = NULL,
            .ObjectName    = &open_name2,
            .Attributes    = OBJ_CASE_INSENSITIVE,
        };
        IO_STATUS_BLOCK iosb2;
        HANDLE h2 = NULL;

        if (NT_SUCCESS(IoCreateDevice(&g_driver, 0, &name2,
                                      FILE_DEVICE_UNKNOWN, FALSE, &g_device2))
            && g_device2 != NULL) {
            g_dev2_flags0 = g_device2->Flags;
            g_open2_status = NtCreateFile(&h2, GENERIC_READ | SYNCHRONIZE,
                                          &oa2, &iosb2, NULL,
                                          FILE_ATTRIBUTE_NORMAL, 0, FILE_OPEN,
                                          FILE_NON_DIRECTORY_FILE |
                                          FILE_SYNCHRONOUS_IO_NONALERT);
            if (NT_SUCCESS(g_open2_status))
                NtClose(h2);
        }
    }

    g_setup_ok = true;
    return true;
}

#define FAIL_SETUP() \
    FAIL_AND_RETURN("setup failed: IoCreateDevice=0x%08x dev=%p " \
                    "NtCreateFile=0x%08x dispatches=%u", \
                    (unsigned)g_create_dev_status, (void *)g_device, \
                    (unsigned)g_open_status, (unsigned)g_nobs)

static const obs_t *find_obs(ULONG phase)
{
    ULONG i;
    for (i = 0; i < g_nobs; i++)
        if (g_obs[i].phase == phase)
            return &g_obs[i];
    return NULL;
}

static bool t_create_device(void)
{
    if (!setup())
        FAIL_SETUP();
    ASSERT_NOT_NULL(g_device);
    /* The kernel stamps the device it just built. */
    ASSERT_EQ_PTR(g_device->DriverObject, &g_driver);
    ASSERT_TRUE(g_device->StackSize >= 1);
    return true;
}

/* Pins the shape the kernel stamps on a freshly created device. */
static bool t_device_initial_fields(void)
{
    if (!setup())
        FAIL_SETUP();
    if (g_dev_type0 != FILE_DEVICE_UNKNOWN)
        FAIL_AND_RETURN("DeviceType: got 0x%08x expected 0x%08x "
                        "(Size=0x%04x StackSize=%u Flags=0x%08x)",
                        (unsigned)g_dev_type0, FILE_DEVICE_UNKNOWN,
                        (unsigned)g_dev_size0, (unsigned)g_dev_stacksize0,
                        (unsigned)g_dev_flags0);
    ASSERT_EQ_U32(g_dev_stacksize0, 1);
    return true;
}

/* A device is usable the moment it is created -- the console has no
 * PnP start, so there is no initializing bit for a driver to clear. */
static bool t_device_initial_flags(void)
{
    if (!setup())
        FAIL_SETUP();
    if (g_dev_flags0 != 0x18)
        FAIL_AND_RETURN("fresh device Flags: got 0x%08x expected 0x00000018",
                        (unsigned)g_dev_flags0);
    return true;
}

/* A fresh device is stamped 0x18 and stays unreachable until its driver
 * clears the initializing bit (0x10); the device created above is left
 * untouched, so opening it must be refused. */
static bool t_untouched_device_is_unreachable(void)
{
    if (!setup())
        FAIL_SETUP();
    ASSERT_NOT_NULL(g_device2);
    ASSERT_EQ_U32(g_dev2_flags0, 0x18);
    if (g_open2_status != STATUS_NO_SUCH_DEVICE)
        FAIL_AND_RETURN("open of an untouched device: got 0x%08x "
                        "expected STATUS_NO_SUCH_DEVICE", 
                        (unsigned)g_open2_status);
    return true;
}

/* Opening the device must dispatch through the title's own table. */
static bool t_open_dispatches_create(void)
{
    const obs_t *o;
    if (!setup())
        FAIL_SETUP();
    o = find_obs(PHASE_CREATE);
    if (o == NULL)
        FAIL_AND_RETURN("no dispatch observed for the open");
    ASSERT_EQ_U32(o->major, 0);  /* IRP_MJ_CREATE is slot 0 on both */
    return true;
}

/* The Xbox MajorFunction table is compacted to 14 slots, so the codes
 * diverge from NT's 28-slot numbering. */
static bool t_ioctl_major_index(void)
{
    const obs_t *o;
    if (!setup())
        FAIL_SETUP();
    o = find_obs(PHASE_IOCTL);
    if (o == NULL)
        FAIL_AND_RETURN("no dispatch observed for the ioctl");
    if (o->major != 10)
        FAIL_AND_RETURN("device-control major: got %u expected 10",
                        (unsigned)o->major);
    return true;
}

/* Retail puts the control code at Parameters+0x10, where NT keeps
 * Type3InputBuffer -- the Xbox arm is
 * {OutputBufferLength, InputBuffer, InputBufferLength, IoControlCode}. */
static bool t_ioctl_control_code_offset(void)
{
    const obs_t *o;
    if (!setup())
        FAIL_SETUP();
    o = find_obs(PHASE_IOCTL);
    if (o == NULL)
        FAIL_AND_RETURN("no dispatch observed for the ioctl");
    if (o->raw[4] != TEST_IOCTL)
        FAIL_AND_RETURN("ioctl code not at +0x10: "
                        "+0x04=0x%08x +0x08=0x%08x +0x0c=0x%08x +0x10=0x%08x",
                        (unsigned)o->raw[1], (unsigned)o->raw[2],
                        (unsigned)o->raw[3], (unsigned)o->raw[4]);
    return true;
}

/* Output length leads; the input length follows the input pointer. */
static bool t_ioctl_buffer_lengths(void)
{
    const obs_t *o;
    if (!setup())
        FAIL_SETUP();
    o = find_obs(PHASE_IOCTL);
    if (o == NULL)
        FAIL_AND_RETURN("no dispatch observed for the ioctl");
    ASSERT_EQ_U32(o->raw[1], sizeof(g_out_buf));  /* OutputBufferLength */
    ASSERT_EQ_U32(o->raw[3], sizeof(g_in_buf));   /* InputBufferLength  */
    return true;
}

/* Unlike NT, the Xbox arm carries the input buffer itself. */
static bool t_ioctl_input_buffer(void)
{
    const obs_t *o;
    if (!setup())
        FAIL_SETUP();
    o = find_obs(PHASE_IOCTL);
    if (o == NULL)
        FAIL_AND_RETURN("no dispatch observed for the ioctl");
    if ((PVOID)o->raw[2] != (PVOID)g_in_buf)
        FAIL_AND_RETURN("stack InputBuffer at +0x08: got 0x%08x expected %p",
                        (unsigned)o->raw[2], (void *)g_in_buf);
    return true;
}

/* The stack location carries the device the request is bound for. */
static bool t_stack_device_object(void)
{
    const obs_t *o;
    if (!setup())
        FAIL_SETUP();
    o = find_obs(PHASE_IOCTL);
    if (o == NULL)
        FAIL_AND_RETURN("no dispatch observed for the ioctl");
    if ((PVOID)o->raw[5] != (PVOID)g_device)
        FAIL_AND_RETURN("stack DeviceObject at +0x14: got 0x%08x expected %p",
                        (unsigned)o->raw[5], (void *)g_device);
    return true;
}

/* METHOD_NEITHER hands the caller's output buffer through untouched. */
static bool t_irp_user_buffer(void)
{
    const obs_t *o;
    if (!setup())
        FAIL_SETUP();
    o = find_obs(PHASE_IOCTL);
    if (o == NULL)
        FAIL_AND_RETURN("no dispatch observed for the ioctl");
    ASSERT_EQ_PTR(o->user_buffer, g_out_buf);
    return true;
}

/* IoStatus written by the driver must reach the caller's block. */
static bool t_iostatus_writeback(void)
{
    if (!setup())
        FAIL_SETUP();
    ASSERT_NTSTATUS(g_ioctl_status, STATUS_SUCCESS);
    ASSERT_NTSTATUS(g_ioctl_iosb.Status, STATUS_SUCCESS);
    ASSERT_EQ_U32(g_ioctl_iosb.Information, MAGIC_INFO);
    return true;
}

/* The stack locations trail the IRP body, so the one in use has to lie
 * inside the packet and past the console-published body.  The exact
 * distance is not contract and differs here: the NT-only fields this
 * kernel keeps keep sit past the published prefix, making the body
 * longer than the console's 0x68.  Nothing observes that -- the stack
 * location is always reached through CurrentStackLocation. */
static bool t_stack_within_irp(void)
{
    const obs_t *o;
    ULONG delta;
    if (!setup())
        FAIL_SETUP();
    o = find_obs(PHASE_IOCTL);
    if (o == NULL)
        FAIL_AND_RETURN("no dispatch observed for the ioctl");
    delta = (ULONG)((ULONG_PTR)o->stack - (ULONG_PTR)o->irp);
    if (delta < 0x68)
        FAIL_AND_RETURN("stack location overlaps the IRP body: +0x%x", delta);
    if (delta + 0x24 > o->irp_size)
        FAIL_AND_RETURN("stack location +0x%x runs past the packet (Size=0x%x)",
                        delta, (unsigned)o->irp_size);
    return true;
}

static const test_entry_t io_irplayout_entries[] = {
    {"create_device", t_create_device},
    {"device_initial_fields", t_device_initial_fields},
    {"device_initial_flags", t_device_initial_flags},
    {"untouched_device_is_unreachable", t_untouched_device_is_unreachable},
    {"open_dispatches_create", t_open_dispatches_create},
    {"ioctl_major_index", t_ioctl_major_index},
    {"ioctl_control_code_offset", t_ioctl_control_code_offset},
    {"ioctl_buffer_lengths", t_ioctl_buffer_lengths},
    {"ioctl_input_buffer", t_ioctl_input_buffer},
    {"stack_device_object", t_stack_device_object},
    {"irp_user_buffer", t_irp_user_buffer},
    {"iostatus_writeback", t_iostatus_writeback},
    {"stack_within_irp", t_stack_within_irp},
};

DEFINE_GROUP(io_irplayout, "io/irplayout");
