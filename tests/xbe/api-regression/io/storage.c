/*
 * Storage-stack dispatch contracts: NtFlushBuffersFile and
 * IOCTL_STORAGE_CHECK_VERIFY.
 *
 * Regression coverage for the storage dispatch trims (IRP_MJ_SHUTDOWN /
 * IRP_MJ_FLUSH_BUFFERS slots in classpnp/partmgr/mountmgr, and the
 * ClassDeviceControl CHECK_VERIFY arm): both surfaces are reachable by a
 * title through NtFlushBuffersFile on HDD handles and
 * NtDeviceIoControlFile on the DVD device, so the statuses they return
 * are title-observable contract.
 *
 * Retail-validated semantics:
 *  - NtFlushBuffersFile on a written HDD file and on the Partition1
 *    DASD volume handle both return STATUS_SUCCESS.  Retail does not
 *    require write access on the handle for the flush (a GENERIC_READ
 *    volume handle flushes fine); nxkrnl returns STATUS_ACCESS_DENIED
 *    there, tracked as a TODO test below.
 *  - IOCTL_STORAGE_CHECK_VERIFY{,2} against \Device\CdRom0 return
 *    STATUS_INVALID_DEVICE_REQUEST in every shape probed (no buffer,
 *    ULONG change-count buffer, undersized buffer), media present.
 *    Retail simply does not implement the CHECK_VERIFY contract; DVD
 *    arrival is signalled out-of-band (SMC tray state), so a media
 *    change-count never exists.  The status is the same for all
 *    buffer shapes, i.e. it is rejected before any buffer validation.
 * NOTE: only the media-present tray state is observable under xemu;
 * an IOCTL the retail kernel rejects unconditionally has no
 * tray-dependent arm to compare.
 */

#include "../harness.h"
#include <string.h>

#ifndef FILE_DELETE_ON_CLOSE
#define FILE_DELETE_ON_CLOSE 0x00001000
#endif
#ifndef METHOD_BUFFERED
#define METHOD_BUFFERED 0
#endif
#ifndef FILE_ANY_ACCESS
#define FILE_ANY_ACCESS  0
#endif
#ifndef FILE_READ_ACCESS
#define FILE_READ_ACCESS 1
#endif

#define IOCTL_STORAGE_BASE 0x0000002d
#define IOCTL_STORAGE_CHECK_VERIFY \
    CTL_CODE(IOCTL_STORAGE_BASE, 0x0200, METHOD_BUFFERED, FILE_READ_ACCESS)
#define IOCTL_STORAGE_CHECK_VERIFY2 \
    CTL_CODE(IOCTL_STORAGE_BASE, 0x0200, METHOD_BUFFERED, FILE_ANY_ACCESS)

#ifndef STATUS_INVALID_DEVICE_REQUEST
#define STATUS_INVALID_DEVICE_REQUEST ((NTSTATUS)0xC0000010L)
#endif

static const char FLUSH_SCRATCH[] =
    "\\Device\\Harddisk0\\Partition1\\nxkrnl-api-flush.tmp";
static const char VOLUME_PATH[] = "\\Device\\Harddisk0\\Partition1";
static const char DVD_PATH[]    = "\\Device\\CdRom0";

static NTSTATUS open_by_path(const char *path, USHORT len, ACCESS_MASK access,
                             ULONG disposition, ULONG opts, HANDLE *h)
{
    IO_STATUS_BLOCK iosb;
    ANSI_STRING name = {
        .Length        = len,
        .MaximumLength = (USHORT)(len + 1),
        .Buffer        = (PCHAR)path,
    };
    OBJECT_ATTRIBUTES oa = {
        .RootDirectory = NULL,
        .ObjectName    = &name,
        .Attributes    = OBJ_CASE_INSENSITIVE,
    };
    return NtCreateFile(h, access | SYNCHRONIZE, &oa, &iosb, NULL,
                        FILE_ATTRIBUTE_NORMAL, FILE_SHARE_READ | FILE_SHARE_WRITE,
                        disposition, opts | FILE_SYNCHRONOUS_IO_NONALERT);
}

static bool t_flush_hdd_file(void)
{
    HANDLE h;
    IO_STATUS_BLOCK iosb;
    static char buf[1024];
    NTSTATUS s;

    s = open_by_path(FLUSH_SCRATCH, sizeof(FLUSH_SCRATCH) - 1,
                     GENERIC_READ | GENERIC_WRITE, FILE_OVERWRITE_IF,
                     FILE_NON_DIRECTORY_FILE | FILE_DELETE_ON_CLOSE, &h);
    ASSERT_NTSTATUS(s, STATUS_SUCCESS);

    memset(buf, 0x3c, sizeof(buf));
    s = NtWriteFile(h, NULL, NULL, NULL, &iosb, buf, sizeof(buf), NULL);
    if (!NT_SUCCESS(s)) {
        NtClose(h);
        FAIL_AND_RETURN("NtWriteFile -> 0x%08x", (unsigned)s);
    }

    s = NtFlushBuffersFile(h, &iosb);
    NtClose(h);
    /* Retail: flushing a written data-partition file succeeds. */
    ASSERT_NTSTATUS(s, STATUS_SUCCESS);
    ASSERT_NTSTATUS(iosb.Status, STATUS_SUCCESS);
    return true;
}

