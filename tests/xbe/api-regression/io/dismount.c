/*
 * Forcing the volume off a device, by name and by object.
 *
 * A cache partition is scratch space by design, so it is the safe
 * target -- the file system remounts on the next open, which is what
 * the round-trip case checks.  This group runs last for that reason.
 *
 * The by-object form dispatches into the driver that owns the device:
 * a file system publishes a dismount entry point in its driver object,
 * and its answer is the caller's answer.
 */

#include "../harness.h"
#include <string.h>

#ifndef STATUS_OBJECT_NAME_NOT_FOUND
#define STATUS_OBJECT_NAME_NOT_FOUND ((NTSTATUS)0xC0000034L)
#endif

static const char CACHE_DEVICE[] = "\\Device\\Harddisk0\\Partition3";
static const char CACHE_FILE[] =
    "\\Device\\Harddisk0\\Partition3\\nxkrnl-api-dismount.tmp";
static const char RAW_DEVICE[] = "\\Device\\Harddisk0\\Partition0";
static const char NO_SUCH_DEVICE[] = "\\Device\\nxkrnl-no-such-device";

static ANSI_STRING str(const char *s)
{
    ANSI_STRING a;

    a.Buffer = (PCHAR)s;
    a.Length = (USHORT)strlen(s);
    a.MaximumLength = a.Length + 1;
    return a;
}

static NTSTATUS touch(const char *path, HANDLE *h)
{
    OBJECT_ATTRIBUTES oa;
    IO_STATUS_BLOCK iosb;
    ANSI_STRING name = str(path);

    oa.RootDirectory = NULL;
    oa.ObjectName = &name;
    oa.Attributes = OBJ_CASE_INSENSITIVE;
    *h = NULL;
    return NtCreateFile(h, GENERIC_READ | GENERIC_WRITE | SYNCHRONIZE, &oa,
                        &iosb, NULL, FILE_ATTRIBUTE_NORMAL, 0,
                        FILE_OPEN_IF, FILE_SYNCHRONOUS_IO_NONALERT);
}

static void unlink_cache_file(void)
{
    OBJECT_ATTRIBUTES oa;
    ANSI_STRING name = str(CACHE_FILE);

    oa.RootDirectory = NULL;
    oa.ObjectName = &name;
    oa.Attributes = OBJ_CASE_INSENSITIVE;
    NtDeleteFile(&oa);
}

static bool t_a_name_that_resolves_to_nothing(void)
{
    ANSI_STRING none = str(NO_SUCH_DEVICE);

    ASSERT_NTSTATUS(IoDismountVolumeByName(&none),
                    STATUS_OBJECT_NAME_NOT_FOUND);
    return true;
}

/* Anything that resolves reports success, whatever the file system
 * makes of the request: a raw partition has no volume to take down,
 * and a cache partition nothing has opened yet has nothing mounted. */
static bool t_a_device_with_no_volume_still_succeeds(void)
{
    ANSI_STRING raw = str(RAW_DEVICE);

    ASSERT_NTSTATUS(IoDismountVolumeByName(&raw), STATUS_SUCCESS);
    return true;
}

/* The real thing: mount the cache volume by touching a file, take it
 * down, and see it come back on the next open. */
static bool t_a_mounted_volume_comes_back(void)
{
    ANSI_STRING cache = str(CACHE_DEVICE);
    HANDLE h = NULL;
    NTSTATUS s;

    s = touch(CACHE_FILE, &h);
    if (!NT_SUCCESS(s)) FAIL_AND_RETURN("mount touch -> 0x%08x", (unsigned)s);
    NtClose(h);

    s = IoDismountVolumeByName(&cache);
    if (!NT_SUCCESS(s)) {
        unlink_cache_file();
        FAIL_AND_RETURN("dismount -> 0x%08x", (unsigned)s);
    }

    s = touch(CACHE_FILE, &h);
    if (NT_SUCCESS(s)) NtClose(h);
    unlink_cache_file();
    ASSERT_NTSTATUS(s, STATUS_SUCCESS);
    return true;
}

/* An open handle does not refuse the request. */
static bool t_an_open_handle_does_not_refuse_it(void)
{
    ANSI_STRING cache = str(CACHE_DEVICE);
    HANDLE h = NULL;
    NTSTATUS s, d;

    s = touch(CACHE_FILE, &h);
    if (!NT_SUCCESS(s)) FAIL_AND_RETURN("touch -> 0x%08x", (unsigned)s);

    d = IoDismountVolumeByName(&cache);
    NtClose(h);
    unlink_cache_file();
    ASSERT_NTSTATUS(d, STATUS_SUCCESS);
    return true;
}

/* --- the by-object form -------------------------------------------------- */

#define FILE_DEVICE_DISK_FILE_SYSTEM 0x00000008u
#define MJ_SLOTS    0x0E
#define GUARD_BYTES 0x100

/* A status nothing else returns, so the caller's answer says whether the
 * driver's own answer reached it. */
#define DRIVER_ANSWER ((NTSTATUS)0xC0DE0001L)

static const char PROBE_DEVICE[] = "\\Device\\nxkrnlDismountDev";

static volatile LONG g_dismount_calls;
static PDEVICE_OBJECT g_dismount_arg;

static NTSTATUS NTAPI pass_dispatch(PDEVICE_OBJECT dev, PIRP irp)
{
    (void)dev;
    irp->IoStatus.Status = STATUS_SUCCESS;
    irp->IoStatus.Information = 0;
    IofCompleteRequest(irp, IO_NO_INCREMENT);
    return STATUS_SUCCESS;
}

