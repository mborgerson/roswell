/*
 * OBJECT_TYPE DATA-ordinal exports. Same failure mode as hal/data --
 * a wrong-size stub corrupts reads at offsets >= 4. Each OBJECT_TYPE is
 * 7 pointers (28 bytes); we sanity-check that PoolTag (the last field)
 * is printable ASCII, which it always is for a real kernel-side init.
 */

#include "../harness.h"

static bool tag_is_printable(ULONG tag)
{
    for (int i = 0; i < 4; i++) {
        UCHAR c = (UCHAR)((tag >> (i * 8)) & 0xff);
        if (c < 0x20 || c > 0x7e) return false;
    }
    return true;
}

static bool check_type(const char *name, OBJECT_TYPE *t)
{
    /* PoolTag is the last field of OBJECT_TYPE (offset +0x18) and must be
     * a printable 4-byte ASCII tag. If a DATA-ordinal stub is sized
     * incorrectly the title reads garbage from this offset; that's the
     * failure mode we're guarding against. */
    if (t->PoolTag == 0) {
        test_record_failure(__FILE__, __LINE__, "%s PoolTag is zero", name);
        return false;
    }
    if (!tag_is_printable(t->PoolTag)) {
        test_record_failure(__FILE__, __LINE__,
            "%s PoolTag=0x%08x not printable", name, (unsigned)t->PoolTag);
        return false;
    }
    return true;
}

static bool t_thread(void)    { return check_type("PsThreadObjectType",   &PsThreadObjectType); }
static bool t_event(void)     { return check_type("ExEventObjectType",    &ExEventObjectType); }
static bool t_semaphore(void) { return check_type("ExSemaphoreObjectType",&ExSemaphoreObjectType); }
static bool t_mutant(void)    { return check_type("ExMutantObjectType",   &ExMutantObjectType); }
static bool t_timer(void)     { return check_type("ExTimerObjectType",    &ExTimerObjectType); }
static bool t_file(void)      { return check_type("IoFileObjectType",     &IoFileObjectType); }
static bool t_device(void)    { return check_type("IoDeviceObjectType",   &IoDeviceObjectType); }
static bool t_symlink(void)   { return check_type("ObSymbolicLinkObjectType",
                                                  &ObSymbolicLinkObjectType); }

/* The type is also the guard on a reference: a symbolic-link handle
 * answers to its own type and to nothing else. */
static bool t_symlink_type_guards_a_reference(void)
{
    static const char LINK[] = "\\??\\ObTypeLink";
    static const char TARGET[] = "\\Device\\CdRom0";
    OBJECT_STRING name, target;
    HANDLE h = NULL;
    NTSTATUS s;
    PVOID object = NULL;

    name.Buffer = (PCHAR)LINK;
    name.Length = (USHORT)(sizeof(LINK) - 1);
    name.MaximumLength = (USHORT)sizeof(LINK);
    target.Buffer = (PCHAR)TARGET;
    target.Length = (USHORT)(sizeof(TARGET) - 1);
    target.MaximumLength = (USHORT)sizeof(TARGET);
    s = IoCreateSymbolicLink(&name, &target);
    if (!NT_SUCCESS(s))
        FAIL_AND_RETURN("IoCreateSymbolicLink: 0x%08x", (unsigned)s);

    {
        OBJECT_ATTRIBUTES oa = {
            .RootDirectory = NULL,
            .ObjectName    = &name,
            .Attributes    = OBJ_CASE_INSENSITIVE,
        };
        s = NtOpenSymbolicLinkObject(&h, &oa);
    }
    if (!NT_SUCCESS(s)) {
        IoDeleteSymbolicLink(&name);
        FAIL_AND_RETURN("NtOpenSymbolicLinkObject: 0x%08x", (unsigned)s);
    }

    s = ObReferenceObjectByHandle(h, &ObSymbolicLinkObjectType, &object);
    if (NT_SUCCESS(s))
        ObfDereferenceObject(object);
    else
        test_record_failure(__FILE__, __LINE__,
                            "reference by its own type: 0x%08x", (unsigned)s);

    if (NT_SUCCESS(s)) {
        PVOID wrong = NULL;
        NTSTATUS w = ObReferenceObjectByHandle(h, &IoFileObjectType, &wrong);
        if (NT_SUCCESS(w)) {
            ObfDereferenceObject(wrong);
            test_record_failure(__FILE__, __LINE__,
                                "the file type referenced a symbolic link");
            s = STATUS_UNSUCCESSFUL;
        }
    }

    NtClose(h);
    IoDeleteSymbolicLink(&name);
    return NT_SUCCESS(s);
}

static const test_entry_t ob_types_entries[] = {
    {"thread",    t_thread},
    {"event",     t_event},
    {"semaphore", t_semaphore},
    {"mutant",    t_mutant},
    {"timer",     t_timer},
    {"file",      t_file},
    {"device",    t_device},
    {"symlink",   t_symlink},
    {"symlink_type_guards_a_reference", t_symlink_type_guards_a_reference},
};

DEFINE_GROUP(ob_types, "ob/types");
