/*
 * Symbolic-link object lifecycle (IoCreateSymbolicLink /
 * NtOpenSymbolicLinkObject / NtQuerySymbolicLinkObject /
 * IoDeleteSymbolicLink), object-directory enumeration via
 * NtQueryDirectoryObject, and NtDuplicateObject handle semantics.
 */

#include "../harness.h"
#include <string.h>

#ifndef STATUS_OBJECT_NAME_NOT_FOUND
#define STATUS_OBJECT_NAME_NOT_FOUND ((NTSTATUS)0xC0000034L)
#endif
#ifndef STATUS_BUFFER_TOO_SMALL
#define STATUS_BUFFER_TOO_SMALL ((NTSTATUS)0xC0000023L)
#endif
#ifndef STATUS_INVALID_HANDLE
#define STATUS_INVALID_HANDLE ((NTSTATUS)0xC0000008L)
#endif
#ifndef DUPLICATE_CLOSE_SOURCE
#define DUPLICATE_CLOSE_SOURCE 0x00000001
#endif

static const char LINK_NAME[]   = "\\??\\T:";
static const char LINK_TARGET[] = "\\Device\\CdRom0";

static void init_str(OBJECT_STRING *s, const char *str)
{
    s->Buffer = (PCHAR)str;
    s->Length = (USHORT)strlen(str);
    s->MaximumLength = s->Length + 1;
}

static NTSTATUS open_link(HANDLE *h)
{
    OBJECT_STRING name;
    init_str(&name, LINK_NAME);
    OBJECT_ATTRIBUTES oa = {
        .RootDirectory = NULL,
        .ObjectName    = &name,
        .Attributes    = OBJ_CASE_INSENSITIVE,
    };
    return NtOpenSymbolicLinkObject(h, &oa);
}

static bool t_create_open_query_delete(void)
{
    OBJECT_STRING name, target;
    init_str(&name, LINK_NAME);
    init_str(&target, LINK_TARGET);

    NTSTATUS s = IoCreateSymbolicLink(&name, &target);
    ASSERT_NTSTATUS(s, STATUS_SUCCESS);

    HANDLE h = NULL;
    s = open_link(&h);
    if (!NT_SUCCESS(s)) {
        IoDeleteSymbolicLink(&name);
        FAIL_AND_RETURN("NtOpenSymbolicLinkObject -> 0x%08x", (unsigned)s);
    }

    char buf[64];
    OBJECT_STRING out = {
        .Buffer = buf, .Length = 0, .MaximumLength = sizeof(buf),
    };
    ULONG retlen = 0;
    s = NtQuerySymbolicLinkObject(h, &out, &retlen);
    NtClose(h);
    if (!NT_SUCCESS(s)) {
        IoDeleteSymbolicLink(&name);
        FAIL_AND_RETURN("NtQuerySymbolicLinkObject -> 0x%08x", (unsigned)s);
    }
    if (out.Length != strlen(LINK_TARGET) ||
        memcmp(out.Buffer, LINK_TARGET, out.Length) != 0) {
        IoDeleteSymbolicLink(&name);
        FAIL_AND_RETURN("target mismatch: %.*s", out.Length, out.Buffer);
    }

    s = IoDeleteSymbolicLink(&name);
    ASSERT_NTSTATUS(s, STATUS_SUCCESS);

    s = open_link(&h);
    ASSERT_NTSTATUS(s, STATUS_OBJECT_NAME_NOT_FOUND);
    return true;
}

static bool t_query_short_buffer(void)
{
    OBJECT_STRING name, target;
    init_str(&name, LINK_NAME);
    init_str(&target, LINK_TARGET);

    NTSTATUS s = IoCreateSymbolicLink(&name, &target);
    ASSERT_NTSTATUS(s, STATUS_SUCCESS);

    HANDLE h = NULL;
    s = open_link(&h);
    if (!NT_SUCCESS(s)) {
        IoDeleteSymbolicLink(&name);
        FAIL_AND_RETURN("NtOpenSymbolicLinkObject -> 0x%08x", (unsigned)s);
    }

    char tiny[4];
    OBJECT_STRING out = {
        .Buffer = tiny, .Length = 0, .MaximumLength = sizeof(tiny),
    };
    s = NtQuerySymbolicLinkObject(h, &out, NULL);
    NtClose(h);
    IoDeleteSymbolicLink(&name);
    ASSERT_NTSTATUS(s, STATUS_BUFFER_TOO_SMALL);
    return true;
}

