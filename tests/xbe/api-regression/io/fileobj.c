/*
 * Title-visible FILE_OBJECT layout.
 *
 * Titles carry block-device drivers of their own, so title code holds
 * real file objects and reads them field by field -- every offset in the
 * head is contract, not internal detail.  The two traps are that the
 * console packs CurrentByteOffset to 4 (it sits at 0x14, not 0x18), and
 * that the flags are a single byte with a numbering of their own rather
 * than the wide word an NT kernel keeps.
 *
 * The head is spelled out here rather than taken from nxdk's
 * FILE_OBJECT: nxdk has the field ORDER right but declares the structure
 * unpacked, so its CurrentByteOffset 8-aligns to 0x18 and everything
 * from there on reads four bytes off the console.
 */

#include "../harness.h"
#include <stddef.h>
#include <string.h>

#pragma pack(push, 4)
typedef struct {
    CSHORT        Type;               /* 0x00 */
    UCHAR         AccessBits;         /* 0x02 */
    UCHAR         Flags;              /* 0x03 */
    PVOID         DeviceObject;       /* 0x04 */
    PVOID         FsContext;          /* 0x08 */
    PVOID         FsContext2;         /* 0x0c */
    NTSTATUS      FinalStatus;        /* 0x10 */
    LARGE_INTEGER CurrentByteOffset;  /* 0x14 */
    PVOID         RelatedFileObject;  /* 0x1c */
    PVOID         CompletionContext;  /* 0x20 */
    LONG          LockCount;          /* 0x24 */
    KEVENT        Lock;               /* 0x28 */
    KEVENT        Event;              /* 0x38 */
} xfo_t;                              /* 0x48 */
#pragma pack(pop)

typedef char xfo_offsets_pinned[
    (offsetof(xfo_t, CurrentByteOffset) == 0x14 &&
     offsetof(xfo_t, LockCount) == 0x24 &&
     offsetof(xfo_t, Lock) == 0x28 &&
     offsetof(xfo_t, Event) == 0x38 &&
     sizeof(xfo_t) == 0x48) ? 1 : -1];

/* Access bits, packed into the byte at 0x02. */
#define XFO_DELETE_PENDING 0x01
#define XFO_READ_ACCESS    0x02
#define XFO_WRITE_ACCESS   0x04
#define XFO_DELETE_ACCESS  0x08

static const char DIR_PATH[] =
    "\\Device\\Harddisk0\\Partition1\\nxkrnl-api-fileobj";

/* The console's flags byte, as measured on retail. */
#define XFO_SYNCHRONOUS_IO            0x01
#define XFO_ALERTABLE_IO              0x02
#define XFO_NO_INTERMEDIATE_BUFFERING 0x04
#define XFO_SEQUENTIAL_ONLY           0x08
#define XFO_OPENED                    0x20
#define XFO_RANDOM_ACCESS             0x40

static NTSTATUS open_at(const char *path, ACCESS_MASK access,
                        ULONG disposition, ULONG opts, HANDLE *h)
{
    ANSI_STRING name = {
        .Length        = (USHORT)strlen(path),
        .MaximumLength = (USHORT)(strlen(path) + 1),
        .Buffer        = (PCHAR)path,
    };
    OBJECT_ATTRIBUTES oa = {
        .RootDirectory = NULL,
        .ObjectName    = &name,
        .Attributes    = OBJ_CASE_INSENSITIVE,
    };
    IO_STATUS_BLOCK iosb;
    return NtCreateFile(h, access | SYNCHRONIZE, &oa, &iosb, NULL,
                        FILE_ATTRIBUTE_NORMAL, 0, disposition, opts);
}

static bool ensure_dir(void)
{
    HANDLE h;
    NTSTATUS s = open_at(DIR_PATH, GENERIC_READ, FILE_OPEN_IF,
                         FILE_DIRECTORY_FILE | FILE_SYNCHRONOUS_IO_NONALERT,
                         &h);
    if (!NT_SUCCESS(s))
        return false;
    NtClose(h);
    return true;
}

static void leaf_path(char *out, const char *leaf)
{
    strcpy(out, DIR_PATH);
    strcat(out, "\\");
    strcat(out, leaf);
}

/* Open a scratch file and hand back both the handle and the file object
 * behind it.  The handle holds the only reference the caller needs, so
 * the pointer reference taken here is released straight away. */
static NTSTATUS open_leaf_object(const char *leaf, ACCESS_MASK access,
                                 ULONG opts, HANDLE *h, xfo_t **fo)
{
    char path[160];
    PVOID object;
    NTSTATUS s;

    leaf_path(path, leaf);
    s = open_at(path, access, FILE_OVERWRITE_IF,
                opts | FILE_NON_DIRECTORY_FILE, h);
    if (!NT_SUCCESS(s))
        return s;

    s = ObReferenceObjectByHandle(*h, NULL, &object);
    if (!NT_SUCCESS(s)) {
        NtClose(*h);
        return s;
    }
    ObfDereferenceObject(object);
    *fo = (xfo_t *)object;
    return STATUS_SUCCESS;
}

