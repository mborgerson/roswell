/*
 * IoQueryFileInformation: the file query a title's own driver makes.
 *
 * The Nt-level query works on a handle, takes any class the file system
 * implements and reports through an IO_STATUS_BLOCK.  This one works on
 * the file object a title already holds -- and it is a much narrower
 * routine than its NT namesake: the console answers exactly three
 * classes and refuses every other one, and it takes the length from the
 * class rather than from the caller.
 */

#include "../harness.h"
#include <string.h>

#ifndef STATUS_INVALID_PARAMETER
#define STATUS_INVALID_PARAMETER ((NTSTATUS)0xC000000DL)
#endif

/* The three the console answers. */
#define CLASS_INTERNAL      6
#define CLASS_POSITION     14
#define CLASS_NETWORK_OPEN 34

static const char SCRATCH[] =
    "\\Device\\Harddisk0\\Partition1\\nxkrnl-api-ioquery.tmp";
static const char SCRATCH2[] =
    "\\Device\\Harddisk0\\Partition1\\nxkrnl-api-ioquery2.tmp";

#define PAYLOAD_BYTES 137

typedef struct {
    LARGE_INTEGER CreationTime;
    LARGE_INTEGER LastAccessTime;
    LARGE_INTEGER LastWriteTime;
    LARGE_INTEGER ChangeTime;
    LARGE_INTEGER AllocationSize;
    LARGE_INTEGER EndOfFile;
    ULONG         FileAttributes;
} network_open_t;

static ANSI_STRING str(const char *s)
{
    ANSI_STRING a;

    a.Buffer = (PCHAR)s;
    a.Length = (USHORT)strlen(s);
    a.MaximumLength = (USHORT)(a.Length + 1);
    return a;
}

/* A scratch file with a known length, plus the file object behind it.
 * The handle holds the reference the caller needs, so the pointer
 * reference taken here is released straight away. */
static NTSTATUS open_scratch(const char *path, ULONG bytes, HANDLE *h,
                             PVOID *fo)
{
    ANSI_STRING name = str(path);
    OBJECT_ATTRIBUTES oa;
    IO_STATUS_BLOCK iosb;
    static UCHAR payload[PAYLOAD_BYTES];
    NTSTATUS s;

    oa.RootDirectory = NULL;
    oa.ObjectName = &name;
    oa.Attributes = OBJ_CASE_INSENSITIVE;

    *h = NULL;
    *fo = NULL;
    memset(payload, 'q', sizeof(payload));
    s = NtCreateFile(h, GENERIC_READ | GENERIC_WRITE | SYNCHRONIZE, &oa,
                     &iosb, NULL, FILE_ATTRIBUTE_NORMAL,
                     FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                     FILE_OVERWRITE_IF,
                     FILE_SYNCHRONOUS_IO_NONALERT |
                         FILE_NON_DIRECTORY_FILE);
    if (!NT_SUCCESS(s))
        return s;

    if (bytes != 0)
        s = NtWriteFile(*h, NULL, NULL, NULL, &iosb, payload, bytes, NULL);
    if (NT_SUCCESS(s))
        s = ObReferenceObjectByHandle(*h, NULL, fo);
    if (!NT_SUCCESS(s)) {
        NtClose(*h);
        *h = NULL;
        return s;
    }
    ObfDereferenceObject(*fo);
    return STATUS_SUCCESS;
}

/* A second file object on a file that is already open. */
static NTSTATUS reopen_scratch(const char *path, HANDLE *h, PVOID *fo)
{
    ANSI_STRING name = str(path);
    OBJECT_ATTRIBUTES oa;
    IO_STATUS_BLOCK iosb;
    NTSTATUS s;

    oa.RootDirectory = NULL;
    oa.ObjectName = &name;
    oa.Attributes = OBJ_CASE_INSENSITIVE;

    *h = NULL;
    *fo = NULL;
    s = NtCreateFile(h, GENERIC_READ | SYNCHRONIZE, &oa, &iosb, NULL,
                     FILE_ATTRIBUTE_NORMAL,
                     FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                     FILE_OPEN,
                     FILE_SYNCHRONOUS_IO_NONALERT |
                         FILE_NON_DIRECTORY_FILE);
    if (!NT_SUCCESS(s))
        return s;

    s = ObReferenceObjectByHandle(*h, NULL, fo);
    if (!NT_SUCCESS(s)) {
        NtClose(*h);
        *h = NULL;
        return s;
    }
    ObfDereferenceObject(*fo);
    return STATUS_SUCCESS;
}

