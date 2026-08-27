/*
 * ObReferenceObjectByName: resolve a kernel object-namespace path to a
 * referenced pointer without going through a handle.
 *
 * \Device is used as the target because a directory object has no parse
 * routine -- the lookup ends on the object itself, so the call is a pure
 * namespace walk with no IO packet involved.
 */

#include "../harness.h"
#include <string.h>

#ifndef STATUS_OBJECT_TYPE_MISMATCH
#define STATUS_OBJECT_TYPE_MISMATCH ((NTSTATUS)0xC0000024L)
#endif

static const char DEVICE_DIR[] = "\\Device";
static const char MISSING_DIR[] = "\\Device\\nxkrnl-apireg-absent";

static void init_name(OBJECT_STRING *s, const char *str, USHORT len)
{
    s->Length        = len;
    s->MaximumLength = (USHORT)(len + 1);
    s->Buffer        = (PCHAR)str;
}

static NTSTATUS ref_device_dir(POBJECT_TYPE type, PVOID *object)
{
    OBJECT_STRING name;

    init_name(&name, DEVICE_DIR, sizeof(DEVICE_DIR) - 1);
    *object = NULL;
    return ObReferenceObjectByName(&name, OBJ_CASE_INSENSITIVE, type, NULL,
                                   object);
}

static bool t_directory_by_name(void)
{
    PVOID object;
    NTSTATUS s = ref_device_dir(&ObDirectoryObjectType, &object);

    ASSERT_NTSTATUS(s, STATUS_SUCCESS);
    ASSERT_NOT_NULL(object);
    ObfDereferenceObject(object);
    return true;
}

static bool t_null_type_accepts_any(void)
{
    PVOID typed, untyped;
    NTSTATUS s = ref_device_dir(&ObDirectoryObjectType, &typed);
    ASSERT_NTSTATUS(s, STATUS_SUCCESS);

    /* A NULL ObjectType means "whatever is there"; the namespace holds one
     * \Device, so both lookups must land on the same object. */
    s = ref_device_dir(NULL, &untyped);
    if (!NT_SUCCESS(s)) {
        ObfDereferenceObject(typed);
        FAIL_AND_RETURN("untyped lookup -> 0x%08x", (unsigned)s);
    }
    bool same = (typed == untyped);
    ObfDereferenceObject(untyped);
    ObfDereferenceObject(typed);
    if (!same)
        FAIL_AND_RETURN("typed %p != untyped %p", typed, untyped);
    return true;
}

static bool t_type_mismatch(void)
{
    PVOID object;
    NTSTATUS s = ref_device_dir(&IoFileObjectType, &object);

    if (NT_SUCCESS(s)) {
        ObfDereferenceObject(object);
        FAIL_AND_RETURN("wrong object type succeeded");
    }
    ASSERT_NTSTATUS(s, STATUS_OBJECT_TYPE_MISMATCH);
    return true;
}

static bool t_missing_name(void)
{
    OBJECT_STRING name;
    PVOID object = (PVOID)(ULONG_PTR)0xdeadbeef;
    NTSTATUS s;

    init_name(&name, MISSING_DIR, sizeof(MISSING_DIR) - 1);
    s = ObReferenceObjectByName(&name, OBJ_CASE_INSENSITIVE,
                                &ObDirectoryObjectType, NULL, &object);
    if (NT_SUCCESS(s)) {
        ObfDereferenceObject(object);
        FAIL_AND_RETURN("absent name resolved");
    }
    ASSERT_NTSTATUS(s, STATUS_OBJECT_NAME_NOT_FOUND);
    return true;
}

static bool t_reference_is_counted(void)
{
    PVOID a, b;
    NTSTATUS s = ref_device_dir(&ObDirectoryObjectType, &a);
    ASSERT_NTSTATUS(s, STATUS_SUCCESS);
    s = ref_device_dir(&ObDirectoryObjectType, &b);
    if (!NT_SUCCESS(s)) {
        ObfDereferenceObject(a);
        FAIL_AND_RETURN("second reference -> 0x%08x", (unsigned)s);
    }
    ASSERT_EQ_PTR(b, a);

    /* Dropping one of two references must leave the object usable. */
    ObfDereferenceObject(b);
    PVOID c;
    s = ref_device_dir(&ObDirectoryObjectType, &c);
    ObfDereferenceObject(a);
    if (!NT_SUCCESS(s))
        FAIL_AND_RETURN("reference after partial release -> 0x%08x",
                        (unsigned)s);
    ASSERT_EQ_PTR(c, a);
    ObfDereferenceObject(c);
    return true;
}

static const test_entry_t ob_refname_entries[] = {
    {"directory_by_name",    t_directory_by_name},
    {"null_type_accepts_any",t_null_type_accepts_any},
    {"type_mismatch",        t_type_mismatch},
    {"missing_name",         t_missing_name},
    {"reference_is_counted", t_reference_is_counted},
};

DEFINE_GROUP(ob_refname, "ob/refname");
