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

/*
 * The console's DRIVER_OBJECT ends at its fourteen-entry dispatch table
 * and the title owns the storage, so nothing past it belongs to the
 * kernel.  NT's structure is far longer, and the fields its unload path
 * wants -- a device list, flags it writes as well as reads, an unload
 * routine it calls -- all land beyond the console's end, in whatever the
 * title keeps there.  A guard after the driver object proves the delete
 * path stops where it should.
 *
 * The offsets below are NT's, measured from the start of the driver
 * object: it keeps the list and the flags under the window and the
 * unload routine inside it.  Zeros underneath let a kernel that mistook
 * the two structures run its whole tail rather than stop at its first
 * check, and a callable routine throughout the window keeps it from
 * jumping to a wild address on the way.
 *
 * It does not keep it alive: a kernel that gets this wrong goes on to
 * make the title's structure a temporary object and takes the box down
 * a moment later, so a regression here ends the run rather than
 * reporting.  That is why this case is last in its group.
 */
#define GUARD_BYTES 0x100
#define HOOK_LO     0xA0
#define HOOK_HI     0x100

static const char NAME_GUARD[] = "\\Device\\nxkrnlguard";

static volatile LONG g_unload_calls;

static VOID NTAPI unload_probe(PVOID context)
{
    (void)context;
    g_unload_calls++;
}

static struct {
    DRIVER_OBJECT drv;
    UCHAR         guard[GUARD_BYTES];
} g_guarded;

static bool t_delete_leaves_the_driver_object_alone(void)
{
    OBJECT_STRING dev_name = {
        .Length        = (USHORT)(sizeof(NAME_GUARD) - 1),
        .MaximumLength = (USHORT)sizeof(NAME_GUARD),
        .Buffer        = (PCHAR)NAME_GUARD,
    };
    UCHAR *base = (UCHAR *)(void *)&g_guarded;
    const unsigned tail = sizeof(DRIVER_OBJECT);
    UCHAR expected[sizeof(g_guarded)];
    PDEVICE_OBJECT dev = NULL;
    NTSTATUS s;
    unsigned i;

    ASSERT_TRUE(HOOK_HI <= sizeof(g_guarded));

    memset(&g_guarded, 0, sizeof(g_guarded));
    for (i = 0; i < MJ_SLOTS; i++)
        g_guarded.drv.MajorFunction[i] = pass_dispatch;
    for (i = HOOK_LO; i + sizeof(PVOID) <= HOOK_HI; i += sizeof(PVOID))
        *(PVOID *)(void *)(base + i) = (PVOID)unload_probe;
    memcpy(expected, base, sizeof(g_guarded));

    g_unload_calls = 0;
    s = IoCreateDevice(&g_guarded.drv, 0, &dev_name, FILE_DEVICE_UNKNOWN,
                       FALSE, &dev);
    if (!NT_SUCCESS(s)) FAIL_AND_RETURN("create -> 0x%08x", (unsigned)s);
    dev->Flags |= DO_READY_SET;
    dev->Flags &= ~DO_READY_CLEAR;

    IoDeleteDevice(dev);

    if (g_unload_calls != 0)
        FAIL_AND_RETURN("the delete called through the driver object %ld time(s)",
                        (long)g_unload_calls);
    for (i = tail; i < sizeof(g_guarded); i++) {
        if (base[i] != expected[i])
            FAIL_AND_RETURN("the delete wrote driver+0x%x: 0x%02x -> 0x%02x",
                            i, expected[i], base[i]);
    }
    /* The console's own fields are still the title's, too. */
    for (i = 0; i < MJ_SLOTS; i++)
        ASSERT_EQ_PTR(g_guarded.drv.MajorFunction[i], pass_dispatch);
    return true;
}

static const test_entry_t io_devlife_entries[] = {
    {"delete_unopened",         t_delete_unopened},
    {"delete_with_handle_open", t_delete_with_handle_open},
    {"delete_leaves_the_driver_object_alone",
     t_delete_leaves_the_driver_object_alone},
};

DEFINE_GROUP(io_devlife, "io/devlife");