static NTSTATUS NTAPI dismount_entry(PDEVICE_OBJECT dev)
{
    g_dismount_calls++;
    g_dismount_arg = dev;
    return DRIVER_ANSWER;
}

/* The console's DRIVER_OBJECT ends at its fourteen-entry dispatch table
 * and the title owns the storage, so a guard behind it proves the
 * dismount path stops where it should. */
static struct {
    DRIVER_OBJECT drv;
    UCHAR         guard[GUARD_BYTES];
} g_guarded;

/* The device object behind a handle: what a title has to hand. */
static PDEVICE_OBJECT device_of(HANDLE h)
{
    PFILE_OBJECT fo = NULL;

    if (!NT_SUCCESS(ObReferenceObjectByHandle(h, &IoFileObjectType,
                                              (PVOID *)&fo)))
        return NULL;
    ObfDereferenceObject(fo);
    return fo->DeviceObject;
}

static bool t_a_device_object_takes_its_volume_down(void)
{
    HANDLE h = NULL;
    PDEVICE_OBJECT dev;
    NTSTATUS s;

    s = touch(CACHE_FILE, &h);
    if (!NT_SUCCESS(s)) FAIL_AND_RETURN("mount touch -> 0x%08x", (unsigned)s);
    dev = device_of(h);
    NtClose(h);
    unlink_cache_file();
    ASSERT_NOT_NULL(dev);

    s = IoDismountVolume(dev);
    if (!NT_SUCCESS(s)) FAIL_AND_RETURN("dismount -> 0x%08x", (unsigned)s);

    /* And the file system comes back on the next open. */
    s = touch(CACHE_FILE, &h);
    if (NT_SUCCESS(s)) NtClose(h);
    unlink_cache_file();
    ASSERT_NTSTATUS(s, STATUS_SUCCESS);
    return true;
}

/* A raw partition has no file system to take down and still succeeds. */
static bool t_a_raw_device_object_still_succeeds(void)
{
    ANSI_STRING name = str(RAW_DEVICE);
    OBJECT_ATTRIBUTES oa;
    IO_STATUS_BLOCK iosb;
    HANDLE h = NULL;
    PDEVICE_OBJECT dev;
    NTSTATUS s;

    oa.RootDirectory = NULL;
    oa.ObjectName = &name;
    oa.Attributes = OBJ_CASE_INSENSITIVE;
    s = NtOpenFile(&h, GENERIC_READ | SYNCHRONIZE, &oa, &iosb,
                   FILE_SHARE_READ | FILE_SHARE_WRITE,
                   FILE_SYNCHRONOUS_IO_NONALERT);
    if (!NT_SUCCESS(s)) FAIL_AND_RETURN("open -> 0x%08x", (unsigned)s);

    dev = device_of(h);
    NtClose(h);
    ASSERT_NOT_NULL(dev);
    ASSERT_NTSTATUS(IoDismountVolume(dev), STATUS_SUCCESS);
    return true;
}

/* The owning driver answers, and the answer is passed straight back. */
static bool t_the_owning_driver_answers(void)
{
    OBJECT_STRING dev_name = {
        .Length        = (USHORT)(sizeof(PROBE_DEVICE) - 1),
        .MaximumLength = (USHORT)sizeof(PROBE_DEVICE),
        .Buffer        = (PCHAR)PROBE_DEVICE,
    };
    PDEVICE_OBJECT dev = NULL;
    NTSTATUS s;
    unsigned i;

    memset(&g_guarded, 0, sizeof(g_guarded));
    for (i = 0; i < MJ_SLOTS; i++)
        g_guarded.drv.MajorFunction[i] = pass_dispatch;
    g_guarded.drv.DriverDismountVolume = dismount_entry;
    g_dismount_calls = 0;
    g_dismount_arg = NULL;

    s = IoCreateDevice(&g_guarded.drv, 0, &dev_name,
                       FILE_DEVICE_DISK_FILE_SYSTEM, FALSE, &dev);
    if (!NT_SUCCESS(s) || dev == NULL)
        FAIL_AND_RETURN("create -> 0x%08x", (unsigned)s);

    s = IoDismountVolume(dev);
    IoDeleteDevice(dev);

    ASSERT_NTSTATUS(s, DRIVER_ANSWER);
    ASSERT_EQ_U32(g_dismount_calls, 1);
    ASSERT_EQ_PTR(g_dismount_arg, dev);
    for (i = 0; i < GUARD_BYTES; i++)
        if (g_guarded.guard[i] != 0)
            FAIL_AND_RETURN("guard byte %u written", i);
    return true;
}

static const test_entry_t io_dismount_entries[] = {
    { "a_name_that_resolves_to_nothing", t_a_name_that_resolves_to_nothing,
      NULL },
    { "a_device_with_no_volume_still_succeeds",
      t_a_device_with_no_volume_still_succeeds, NULL },
    { "a_mounted_volume_comes_back", t_a_mounted_volume_comes_back, NULL },
    { "an_open_handle_does_not_refuse_it",
      t_an_open_handle_does_not_refuse_it, NULL },
    { "a_device_object_takes_its_volume_down",
      t_a_device_object_takes_its_volume_down, NULL },
    { "a_raw_device_object_still_succeeds",
      t_a_raw_device_object_still_succeeds, NULL },
    { "the_owning_driver_answers", t_the_owning_driver_answers, NULL },
};

DEFINE_GROUP(io_dismount, "io/dismount");
