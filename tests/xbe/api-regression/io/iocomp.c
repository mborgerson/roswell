/*
 * I/O completion ports, as titles see them.
 *
 * An IoCompletion object is a thin wrapper over a KQUEUE: the packet a
 * setter hands in (KeyContext, ApcContext, an NTSTATUS and an
 * information value) is queued, and a remover blocks until one is
 * available.  The XAPI's overlapped-I/O layer is built on this, so the
 * queue's ordering, its depth accounting and the blocking behavior of a
 * timed-out remove are all title-visible.
 */

#include "../harness.h"
#include <string.h>

#ifndef STATUS_TIMEOUT
#define STATUS_TIMEOUT ((NTSTATUS)0x00000102L)
#endif

#ifndef OBJ_OPENIF
#define OBJ_OPENIF 0x00000080L
#endif

#define IO_COMPLETION_ALL_ACCESS 0x001F0003

/* KOBJECTS: the value the retail kernel stamps into a queue. */
#define QUEUE_OBJECT 4

static NTSTATUS make_port(PHANDLE h, ULONG count)
{
    *h = NULL;
    return NtCreateIoCompletion(h, IO_COMPLETION_ALL_ACCESS, NULL, count);
}

static void init_named_oa(OBJECT_ATTRIBUTES *oa, ANSI_STRING *name,
                          const char *str)
{
    name->Buffer = (PCHAR)str;
    name->Length = (USHORT)strlen(str);
    name->MaximumLength = name->Length + 1;
    oa->RootDirectory = ObWin32NamedObjectsDirectory();
    oa->ObjectName = name;
    oa->Attributes = OBJ_OPENIF | OBJ_CASE_INSENSITIVE;
}

static LARGE_INTEGER zero_timeout(void)
{
    LARGE_INTEGER t;
    t.QuadPart = 0;
    return t;
}

static bool t_create_and_query_empty(void)
{
    IO_COMPLETION_BASIC_INFORMATION info;
    HANDLE h;
    NTSTATUS s;

    s = make_port(&h, 0);
    if (!NT_SUCCESS(s)) FAIL_AND_RETURN("create -> 0x%08x", (unsigned)s);
    ASSERT_NOT_NULL(h);

    info.Depth = 0x5A5A5A5A;
    s = NtQueryIoCompletion(h, &info);
    NtClose(h);
    ASSERT_NTSTATUS(s, STATUS_SUCCESS);
    ASSERT_EQ_U32(info.Depth, 0);
    return true;
}

/* The object body is a KQUEUE and the Count argument is its concurrent
 * thread maximum, exactly as KeInitializeQueue takes it -- zero meaning
 * one per processor. */
static bool t_count_is_the_queue_maximum(void)
{
    KQUEUE *q = NULL;
    HANDLE h;
    NTSTATUS s;

    s = make_port(&h, 3);
    if (!NT_SUCCESS(s)) FAIL_AND_RETURN("create -> 0x%08x", (unsigned)s);

    s = ObReferenceObjectByHandle(h, &IoCompletionObjectType, (PVOID *)&q);
    if (!NT_SUCCESS(s)) {
        NtClose(h);
        FAIL_AND_RETURN("reference -> 0x%08x", (unsigned)s);
    }
    ObfDereferenceObject(q);
    NtClose(h);

    ASSERT_EQ_U32(q->Header.Type, QUEUE_OBJECT);
    ASSERT_EQ_U32(q->Header.Size, sizeof(KQUEUE) / sizeof(ULONG));
    ASSERT_EQ_U32(q->MaximumCount, 3);
    ASSERT_EQ_U32(q->CurrentCount, 0);
    return true;
}

