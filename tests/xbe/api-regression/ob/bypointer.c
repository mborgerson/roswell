/*
 * The object-manager entries that take the object itself rather than a
 * handle or a name.
 *
 * There is no user mode here, so the type argument is the caller's only
 * guard against being handed the wrong kind of object: the routine
 * enforces it and refuses a mismatch with STATUS_OBJECT_TYPE_MISMATCH.
 * The reference it takes is the caller's own -- it outlives the handle
 * the object was reached through.
 */

#include "../harness.h"

#ifndef STATUS_OBJECT_TYPE_MISMATCH
#define STATUS_OBJECT_TYPE_MISMATCH ((NTSTATUS)0xC0000024L)
#endif

/* An event and the object behind it.  The handle holds the reference
 * the caller needs, so the pointer reference taken here goes straight
 * back. */
static NTSTATUS open_event(HANDLE *h, PVOID *obj)
{
    NTSTATUS s;

    *h = NULL;
    *obj = NULL;
    s = NtCreateEvent(h, NULL, NotificationEvent, FALSE);
    if (!NT_SUCCESS(s))
        return s;

    s = ObReferenceObjectByHandle(*h, &ExEventObjectType, obj);
    if (!NT_SUCCESS(s)) {
        NtClose(*h);
        *h = NULL;
        return s;
    }
    ObfDereferenceObject(*obj);
    return STATUS_SUCCESS;
}

static bool t_the_object_s_own_type_is_accepted(void)
{
    HANDLE h;
    PVOID obj;
    NTSTATUS s;

    s = open_event(&h, &obj);
    if (!NT_SUCCESS(s)) FAIL_AND_RETURN("open -> 0x%08x", (unsigned)s);

    s = ObReferenceObjectByPointer(obj, &ExEventObjectType);
    if (NT_SUCCESS(s))
        ObfDereferenceObject(obj);
    NtClose(h);

    ASSERT_NTSTATUS(s, STATUS_SUCCESS);
    return true;
}

static bool t_another_type_is_refused(void)
{
    HANDLE h;
    PVOID obj;
    NTSTATUS s;

    s = open_event(&h, &obj);
    if (!NT_SUCCESS(s)) FAIL_AND_RETURN("open -> 0x%08x", (unsigned)s);

    s = ObReferenceObjectByPointer(obj, &ExSemaphoreObjectType);
    if (NT_SUCCESS(s))
        ObfDereferenceObject(obj);
    NtClose(h);

    ASSERT_NTSTATUS(s, STATUS_OBJECT_TYPE_MISMATCH);
    return true;
}

/* The reference stands on its own: the object survives the handle it
 * was reached through, and is still a working event afterwards. */
static bool t_the_reference_outlives_the_handle(void)
{
    HANDLE h;
    PVOID obj;
    NTSTATUS s;
    LONG was_set, was_reset;

    s = open_event(&h, &obj);
    if (!NT_SUCCESS(s)) FAIL_AND_RETURN("open -> 0x%08x", (unsigned)s);

    s = ObReferenceObjectByPointer(obj, &ExEventObjectType);
    if (!NT_SUCCESS(s)) {
        NtClose(h);
        FAIL_AND_RETURN("reference -> 0x%08x", (unsigned)s);
    }

    NtClose(h);
    was_set = KeSetEvent((PRKEVENT)obj, 0, FALSE);
    was_reset = KeResetEvent((PRKEVENT)obj);
    ObfDereferenceObject(obj);

    ASSERT_EQ_U32(was_set, 0);
    ASSERT_EQ_U32(was_reset, 1);
    return true;
}

static const test_entry_t ob_bypointer_entries[] = {
    { "the_object_s_own_type_is_accepted",
      t_the_object_s_own_type_is_accepted, NULL },
    { "another_type_is_refused", t_another_type_is_refused, NULL },
    { "the_reference_outlives_the_handle",
      t_the_reference_outlives_the_handle, NULL },
};

DEFINE_GROUP(ob_bypointer, "ob/bypointer");
