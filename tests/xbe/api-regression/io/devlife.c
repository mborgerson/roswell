/*
 * IoDeleteDevice, the other half of the IoCreateDevice a title driver
 * calls when it publishes a device.
 *
 * A device with nothing open goes away at once and its name stops
 * resolving; a device with a handle still open is only marked, and the
 * name survives until the last handle closes.  Both halves are what a
 * title's Memory Unit driver relies on when a unit is pulled.
 */

#include "../harness.h"
#include <string.h>

#define FILE_DEVICE_UNKNOWN 0x00000022u
#define MJ_SLOTS 0x0E

#define DO_READY_SET   0x00000004u
#define DO_READY_CLEAR 0x00000010u

static NTSTATUS NTAPI pass_dispatch(PDEVICE_OBJECT dev, PIRP irp)
{
    (void)dev;
    irp->IoStatus.Status = STATUS_SUCCESS;
    irp->IoStatus.Information = 0;
    IofCompleteRequest(irp, IO_NO_INCREMENT);
    return STATUS_SUCCESS;
}

static DRIVER_OBJECT g_driver;
static bool g_driver_ready;

static void prepare_driver(void)
{
    int i;

    if (g_driver_ready)
        return;
    for (i = 0; i < MJ_SLOTS; i++)
        g_driver.MajorFunction[i] = pass_dispatch;
    g_driver_ready = true;
}

static NTSTATUS make_device(const char *name, USHORT len, PDEVICE_OBJECT *out)
{
    OBJECT_STRING dev_name = {
        .Length        = len,
        .MaximumLength = (USHORT)(len + 1),
        .Buffer        = (PCHAR)name,
    };
    NTSTATUS s;

    prepare_driver();
    *out = NULL;
    s = IoCreateDevice(&g_driver, 0, &dev_name, FILE_DEVICE_UNKNOWN, FALSE,
                       out);
    if (NT_SUCCESS(s) && *out != NULL) {
        (*out)->Flags |= DO_READY_SET;
        (*out)->Flags &= ~DO_READY_CLEAR;
    }
    return s;
}

static NTSTATUS open_device(const char *name, USHORT len, HANDLE *h)
{
    ANSI_STRING open_name = {
        .Length        = len,
        .MaximumLength = (USHORT)(len + 1),
        .Buffer        = (PCHAR)name,
    };
    OBJECT_ATTRIBUTES oa = {
        .RootDirectory = NULL,
        .ObjectName    = &open_name,
        .Attributes    = OBJ_CASE_INSENSITIVE,
    };
    IO_STATUS_BLOCK iosb;

    *h = NULL;
    return NtCreateFile(h, GENERIC_READ | SYNCHRONIZE, &oa, &iosb, NULL,
                        FILE_ATTRIBUTE_NORMAL, 0, FILE_OPEN,
                        FILE_NON_DIRECTORY_FILE |
                        FILE_SYNCHRONOUS_IO_NONALERT);
}

static const char NAME_UNUSED[] = "\\Device\\nxkrnlDevLifeA";
static const char NAME_HELD[]   = "\\Device\\nxkrnlDevLifeB";

static bool t_delete_unopened(void)
{
    PDEVICE_OBJECT dev;
    HANDLE h;
    NTSTATUS s;

    s = make_device(NAME_UNUSED, sizeof(NAME_UNUSED) - 1, &dev);
    ASSERT_NTSTATUS(s, STATUS_SUCCESS);

    /* Reachable while it exists. */
    s = open_device(NAME_UNUSED, sizeof(NAME_UNUSED) - 1, &h);
    ASSERT_NTSTATUS(s, STATUS_SUCCESS);
    NtClose(h);

    IoDeleteDevice(dev);

    /* ...and gone from the namespace afterwards. */
    s = open_device(NAME_UNUSED, sizeof(NAME_UNUSED) - 1, &h);
    if (NT_SUCCESS(s)) {
        NtClose(h);
        FAIL_AND_RETURN("deleted device still opens");
    }
    return true;
}

static bool t_delete_with_handle_open(void)
{
    PDEVICE_OBJECT dev;
    HANDLE held, again;
    NTSTATUS s;

    s = make_device(NAME_HELD, sizeof(NAME_HELD) - 1, &dev);
    ASSERT_NTSTATUS(s, STATUS_SUCCESS);

    s = open_device(NAME_HELD, sizeof(NAME_HELD) - 1, &held);
    ASSERT_NTSTATUS(s, STATUS_SUCCESS);

    /* The delete is only pending: the object outlives the call because
     * the open handle still references it. */
    IoDeleteDevice(dev);
    ASSERT_EQ_U32(dev->DeletePending, TRUE);

    /* A new open is refused even though the object is still there. */
    s = open_device(NAME_HELD, sizeof(NAME_HELD) - 1, &again);
    if (NT_SUCCESS(s)) {
        NtClose(again);
        NtClose(held);
        FAIL_AND_RETURN("delete-pending device accepted a new open");
    }

    NtClose(held);
    return true;
}

static const test_entry_t io_devlife_entries[] = {
    {"delete_unopened",         t_delete_unopened},
    {"delete_with_handle_open", t_delete_with_handle_open},
};

DEFINE_GROUP(io_devlife, "io/devlife");