static bool t_zero_count_is_one_per_processor(void)
{
    KQUEUE *q = NULL;
    HANDLE h;
    NTSTATUS s;

    s = make_port(&h, 0);
    if (!NT_SUCCESS(s)) FAIL_AND_RETURN("create -> 0x%08x", (unsigned)s);
    s = ObReferenceObjectByHandle(h, &IoCompletionObjectType, (PVOID *)&q);
    if (!NT_SUCCESS(s)) {
        NtClose(h);
        FAIL_AND_RETURN("reference -> 0x%08x", (unsigned)s);
    }
    ObfDereferenceObject(q);
    NtClose(h);
    ASSERT_EQ_U32(q->MaximumCount, 1);
    return true;
}

static bool t_set_then_remove_round_trips(void)
{
    IO_COMPLETION_BASIC_INFORMATION info;
    IO_STATUS_BLOCK iosb;
    PVOID key = NULL, apc = NULL;
    LARGE_INTEGER timeout = zero_timeout();
    HANDLE h;
    NTSTATUS s;

    s = make_port(&h, 0);
    if (!NT_SUCCESS(s)) FAIL_AND_RETURN("create -> 0x%08x", (unsigned)s);

    s = NtSetIoCompletion(h, (PVOID)0x11112222, (PVOID)0x33334444,
                          (NTSTATUS)0xC0000023, 0x55556666);
    if (!NT_SUCCESS(s)) {
        NtClose(h);
        FAIL_AND_RETURN("set -> 0x%08x", (unsigned)s);
    }

    info.Depth = 0x5A5A5A5A;
    s = NtQueryIoCompletion(h, &info);
    if (!NT_SUCCESS(s)) {
        NtClose(h);
        FAIL_AND_RETURN("query -> 0x%08x", (unsigned)s);
    }
    if (info.Depth != 1) {
        NtClose(h);
        FAIL_AND_RETURN("depth after set = %d", (int)info.Depth);
    }

    memset(&iosb, 0, sizeof(iosb));
    s = NtRemoveIoCompletion(h, &key, &apc, &iosb, &timeout);
    if (!NT_SUCCESS(s)) {
        NtClose(h);
        FAIL_AND_RETURN("remove -> 0x%08x", (unsigned)s);
    }

    info.Depth = 0x5A5A5A5A;
    s = NtQueryIoCompletion(h, &info);
    NtClose(h);
    ASSERT_NTSTATUS(s, STATUS_SUCCESS);

    ASSERT_EQ_PTR(key, (PVOID)0x11112222);
    ASSERT_EQ_PTR(apc, (PVOID)0x33334444);
    ASSERT_EQ_U32(iosb.Status, 0xC0000023);
    ASSERT_EQ_U32(iosb.Information, 0x55556666);
    ASSERT_EQ_U32(info.Depth, 0);
    return true;
}

static bool t_remove_from_empty_times_out(void)
{
    IO_STATUS_BLOCK iosb;
    PVOID key = (PVOID)0xDEADBEEF, apc = (PVOID)0xFEEDFACE;
    LARGE_INTEGER timeout = zero_timeout();
    HANDLE h;
    NTSTATUS s;

    s = make_port(&h, 0);
    if (!NT_SUCCESS(s)) FAIL_AND_RETURN("create -> 0x%08x", (unsigned)s);

    memset(&iosb, 0, sizeof(iosb));
    s = NtRemoveIoCompletion(h, &key, &apc, &iosb, &timeout);
    NtClose(h);
    ASSERT_NTSTATUS(s, STATUS_TIMEOUT);
    return true;
}