static bool t_flush_volume_handle(void)
{
    HANDLE h;
    IO_STATUS_BLOCK iosb;
    NTSTATUS s;

    /* A DASD open of the volume itself: this is the flush shape that
     * reaches the storage stack below the FSD (the per-file flush
     * never leaves the FSD). */
    s = open_by_path(VOLUME_PATH, sizeof(VOLUME_PATH) - 1,
                     GENERIC_READ | GENERIC_WRITE, FILE_OPEN, 0, &h);
    ASSERT_NTSTATUS(s, STATUS_SUCCESS);

    s = NtFlushBuffersFile(h, &iosb);
    NtClose(h);
    /* Retail: STATUS_SUCCESS even though the PATA disk's flush support
     * is not title-visible -- an unsupported lower-edge flush must be
     * swallowed by the FSD, not propagated. */
    ASSERT_NTSTATUS(s, STATUS_SUCCESS);
    return true;
}

static bool t_flush_volume_readonly(void)
{
    HANDLE h;
    IO_STATUS_BLOCK iosb;
    NTSTATUS s;

    s = open_by_path(VOLUME_PATH, sizeof(VOLUME_PATH) - 1,
                     GENERIC_READ, FILE_OPEN, 0, &h);
    ASSERT_NTSTATUS(s, STATUS_SUCCESS);

    s = NtFlushBuffersFile(h, &iosb);
    NtClose(h);
    /* Retail: SUCCESS -- no write-access check on the flush path. */
    ASSERT_NTSTATUS(s, STATUS_SUCCESS);
    return true;
}

static NTSTATUS dvd_ioctl(ULONG code, PVOID outbuf, ULONG outlen,
                          IO_STATUS_BLOCK *iosb)
{
    HANDLE h;
    NTSTATUS s = open_by_path(DVD_PATH, sizeof(DVD_PATH) - 1,
                              GENERIC_READ, FILE_OPEN, 0, &h);
    if (!NT_SUCCESS(s))
        return s;
    memset(iosb, 0x5a, sizeof(*iosb));
    s = NtDeviceIoControlFile(h, NULL, NULL, NULL, iosb, code,
                              NULL, 0, outbuf, outlen);
    NtClose(h);
    return s;
}

static bool t_dvd_check_verify(void)
{
    IO_STATUS_BLOCK iosb;
    NTSTATUS s = dvd_ioctl(IOCTL_STORAGE_CHECK_VERIFY, NULL, 0, &iosb);
    /* Retail: not implemented, media state notwithstanding. */
    if (s != STATUS_INVALID_DEVICE_REQUEST)
        FAIL_AND_RETURN("CHECK_VERIFY -> 0x%08x info=%u", (unsigned)s,
                        (unsigned)iosb.Information);
    return true;
}

static bool t_dvd_check_verify2(void)
{
    IO_STATUS_BLOCK iosb;
    NTSTATUS s = dvd_ioctl(IOCTL_STORAGE_CHECK_VERIFY2, NULL, 0, &iosb);
    if (s != STATUS_INVALID_DEVICE_REQUEST)
        FAIL_AND_RETURN("CHECK_VERIFY2 -> 0x%08x info=%u", (unsigned)s,
                        (unsigned)iosb.Information);
    return true;
}

static bool t_dvd_check_verify_count(void)
{
    IO_STATUS_BLOCK iosb;
    ULONG count = 0xdeadbeef;
    NTSTATUS s = dvd_ioctl(IOCTL_STORAGE_CHECK_VERIFY, &count, sizeof(count),
                           &iosb);
    /* Retail: still invalid -- no media change count exists, and the
     * caller's count buffer is never written. */
    if (s != STATUS_INVALID_DEVICE_REQUEST)
        FAIL_AND_RETURN("CHECK_VERIFY(count) -> 0x%08x info=%u count=0x%08x",
                        (unsigned)s, (unsigned)iosb.Information,
                        (unsigned)count);
    if (count != 0xdeadbeef)
        FAIL_AND_RETURN("CHECK_VERIFY(count) wrote count=0x%08x",
                        (unsigned)count);
    return true;
}

static bool t_dvd_check_verify_small_buffer(void)
{
    IO_STATUS_BLOCK iosb;
    USHORT tiny = 0;
    NTSTATUS s = dvd_ioctl(IOCTL_STORAGE_CHECK_VERIFY, &tiny, sizeof(tiny),
                           &iosb);
    /* Retail: same rejection -- no buffer-size arm runs at all. */
    if (s != STATUS_INVALID_DEVICE_REQUEST)
        FAIL_AND_RETURN("CHECK_VERIFY(tiny) -> 0x%08x info=%u", (unsigned)s,
                        (unsigned)iosb.Information);
    return true;
}

static const test_entry_t io_storage_entries[] = {
    {"flush_hdd_file",               t_flush_hdd_file},
    {"flush_volume_handle",          t_flush_volume_handle},
    {"flush_volume_readonly",        t_flush_volume_readonly,
     .todo = "retail flushes a read-only volume handle; "
             "nxkrnl returns ACCESS_DENIED"},
    {"dvd_check_verify",             t_dvd_check_verify},
    {"dvd_check_verify2",            t_dvd_check_verify2},
    {"dvd_check_verify_count",       t_dvd_check_verify_count},
    {"dvd_check_verify_small_buffer", t_dvd_check_verify_small_buffer},
};

DEFINE_GROUP(io_storage, "io/storage");