static bool t_head_fields(void)
{
    HANDLE h;
    xfo_t *fo;
    NTSTATUS s;

    if (!ensure_dir())
        FAIL_AND_RETURN("scratch directory unavailable");

    s = open_leaf_object("head", GENERIC_READ | GENERIC_WRITE,
                         FILE_SYNCHRONOUS_IO_NONALERT, &h, &fo);
    ASSERT_NTSTATUS(s, STATUS_SUCCESS);

    /* IO_TYPE_FILE, and the device the file resolved onto. */
    ASSERT_EQ_U32(fo->Type, 5);
    ASSERT_NOT_NULL(fo->DeviceObject);
    /* The file system owns both context slots; at least the first is set
     * for an open file on every file system the console ships. */
    ASSERT_NOT_NULL(fo->FsContext);
    ASSERT_NTSTATUS(fo->FinalStatus, STATUS_SUCCESS);
    ASSERT_EQ_PTR(fo->RelatedFileObject, NULL);

    /* Both embedded events are self-linked list heads, which is what
     * makes their offsets identifiable rather than inferred. */
    ASSERT_EQ_PTR(fo->Lock.Header.WaitListHead.Flink,
                  &fo->Lock.Header.WaitListHead);
    ASSERT_EQ_PTR(fo->Event.Header.WaitListHead.Flink,
                  &fo->Event.Header.WaitListHead);
    /* Lock is an unsignalled synchronization event, Event a signalled
     * notification event, and the lock starts out unowned at -1. */
    ASSERT_EQ_U32(fo->Lock.Header.Type, SynchronizationEvent);
    ASSERT_EQ_U32(fo->Lock.Header.SignalState, 0);
    ASSERT_EQ_U32(fo->Event.Header.Type, NotificationEvent);
    ASSERT_EQ_U32(fo->Event.Header.SignalState, 1);
    ASSERT_EQ_U32(fo->LockCount, (ULONG)-1);

    NtClose(h);
    return true;
}

/* CurrentByteOffset is the field the packing decides: assume the
 * LARGE_INTEGER is 8-aligned and it -- and everything after it -- reads
 * four bytes off. */
static bool t_current_byte_offset_tracks_io(void)
{
    HANDLE h;
    xfo_t *fo;
    IO_STATUS_BLOCK iosb;
    NTSTATUS s;
    static UCHAR buf[512];
    LARGE_INTEGER at;

    if (!ensure_dir())
        FAIL_AND_RETURN("scratch directory unavailable");

    s = open_leaf_object("offset", GENERIC_READ | GENERIC_WRITE,
                         FILE_SYNCHRONOUS_IO_NONALERT, &h, &fo);
    ASSERT_NTSTATUS(s, STATUS_SUCCESS);

    ASSERT_EQ_U32(fo->CurrentByteOffset.LowPart, 0);
    ASSERT_EQ_U32(fo->CurrentByteOffset.HighPart, 0);

    memset(buf, 0xA5, sizeof(buf));
    s = NtWriteFile(h, NULL, NULL, NULL, &iosb, buf, sizeof(buf), NULL);
    if (!NT_SUCCESS(s)) {
        NtClose(h);
        FAIL_AND_RETURN("NtWriteFile -> 0x%08x", (unsigned)s);
    }
    ASSERT_EQ_U32(fo->CurrentByteOffset.LowPart, 512);
    ASSERT_EQ_U32(fo->CurrentByteOffset.HighPart, 0);

    /* An explicit offset moves it too, so the field is where the read
     * landed rather than merely a running total. */
    at.QuadPart = 0x140;
    s = NtReadFile(h, NULL, NULL, NULL, &iosb, buf, 16, &at);
    if (!NT_SUCCESS(s)) {
        NtClose(h);
        FAIL_AND_RETURN("NtReadFile -> 0x%08x", (unsigned)s);
    }
    ASSERT_EQ_U32(fo->CurrentByteOffset.LowPart, 0x150);
    ASSERT_EQ_U32(fo->CurrentByteOffset.HighPart, 0);

    NtClose(h);
    return true;
}

static bool t_access_bits(void)
{
    static const struct {
        const char *leaf;
        ACCESS_MASK access;
        UCHAR read, write, del;
    } cases[] = {
        {"acc_r",  GENERIC_READ,                 1, 0, 0},
        {"acc_rw", GENERIC_READ | GENERIC_WRITE, 1, 1, 0},
        {"acc_rd", GENERIC_READ | DELETE,        1, 0, 1},
    };
    unsigned i;

    if (!ensure_dir())
        FAIL_AND_RETURN("scratch directory unavailable");

    for (i = 0; i < sizeof(cases)/sizeof(cases[0]); i++) {
        HANDLE h;
        xfo_t *fo;
        NTSTATUS s = open_leaf_object(cases[i].leaf, cases[i].access,
                                      FILE_SYNCHRONOUS_IO_NONALERT, &h, &fo);
        ASSERT_NTSTATUS(s, STATUS_SUCCESS);

        UCHAR expect = (UCHAR)((cases[i].read ? XFO_READ_ACCESS : 0) |
                               (cases[i].write ? XFO_WRITE_ACCESS : 0) |
                               (cases[i].del ? XFO_DELETE_ACCESS : 0));
        if (fo->AccessBits != expect) {
            UCHAR got = fo->AccessBits;
            NtClose(h);
            FAIL_AND_RETURN("%s: access bits 0x%02x expected 0x%02x",
                            cases[i].leaf, got, expect);
        }
        NtClose(h);
    }
    return true;
}