static bool t_packets_come_back_in_order(void)
{
    IO_STATUS_BLOCK iosb;
    LARGE_INTEGER timeout = zero_timeout();
    HANDLE h;
    NTSTATUS s;
    int i;

    s = make_port(&h, 0);
    if (!NT_SUCCESS(s)) FAIL_AND_RETURN("create -> 0x%08x", (unsigned)s);

    for (i = 0; i < 3; i++) {
        s = NtSetIoCompletion(h, (PVOID)(ULONG_PTR)(0x100 + i),
                              (PVOID)(ULONG_PTR)(0x200 + i),
                              STATUS_SUCCESS, (ULONG_PTR)i);
        if (!NT_SUCCESS(s)) {
            NtClose(h);
            FAIL_AND_RETURN("set %d -> 0x%08x", i, (unsigned)s);
        }
    }

    for (i = 0; i < 3; i++) {
        PVOID key = NULL, apc = NULL;

        memset(&iosb, 0, sizeof(iosb));
        s = NtRemoveIoCompletion(h, &key, &apc, &iosb, &timeout);
        if (!NT_SUCCESS(s)) {
            NtClose(h);
            FAIL_AND_RETURN("remove %d -> 0x%08x", i, (unsigned)s);
        }
        if (key != (PVOID)(ULONG_PTR)(0x100 + i) ||
            apc != (PVOID)(ULONG_PTR)(0x200 + i) ||
            iosb.Information != (ULONG_PTR)i) {
            NtClose(h);
            FAIL_AND_RETURN("packet %d out of order: key=%p apc=%p info=%u",
                            i, key, apc, (unsigned)iosb.Information);
        }
    }
    NtClose(h);
    return true;
}

/* The Io-prefixed setter takes the referenced object rather than a
 * handle, which is how a driver completes into a title's port. */
static bool t_io_setter_takes_the_object(void)
{
    IO_STATUS_BLOCK iosb;
    PVOID key = NULL, apc = NULL, object = NULL;
    LARGE_INTEGER timeout = zero_timeout();
    HANDLE h;
    NTSTATUS s;

    s = make_port(&h, 0);
    if (!NT_SUCCESS(s)) FAIL_AND_RETURN("create -> 0x%08x", (unsigned)s);

    s = ObReferenceObjectByHandle(h, &IoCompletionObjectType, &object);
    if (!NT_SUCCESS(s)) {
        NtClose(h);
        FAIL_AND_RETURN("reference -> 0x%08x", (unsigned)s);
    }

    s = IoSetIoCompletion(object, (PVOID)0x0A0A0A0A, (PVOID)0x0B0B0B0B,
                          STATUS_SUCCESS, 0x0C0C0C0C);
    ObfDereferenceObject(object);
    if (!NT_SUCCESS(s)) {
        NtClose(h);
        FAIL_AND_RETURN("IoSetIoCompletion -> 0x%08x", (unsigned)s);
    }

    memset(&iosb, 0, sizeof(iosb));
    s = NtRemoveIoCompletion(h, &key, &apc, &iosb, &timeout);
    NtClose(h);
    if (!NT_SUCCESS(s)) FAIL_AND_RETURN("remove -> 0x%08x", (unsigned)s);
    ASSERT_EQ_PTR(key, (PVOID)0x0A0A0A0A);
    ASSERT_EQ_PTR(apc, (PVOID)0x0B0B0B0B);
    ASSERT_EQ_U32(iosb.Information, 0x0C0C0C0C);
    return true;
}

/* PoolTag is the last field of the 28-byte OBJECT_TYPE and the only one
 * titles read; a wrong-size DATA scaffold shows up as garbage here. */
static bool t_object_type_tag(void)
{
    ASSERT_EQ_U32(IoCompletionObjectType.PoolTag, 'pmoC');
    return true;
}

/* The creator takes an access mask, but nothing consults it: a port
 * created with none at all still accepts every operation. */
static bool t_access_mask_is_not_enforced(void)
{
    IO_COMPLETION_BASIC_INFORMATION info;
    HANDLE h = NULL;
    NTSTATUS s;

    s = NtCreateIoCompletion(&h, 0, NULL, 0);
    if (!NT_SUCCESS(s)) FAIL_AND_RETURN("create -> 0x%08x", (unsigned)s);

    s = NtSetIoCompletion(h, NULL, NULL, STATUS_SUCCESS, 0);
    if (!NT_SUCCESS(s)) {
        NtClose(h);
        FAIL_AND_RETURN("set -> 0x%08x", (unsigned)s);
    }
    info.Depth = 0x5A5A5A5A;
    s = NtQueryIoCompletion(h, &info);
    NtClose(h);
    ASSERT_NTSTATUS(s, STATUS_SUCCESS);
    ASSERT_EQ_U32(info.Depth, 1);
    return true;
}