static void close_scratch(const char *path, HANDLE h)
{
    ANSI_STRING name = str(path);
    OBJECT_ATTRIBUTES oa;

    if (h != NULL) NtClose(h);
    oa.RootDirectory = NULL;
    oa.ObjectName = &name;
    oa.Attributes = OBJ_CASE_INSENSITIVE;
    NtDeleteFile(&oa);
}

/* Everything outside the three is STATUS_INVALID_PARAMETER -- including
 * classes the file system answers perfectly well through the Nt-level
 * query, FileStandardInformation among them. */
static bool t_only_three_classes_are_answered(void)
{
    static char line[48];
    UCHAR buffer[256];
    HANDLE h;
    PVOID fo;
    NTSTATUS s;
    bool ok = true;
    int cls;

    s = open_scratch(SCRATCH, PAYLOAD_BYTES, &h, &fo);
    if (!NT_SUCCESS(s)) FAIL_AND_RETURN("open -> 0x%08x", (unsigned)s);

    for (cls = 1; cls <= 40; cls++) {
        bool expected = (cls == CLASS_INTERNAL || cls == CLASS_POSITION ||
                         cls == CLASS_NETWORK_OPEN);
        ULONG returned = 0;

        memset(buffer, 0, sizeof(buffer));
        s = IoQueryFileInformation(fo, (FILE_INFORMATION_CLASS)cls,
                                   sizeof(buffer), buffer, &returned);
        line[cls - 1] = (s == STATUS_SUCCESS) ? '.'
                      : (s == STATUS_INVALID_PARAMETER) ? 'p' : '?';
        if ((s == STATUS_SUCCESS) != expected)
            ok = false;
    }
    line[40] = '\0';
    close_scratch(SCRATCH, h);

    if (!ok) FAIL_AND_RETURN("classes 1-40: %s", line);
    return true;
}

/* Position comes from the file object, so a read moves it. */
static bool t_position_follows_the_file_object(void)
{
    FILE_POSITION_INFORMATION pos;
    IO_STATUS_BLOCK iosb;
    LARGE_INTEGER zero;
    UCHAR buffer[16];
    HANDLE h;
    PVOID fo;
    ULONG returned = 0;
    NTSTATUS s;

    s = open_scratch(SCRATCH, PAYLOAD_BYTES, &h, &fo);
    if (!NT_SUCCESS(s)) FAIL_AND_RETURN("open -> 0x%08x", (unsigned)s);

    memset(&pos, 0xCC, sizeof(pos));
    s = IoQueryFileInformation(fo, FilePositionInformation, sizeof(pos),
                               &pos, &returned);
    if (!NT_SUCCESS(s)) {
        close_scratch(SCRATCH, h);
        FAIL_AND_RETURN("query -> 0x%08x", (unsigned)s);
    }
    if (pos.CurrentByteOffset.LowPart != PAYLOAD_BYTES) {
        close_scratch(SCRATCH, h);
        FAIL_AND_RETURN("after write: %u",
                        (unsigned)pos.CurrentByteOffset.LowPart);
    }

    zero.QuadPart = 0;
    s = NtReadFile(h, NULL, NULL, NULL, &iosb, buffer, sizeof(buffer), &zero);
    if (!NT_SUCCESS(s)) {
        close_scratch(SCRATCH, h);
        FAIL_AND_RETURN("read -> 0x%08x", (unsigned)s);
    }

    s = IoQueryFileInformation(fo, FilePositionInformation, sizeof(pos),
                               &pos, &returned);
    close_scratch(SCRATCH, h);
    ASSERT_NTSTATUS(s, STATUS_SUCCESS);
    ASSERT_EQ_U32(returned, sizeof(pos));
    ASSERT_EQ_U32(pos.CurrentByteOffset.LowPart, sizeof(buffer));
    return true;
}

/* The network-open class carries the size and attributes in one answer. */
static bool t_network_open_information(void)
{
    network_open_t info;
    HANDLE h;
    PVOID fo;
    ULONG returned = 0;
    NTSTATUS s;

    s = open_scratch(SCRATCH, PAYLOAD_BYTES, &h, &fo);
    if (!NT_SUCCESS(s)) FAIL_AND_RETURN("open -> 0x%08x", (unsigned)s);

    memset(&info, 0xCC, sizeof(info));
    s = IoQueryFileInformation(fo, (FILE_INFORMATION_CLASS)CLASS_NETWORK_OPEN,
                               sizeof(info), &info, &returned);
    if (!NT_SUCCESS(s)) {
        close_scratch(SCRATCH, h);
        FAIL_AND_RETURN("query -> 0x%08x", (unsigned)s);
    }
    close_scratch(SCRATCH, h);

    ASSERT_EQ_U32(returned, sizeof(info));
    ASSERT_EQ_U32(info.EndOfFile.LowPart, PAYLOAD_BYTES);
    ASSERT_EQ_U32(info.EndOfFile.HighPart, 0);
    /* Which bits a fresh file carries is the file system's business, so
     * only their presence is pinned here. */
    ASSERT_TRUE(info.FileAttributes != 0);
    ASSERT_TRUE(info.AllocationSize.QuadPart >= info.EndOfFile.QuadPart);
    ASSERT_TRUE(info.LastWriteTime.QuadPart != 0);
    ASSERT_TRUE(info.CreationTime.QuadPart != 0);
    return true;
}