/* Enumerate \Device and expect to find CdRom0.  The name data must be
 * in-buffer ANSI (OBJECT_STRING entries), so a raw byte scan finds it
 * without depending on entry stride. */
static bool t_query_directory_object(void)
{
    OBJECT_STRING name;
    init_str(&name, "\\Device");
    OBJECT_ATTRIBUTES oa = {
        .RootDirectory = NULL,
        .ObjectName    = &name,
        .Attributes    = OBJ_CASE_INSENSITIVE,
    };

    HANDLE h = NULL;
    NTSTATUS s = ObOpenObjectByName(&oa, &ObDirectoryObjectType, NULL, &h);
    ASSERT_NTSTATUS(s, STATUS_SUCCESS);

    static const char NEEDLE[] = "CdRom0";
    char buf[1024];
    ULONG ctx = 0, retlen = 0;
    BOOLEAN restart = TRUE;
    bool found = false;
    int calls = 0;

    for (;;) {
        memset(buf, 0, sizeof(buf));
        s = NtQueryDirectoryObject(h, buf, sizeof(buf), restart, &ctx,
                                   &retlen);
        restart = FALSE;
        if (!NT_SUCCESS(s))
            break;
        calls++;
        for (size_t i = 0; i + sizeof(NEEDLE) - 1 <= sizeof(buf); i++) {
            if (memcmp(buf + i, NEEDLE, sizeof(NEEDLE) - 1) == 0) {
                found = true;
                break;
            }
        }
        if (found || calls > 64)
            break;
    }
    NtClose(h);

    if (calls == 0)
        FAIL_AND_RETURN("first NtQueryDirectoryObject -> 0x%08x", (unsigned)s);
    ASSERT_TRUE(found);
    return true;
}

static bool t_duplicate_keeps_object_alive(void)
{
    HANDLE h = NULL, dup = NULL;
    NTSTATUS s = NtCreateEvent(&h, NULL, NotificationEvent, TRUE);
    ASSERT_NTSTATUS(s, STATUS_SUCCESS);

    s = NtDuplicateObject(h, &dup, 0);
    if (!NT_SUCCESS(s)) {
        NtClose(h);
        FAIL_AND_RETURN("NtDuplicateObject -> 0x%08x", (unsigned)s);
    }
    ASSERT_TRUE(dup != h);

    NtClose(h);

    /* The duplicate must still reference the (signaled) event. */
    LARGE_INTEGER zero = { .QuadPart = 0 };
    HANDLE handles[1] = { dup };
    s = NtWaitForMultipleObjectsEx(1, handles, WaitAny, UserMode, FALSE,
                                   &zero);
    NtClose(dup);
    ASSERT_NTSTATUS(s, STATUS_WAIT_0);
    return true;
}

static bool t_duplicate_close_source(void)
{
    HANDLE h = NULL, dup = NULL;
    NTSTATUS s = NtCreateEvent(&h, NULL, NotificationEvent, TRUE);
    ASSERT_NTSTATUS(s, STATUS_SUCCESS);

    s = NtDuplicateObject(h, &dup, DUPLICATE_CLOSE_SOURCE);
    ASSERT_NTSTATUS(s, STATUS_SUCCESS);

    /* The source slot is freed and may be recycled as the duplicate's
     * (retail does exactly that), so the source value must not be
     * touched -- only the duplicate is usable. */
    LARGE_INTEGER zero = { .QuadPart = 0 };
    HANDLE handles[1] = { dup };
    s = NtWaitForMultipleObjectsEx(1, handles, WaitAny, UserMode, FALSE,
                                   &zero);
    NtClose(dup);
    ASSERT_NTSTATUS(s, STATUS_WAIT_0);
    return true;
}