/* The flags byte is a compact renumbering, not our wide word narrowed:
 * FILE_SEQUENTIAL_ONLY reads 0x08 where the NT constant is 0x20. */
static bool t_published_flags(void)
{
    static const struct {
        const char *leaf;
        ULONG opts;
        UCHAR expect;
    } cases[] = {
        {"fl_sync", FILE_SYNCHRONOUS_IO_NONALERT,
         XFO_OPENED | XFO_SYNCHRONOUS_IO},
        {"fl_alert", FILE_SYNCHRONOUS_IO_ALERT,
         XFO_OPENED | XFO_SYNCHRONOUS_IO | XFO_ALERTABLE_IO},
        {"fl_nobuf", FILE_SYNCHRONOUS_IO_NONALERT |
                     FILE_NO_INTERMEDIATE_BUFFERING,
         XFO_OPENED | XFO_SYNCHRONOUS_IO | XFO_NO_INTERMEDIATE_BUFFERING},
        {"fl_seq", FILE_SYNCHRONOUS_IO_NONALERT | FILE_SEQUENTIAL_ONLY,
         XFO_OPENED | XFO_SYNCHRONOUS_IO | XFO_SEQUENTIAL_ONLY},
        {"fl_rand", FILE_SYNCHRONOUS_IO_NONALERT | FILE_RANDOM_ACCESS,
         XFO_OPENED | XFO_SYNCHRONOUS_IO | XFO_RANDOM_ACCESS},
        /* Write-through is not carried in this byte at all: the open
         * reads the same as a plain synchronous one. */
        {"fl_wt", FILE_SYNCHRONOUS_IO_NONALERT | FILE_WRITE_THROUGH,
         XFO_OPENED | XFO_SYNCHRONOUS_IO},
    };
    unsigned i;

    if (!ensure_dir())
        FAIL_AND_RETURN("scratch directory unavailable");

    for (i = 0; i < sizeof(cases)/sizeof(cases[0]); i++) {
        HANDLE h;
        xfo_t *fo;
        NTSTATUS s = open_leaf_object(cases[i].leaf, GENERIC_READ,
                                      cases[i].opts, &h, &fo);
        ASSERT_NTSTATUS(s, STATUS_SUCCESS);
        if (fo->Flags != cases[i].expect) {
            UCHAR got = fo->Flags;
            NtClose(h);
            FAIL_AND_RETURN("%s: flags 0x%02x expected 0x%02x",
                            cases[i].leaf, got, cases[i].expect);
        }
        NtClose(h);
    }
    return true;
}

/* A write leaves the byte alone -- whatever the console tracks
 * file-modified with, it is not published here. */
static bool t_published_flags_survive_write(void)
{
    HANDLE h;
    xfo_t *fo;
    IO_STATUS_BLOCK iosb;
    NTSTATUS s;
    UCHAR before, buf[32];

    if (!ensure_dir())
        FAIL_AND_RETURN("scratch directory unavailable");

    s = open_leaf_object("fl_write", GENERIC_READ | GENERIC_WRITE,
                         FILE_SYNCHRONOUS_IO_NONALERT, &h, &fo);
    ASSERT_NTSTATUS(s, STATUS_SUCCESS);

    before = fo->Flags;
    memset(buf, 0x5A, sizeof(buf));
    s = NtWriteFile(h, NULL, NULL, NULL, &iosb, buf, sizeof(buf), NULL);
    if (!NT_SUCCESS(s)) {
        NtClose(h);
        FAIL_AND_RETURN("NtWriteFile -> 0x%08x", (unsigned)s);
    }
    if (fo->Flags != before) {
        UCHAR after = fo->Flags;
        NtClose(h);
        FAIL_AND_RETURN("flags 0x%02x -> 0x%02x across a write",
                        before, after);
    }
    NtClose(h);
    return true;
}

static const test_entry_t io_fileobj_entries[] = {
    {"head_fields",                 t_head_fields},
    {"current_byte_offset",         t_current_byte_offset_tracks_io},
    {"access_bits",                 t_access_bits},
    {"published_flags",             t_published_flags},
    {"published_flags_survive_write", t_published_flags_survive_write},
};

DEFINE_GROUP(io_fileobj, "io/fileobj");
