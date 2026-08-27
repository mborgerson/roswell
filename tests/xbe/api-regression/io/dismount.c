/*
 * IoDismountVolumeByName: forcing the volume off a named device.
 *
 * A cache partition is scratch space by design, so it is the safe
 * target -- the file system remounts on the next open, which is what
 * the round-trip case checks.  This group runs last for that reason.
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

static const test_entry_t io_dismount_entries[] = {
    { "a_name_that_resolves_to_nothing", t_a_name_that_resolves_to_nothing,
      NULL },
    { "a_device_with_no_volume_still_succeeds",
      t_a_device_with_no_volume_still_succeeds, NULL },
    { "a_mounted_volume_comes_back", t_a_mounted_volume_comes_back, NULL },
    { "an_open_handle_does_not_refuse_it",
      t_an_open_handle_does_not_refuse_it, NULL },
};

DEFINE_GROUP(io_dismount, "io/dismount");