/* Two creates of one name through the XAPI pseudo root resolve to a
 * single port, which is what makes the object attributes load-bearing. */
static bool t_named_port_is_shared(void)
{
    IO_COMPLETION_BASIC_INFORMATION info;
    OBJECT_ATTRIBUTES oa1, oa2;
    ANSI_STRING n1, n2;
    HANDLE h1 = NULL, h2 = NULL;
    NTSTATUS s;

    init_named_oa(&oa1, &n1, "apireg_iocomp");
    s = NtCreateIoCompletion(&h1, IO_COMPLETION_ALL_ACCESS, &oa1, 0);
    if (!NT_SUCCESS(s)) FAIL_AND_RETURN("first create -> 0x%08x", (unsigned)s);

    init_named_oa(&oa2, &n2, "apireg_iocomp");
    s = NtCreateIoCompletion(&h2, IO_COMPLETION_ALL_ACCESS, &oa2, 0);
    if (!NT_SUCCESS(s)) {
        NtClose(h1);
        FAIL_AND_RETURN("second create -> 0x%08x", (unsigned)s);
    }

    /* One object: a packet set through h1 is visible through h2. */
    s = NtSetIoCompletion(h1, NULL, NULL, STATUS_SUCCESS, 0);
    if (!NT_SUCCESS(s)) {
        NtClose(h2);
        NtClose(h1);
        FAIL_AND_RETURN("set -> 0x%08x", (unsigned)s);
    }
    info.Depth = 0x5A5A5A5A;
    s = NtQueryIoCompletion(h2, &info);
    NtClose(h2);
    NtClose(h1);
    ASSERT_NTSTATUS(s, STATUS_SUCCESS);
    ASSERT_EQ_U32(info.Depth, 1);
    return true;
}

/* Every entry point type-checks the handle. */
static bool t_wrong_object_type_is_refused(void)
{
    IO_COMPLETION_BASIC_INFORMATION info;
    HANDLE h = NULL;
    NTSTATUS s, q;

    s = NtCreateEvent(&h, NULL, NotificationEvent, FALSE);
    if (!NT_SUCCESS(s)) FAIL_AND_RETURN("create event -> 0x%08x", (unsigned)s);

    s = NtSetIoCompletion(h, NULL, NULL, STATUS_SUCCESS, 0);
    q = NtQueryIoCompletion(h, &info);
    NtClose(h);
    ASSERT_NTSTATUS(s, STATUS_OBJECT_TYPE_MISMATCH);
    ASSERT_NTSTATUS(q, STATUS_OBJECT_TYPE_MISMATCH);
    return true;
}

static const test_entry_t io_iocomp_entries[] = {
    { "create_and_query_empty",       t_create_and_query_empty,       NULL },
    { "count_is_the_queue_maximum",   t_count_is_the_queue_maximum,   NULL },
    { "zero_count_is_one_per_processor",
      t_zero_count_is_one_per_processor, NULL },
    { "set_then_remove_round_trips",  t_set_then_remove_round_trips,  NULL },
    { "remove_from_empty_times_out",  t_remove_from_empty_times_out,  NULL },
    { "packets_come_back_in_order",   t_packets_come_back_in_order,   NULL },
    { "io_setter_takes_the_object",   t_io_setter_takes_the_object,   NULL },
    { "object_type_tag",              t_object_type_tag,              NULL },
    { "access_mask_is_not_enforced",  t_access_mask_is_not_enforced,  NULL },
    { "named_port_is_shared",         t_named_port_is_shared,         NULL },
    { "wrong_object_type_is_refused", t_wrong_object_type_is_refused, NULL },
};

DEFINE_GROUP(io_iocomp, "io/iocomp");