/* A symbolic link the IO manager creates is permanent: its name stays
 * in the directory when the last handle to it closes.
 * ObMakeTemporaryObject clears that bit, so the name is dropped on the
 * close instead -- the step IoDeleteDevice takes on a named device
 * object before releasing it.  Closing a handle is what triggers the
 * check, so the link is opened rather than merely referenced. */
static bool link_resolves(const char *link_name)
{
    OBJECT_STRING name;
    OBJECT_ATTRIBUTES oa;
    HANDLE h = NULL;

    init_str(&name, link_name);
    oa.RootDirectory = NULL;
    oa.ObjectName    = &name;
    oa.Attributes    = OBJ_CASE_INSENSITIVE;
    if (!NT_SUCCESS(NtOpenSymbolicLinkObject(&h, &oa)))
        return false;
    NtClose(h);
    return true;
}

/* Create the link, open it, and hand back both the handle and the
 * object behind it. */
static NTSTATUS hold_link(const char *link_name, HANDLE *h, PVOID *object)
{
    OBJECT_STRING name, target;
    OBJECT_ATTRIBUTES oa;
    NTSTATUS s;

    init_str(&name, link_name);
    init_str(&target, LINK_TARGET);
    *h = NULL;
    *object = NULL;

    s = IoCreateSymbolicLink(&name, &target);
    if (!NT_SUCCESS(s))
        return s;

    oa.RootDirectory = NULL;
    oa.ObjectName    = &name;
    oa.Attributes    = OBJ_CASE_INSENSITIVE;
    s = NtOpenSymbolicLinkObject(h, &oa);
    if (!NT_SUCCESS(s)) {
        IoDeleteSymbolicLink(&name);
        return s;
    }

    s = ObReferenceObjectByHandle(*h, NULL, object);
    if (!NT_SUCCESS(s)) {
        NtClose(*h);
        IoDeleteSymbolicLink(&name);
    }
    return s;
}

static bool t_make_temporary_unnames(void)
{
    static const char PERM_NAME[] = "\\??\\nxkrnlPermLink";
    static const char TEMP_NAME[] = "\\??\\nxkrnlTempLink";
    OBJECT_STRING perm;
    HANDLE h;
    PVOID object;
    bool perm_named, temp_named_held, temp_named_closed;
    NTSTATUS s;

    /* Permanent: the name survives the close. */
    s = hold_link(PERM_NAME, &h, &object);
    ASSERT_NTSTATUS(s, STATUS_SUCCESS);
    ObfDereferenceObject(object);
    NtClose(h);
    perm_named = link_resolves(PERM_NAME);
    init_str(&perm, PERM_NAME);
    IoDeleteSymbolicLink(&perm);

    /* Temporary: the same link loses its name on the close, but not on
     * the ObMakeTemporaryObject call itself. */
    s = hold_link(TEMP_NAME, &h, &object);
    ASSERT_NTSTATUS(s, STATUS_SUCCESS);
    ObMakeTemporaryObject(object);
    temp_named_held = link_resolves(TEMP_NAME);
    ObfDereferenceObject(object);
    NtClose(h);
    temp_named_closed = link_resolves(TEMP_NAME);

    if (!perm_named)
        FAIL_AND_RETURN("permanent link lost its name on close");
    if (!temp_named_held)
        FAIL_AND_RETURN("temporary link unnamed while still open");
    if (temp_named_closed) {
        OBJECT_STRING temp;
        init_str(&temp, TEMP_NAME);
        IoDeleteSymbolicLink(&temp);
        FAIL_AND_RETURN("temporary link kept its name past its last handle");
    }
    return true;
}

static const test_entry_t ob_symlink_entries[] = {
    {"create_open_query_delete", t_create_open_query_delete},
    {"query_short_buffer",       t_query_short_buffer},
    {"query_directory_object",   t_query_directory_object},
    {"duplicate_keeps_object_alive", t_duplicate_keeps_object_alive},
    {"duplicate_close_source",   t_duplicate_close_source},
    {"make_temporary_unnames",   t_make_temporary_unnames},
};

DEFINE_GROUP(ob_symlink, "ob/symlink");