/* The internal-information class names the file, so two file objects on
 * one file agree and two different files do not. */
static bool t_internal_information_names_the_file(void)
{
    FILE_INTERNAL_INFORMATION first, again, other;
    HANDLE h1 = NULL, h2 = NULL, h3 = NULL;
    PVOID fo1, fo2, fo3;
    ULONG returned = 0;
    NTSTATUS s;

    s = open_scratch(SCRATCH, PAYLOAD_BYTES, &h1, &fo1);
    if (NT_SUCCESS(s)) s = reopen_scratch(SCRATCH, &h2, &fo2);
    if (NT_SUCCESS(s)) s = open_scratch(SCRATCH2, 0, &h3, &fo3);
    if (!NT_SUCCESS(s)) {
        close_scratch(SCRATCH2, h3);
        close_scratch(SCRATCH, h2);
        close_scratch(SCRATCH, h1);
        FAIL_AND_RETURN("open -> 0x%08x", (unsigned)s);
    }

    memset(&first, 0xCC, sizeof(first));
    memset(&again, 0xCC, sizeof(again));
    memset(&other, 0xCC, sizeof(other));
    s = IoQueryFileInformation(fo1, FileInternalInformation, sizeof(first),
                               &first, &returned);
    if (NT_SUCCESS(s))
        s = IoQueryFileInformation(fo2, FileInternalInformation,
                                   sizeof(again), &again, &returned);
    if (NT_SUCCESS(s))
        s = IoQueryFileInformation(fo3, FileInternalInformation,
                                   sizeof(other), &other, &returned);

    NtClose(h2);
    close_scratch(SCRATCH2, h3);
    close_scratch(SCRATCH, h1);

    ASSERT_NTSTATUS(s, STATUS_SUCCESS);
    ASSERT_EQ_U32(returned, sizeof(first));
    ASSERT_TRUE(first.IndexNumber.QuadPart == again.IndexNumber.QuadPart);
    ASSERT_TRUE(first.IndexNumber.QuadPart != other.IndexNumber.QuadPart);
    return true;
}

/* The length is the class's, not the caller's: a length far too small
 * for the class is neither refused nor honoured. */
static bool t_the_length_argument_is_ignored(void)
{
    struct {
        network_open_t info;
        ULONG          tail;
    } probe;
    HANDLE h;
    PVOID fo;
    ULONG returned = 0;
    NTSTATUS s;

    s = open_scratch(SCRATCH, PAYLOAD_BYTES, &h, &fo);
    if (!NT_SUCCESS(s)) FAIL_AND_RETURN("open -> 0x%08x", (unsigned)s);

    memset(&probe, 0xCC, sizeof(probe));
    s = IoQueryFileInformation(fo, (FILE_INFORMATION_CLASS)CLASS_NETWORK_OPEN,
                               1, &probe.info, &returned);
    close_scratch(SCRATCH, h);

    ASSERT_NTSTATUS(s, STATUS_SUCCESS);
    ASSERT_EQ_U32(returned, sizeof(network_open_t));
    ASSERT_EQ_U32(probe.info.EndOfFile.LowPart, PAYLOAD_BYTES);
    /* ...and only the class's own length is written. */
    ASSERT_EQ_U32(probe.tail, 0xCCCCCCCC);
    return true;
}

static const test_entry_t io_ioquery_entries[] = {
    { "only_three_classes_are_answered", t_only_three_classes_are_answered,
      NULL },
    { "position_follows_the_file_object",
      t_position_follows_the_file_object, NULL },
    { "network_open_information", t_network_open_information, NULL },
    { "internal_information_names_the_file",
      t_internal_information_names_the_file, NULL },
    { "the_length_argument_is_ignored", t_the_length_argument_is_ignored,
      NULL },
};

DEFINE_GROUP(io_ioquery, "io/ioquery");
