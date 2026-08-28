/*
 * ObCreateObject / ObInsertObject -- a title's own object type.
 *
 * These two are the only place where the caller, not the kernel, owns
 * the storage an object lives in: the type structure carries an
 * allocate and a free procedure, and the object manager calls them.
 * So the type used here does its own pool call through a counting
 * wrapper, which makes the header size, the pool tag, and the moment
 * the object goes away all directly observable.
 *
 * Creating and inserting are separate steps.  The create hands back a
 * body pointer with one reference on it and nothing else; the insert
 * puts it in the handle table -- and, when the attributes name it, in
 * the namespace -- and gives up the create's reference on the way out.
 * A refused insert therefore destroys the object.
 */

#include "../harness.h"
#include <string.h>

#ifndef STATUS_OBJECT_TYPE_MISMATCH
#define STATUS_OBJECT_TYPE_MISMATCH ((NTSTATUS)0xC0000024L)
#endif
#ifndef STATUS_OBJECT_NAME_COLLISION
#define STATUS_OBJECT_NAME_COLLISION ((NTSTATUS)0xC0000035L)
#endif
#ifndef STATUS_OBJECT_NAME_EXISTS
#define STATUS_OBJECT_NAME_EXISTS ((NTSTATUS)0x40000000L)
#endif
#ifndef STATUS_OBJECT_NAME_INVALID
#define STATUS_OBJECT_NAME_INVALID ((NTSTATUS)0xC0000033L)
#endif

#define OB_FLAG_NAMED_OBJECT     0x01
#define OB_FLAG_PERMANENT_OBJECT 0x02
#define OB_FLAG_ATTACHED_OBJECT  0x04

#define TEST_POOL_TAG  'tSbO'
#define TEST_BODY_SIZE 64
#define TEST_NAME      "\\apiregobj"
#define TEST_LEAF_LEN  (sizeof(TEST_NAME) - 2)  /* without the separator */

/* What the type's procedures saw, most recent call last. */
static struct {
    ULONG allocs;
    SIZE_T alloc_size;
    ULONG alloc_tag;
    ULONG frees;
    PVOID free_base;
    ULONG deletes;
    PVOID delete_object;
} g_seen;

static PVOID NTAPI probe_allocate(SIZE_T Size, ULONG Tag)
{
    g_seen.allocs++;
    g_seen.alloc_size = Size;
    g_seen.alloc_tag = Tag;
    return ExAllocatePoolWithTag(Size, Tag);
}

static VOID NTAPI probe_free(PVOID Base)
{
    g_seen.frees++;
    g_seen.free_base = Base;
    ExFreePool(Base);
}

static VOID NTAPI probe_delete(PVOID Object)
{
    g_seen.deletes++;
    g_seen.delete_object = Object;
}

static OBJECT_TYPE g_probe_type = {
    probe_allocate,
    probe_free,
    NULL,               /* CloseProcedure */
    probe_delete,
    NULL,               /* ParseProcedure */
    NULL,               /* DefaultObject */
    TEST_POOL_TAG,
};

static void init_str(OBJECT_STRING *s, const char *str)
{
    s->Buffer = (PCHAR)str;
    s->Length = (USHORT)strlen(str);
    s->MaximumLength = s->Length;
}

static void init_oa(OBJECT_ATTRIBUTES *oa, OBJECT_STRING *name, ULONG attrs)
{
    oa->RootDirectory = NULL;
    oa->ObjectName = name;
    oa->Attributes = attrs;
}

static NTSTATUS create_unnamed(PVOID *obj)
{
    memset(&g_seen, 0, sizeof(g_seen));
    *obj = NULL;
    return ObCreateObject(&g_probe_type, NULL, TEST_BODY_SIZE, obj);
}

static NTSTATUS create_named(PVOID *obj, OBJECT_STRING *name)
{
    OBJECT_ATTRIBUTES oa;

    init_str(name, TEST_NAME);
    init_oa(&oa, name, 0);
    memset(&g_seen, 0, sizeof(g_seen));
    *obj = NULL;
    return ObCreateObject(&g_probe_type, &oa, TEST_BODY_SIZE, obj);
}

static bool t_an_unnamed_object_comes_back_with_its_header(void)
{
    POBJECT_HEADER header;
    PVOID obj;
    NTSTATUS s;

    s = create_unnamed(&obj);
    ASSERT_NTSTATUS(s, STATUS_SUCCESS);
    ASSERT_NOT_NULL(obj);

    header = OBJECT_TO_OBJECT_HEADER(obj);
    ASSERT_EQ_U32(header->PointerCount, 1);
    ASSERT_EQ_U32(header->HandleCount, 0);
    ASSERT_EQ_PTR(header->Type, &g_probe_type);
    ASSERT_EQ_U32(header->Flags, 0);

    /* The body is the caller's; writing it must not disturb anything. */
    memset(obj, 0xA5, TEST_BODY_SIZE);
    ASSERT_EQ_U32(header->PointerCount, 1);

    ObfDereferenceObject(obj);
    return true;
}

/* The allocation is one call, for the header plus the body, under the
 * type's own tag. */
static bool t_the_allocate_procedure_sizes_the_header(void)
{
    PVOID obj;
    NTSTATUS s;

    s = create_unnamed(&obj);
    ASSERT_NTSTATUS(s, STATUS_SUCCESS);

    ASSERT_EQ_U32(g_seen.allocs, 1);
    ASSERT_EQ_U32(g_seen.alloc_size, 16 + TEST_BODY_SIZE);
    ASSERT_EQ_U32(g_seen.alloc_tag, TEST_POOL_TAG);

    ObfDereferenceObject(obj);
    return true;
}

/* The last reference runs the delete procedure on the body and then
 * hands the allocation base back to the free procedure. */
static bool t_the_procedures_run_on_the_last_dereference(void)
{
    PVOID obj;
    NTSTATUS s;

    s = create_unnamed(&obj);
    ASSERT_NTSTATUS(s, STATUS_SUCCESS);

    ObfReferenceObject(obj);
    ObfDereferenceObject(obj);
    ASSERT_EQ_U32(g_seen.deletes, 0);
    ASSERT_EQ_U32(g_seen.frees, 0);

    ObfDereferenceObject(obj);
    ASSERT_EQ_U32(g_seen.deletes, 1);
    ASSERT_EQ_PTR(g_seen.delete_object, obj);
    ASSERT_EQ_U32(g_seen.frees, 1);
    ASSERT_EQ_PTR(g_seen.free_base, OBJECT_TO_OBJECT_HEADER(obj));
    return true;
}

/* A named create reserves the name ahead of the header and copies the
 * leaf in, so the allocation grows by both. */
static bool t_a_named_object_reserves_room_for_its_name(void)
{
    OBJECT_STRING name;
    PVOID obj;
    NTSTATUS s;

    s = create_named(&obj, &name);
    ASSERT_NTSTATUS(s, STATUS_SUCCESS);
    ASSERT_NOT_NULL(obj);

    ASSERT_EQ_U32(g_seen.allocs, 1);
    ASSERT_EQ_U32(g_seen.alloc_size,
                  16 + 16 + TEST_BODY_SIZE + TEST_LEAF_LEN);
    ASSERT_EQ_U32(OBJECT_TO_OBJECT_HEADER(obj)->Flags, OB_FLAG_NAMED_OBJECT);

    ObfDereferenceObject(obj);
    ASSERT_EQ_U32(g_seen.frees, 1);
    ASSERT_EQ_PTR(g_seen.free_base,
                  (PUCHAR)OBJECT_TO_OBJECT_HEADER(obj) - 16);
    return true;
}

/* The object's name is the last element of the path.  A name with no
 * element in it at all names nothing, and is refused before anything is
 * allocated. */
static bool t_a_name_with_no_element_is_refused(void)
{
    static const char *const EMPTY[] = { "", "\\", "\\\\" };
    OBJECT_ATTRIBUTES oa;
    OBJECT_STRING name;
    PVOID obj = NULL;
    NTSTATUS s;
    size_t i;

    memset(&g_seen, 0, sizeof(g_seen));
    for (i = 0; i < sizeof(EMPTY)/sizeof(EMPTY[0]); i++) {
        init_str(&name, EMPTY[i]);
        init_oa(&oa, &name, 0);
        s = ObCreateObject(&g_probe_type, &oa, TEST_BODY_SIZE, &obj);
        if (NT_SUCCESS(s)) {
            ObfDereferenceObject(obj);
            FAIL_AND_RETURN("name %u of %u was accepted",
                            (unsigned)i, (unsigned)(sizeof(EMPTY)/sizeof(EMPTY[0])));
        }
        ASSERT_NTSTATUS(s, STATUS_OBJECT_NAME_INVALID);
    }
    ASSERT_EQ_U32(g_seen.allocs, 0);
    return true;
}

/* The path in front of the last element is the insert's business, not
 * the create's: only the leaf is copied.  A separator at either end is
 * absorbed. */
static bool t_only_the_last_element_of_the_name_is_kept(void)
{
    static const struct { const char *name; ULONG leaf; } CASES[] = {
        { "\\apireg\\sub", 3 },   /* the leaf of a path */
        { "\\apireg\\",    6 },   /* a trailing separator */
        { "\\\\apireg",    6 },   /* a doubled leading separator */
    };
    OBJECT_ATTRIBUTES oa;
    OBJECT_STRING name;
    PVOID obj = NULL;
    NTSTATUS s;
    size_t i;

    for (i = 0; i < sizeof(CASES)/sizeof(CASES[0]); i++) {
        init_str(&name, CASES[i].name);
        init_oa(&oa, &name, 0);
        memset(&g_seen, 0, sizeof(g_seen));
        s = ObCreateObject(&g_probe_type, &oa, TEST_BODY_SIZE, &obj);
        if (!NT_SUCCESS(s))
            FAIL_AND_RETURN("'%s' -> 0x%08x", CASES[i].name, (unsigned)s);
        if (g_seen.alloc_size != 16 + 16 + TEST_BODY_SIZE + CASES[i].leaf) {
            ObfDereferenceObject(obj);
            FAIL_AND_RETURN("'%s': allocated %u, leaf %u expected",
                            CASES[i].name, (unsigned)g_seen.alloc_size,
                            (unsigned)CASES[i].leaf);
        }
        ObfDereferenceObject(obj);
    }
    return true;
}

/* An empty element inside the path is not absorbed the way one at
 * either end is -- it makes the whole name invalid. */
static bool t_an_empty_element_inside_the_path_is_refused(void)
{
    OBJECT_ATTRIBUTES oa;
    OBJECT_STRING name;
    PVOID obj = NULL;
    NTSTATUS s;

    init_str(&name, "\\\\apireg\\\\");
    init_oa(&oa, &name, 0);
    memset(&g_seen, 0, sizeof(g_seen));
    s = ObCreateObject(&g_probe_type, &oa, TEST_BODY_SIZE, &obj);
    if (NT_SUCCESS(s)) {
        ObfDereferenceObject(obj);
        FAIL_AND_RETURN("accepted");
    }
    ASSERT_NTSTATUS(s, STATUS_OBJECT_NAME_INVALID);
    ASSERT_EQ_U32(g_seen.allocs, 0);
    return true;
}

/* Inserting takes the create's reference and leaves a handle holding
 * the object. */
static bool t_an_unnamed_insert_hands_back_a_handle(void)
{
    POBJECT_HEADER header;
    PVOID obj, reached = NULL;
    HANDLE h = NULL;
    NTSTATUS s;

    s = create_unnamed(&obj);
    if (!NT_SUCCESS(s)) FAIL_AND_RETURN("create -> 0x%08x", (unsigned)s);
    header = OBJECT_TO_OBJECT_HEADER(obj);

    s = ObInsertObject(obj, NULL, 0, &h);
    ASSERT_NTSTATUS(s, STATUS_SUCCESS);
    ASSERT_NOT_NULL(h);
    ASSERT_EQ_U32(header->PointerCount, 1);
    ASSERT_EQ_U32(header->HandleCount, 1);

    s = ObReferenceObjectByHandle(h, &g_probe_type, &reached);
    ASSERT_NTSTATUS(s, STATUS_SUCCESS);
    ASSERT_EQ_PTR(reached, obj);
    ObfDereferenceObject(reached);

    ASSERT_NTSTATUS(NtClose(h), STATUS_SUCCESS);
    ASSERT_EQ_U32(g_seen.frees, 1);
    return true;
}

/* The bias is an extra reference the handle does not own. */
static bool t_the_pointer_bias_outlives_the_handle(void)
{
    PVOID obj;
    HANDLE h = NULL;
    NTSTATUS s;

    s = create_unnamed(&obj);
    if (!NT_SUCCESS(s)) FAIL_AND_RETURN("create -> 0x%08x", (unsigned)s);

    s = ObInsertObject(obj, NULL, 1, &h);
    ASSERT_NTSTATUS(s, STATUS_SUCCESS);
    ASSERT_EQ_U32(OBJECT_TO_OBJECT_HEADER(obj)->PointerCount, 2);

    ASSERT_NTSTATUS(NtClose(h), STATUS_SUCCESS);
    ASSERT_EQ_U32(g_seen.frees, 0);
    ASSERT_EQ_U32(OBJECT_TO_OBJECT_HEADER(obj)->PointerCount, 1);

    ObfDereferenceObject(obj);
    ASSERT_EQ_U32(g_seen.frees, 1);
    return true;
}

/* A named insert puts the object in the namespace, where a by-name
 * reference finds it, and takes it back out with the last handle. */
static bool t_a_named_object_is_reachable_by_its_name(void)
{
    OBJECT_ATTRIBUTES oa;
    OBJECT_STRING name;
    PVOID obj, reached = NULL;
    HANDLE h = NULL;
    NTSTATUS s;

    s = create_named(&obj, &name);
    if (!NT_SUCCESS(s)) FAIL_AND_RETURN("create -> 0x%08x", (unsigned)s);

    init_oa(&oa, &name, 0);
    s = ObInsertObject(obj, &oa, 0, &h);
    ASSERT_NTSTATUS(s, STATUS_SUCCESS);
    ASSERT_NOT_NULL(h);
    ASSERT_EQ_U32(OBJECT_TO_OBJECT_HEADER(obj)->Flags,
                  OB_FLAG_NAMED_OBJECT | OB_FLAG_ATTACHED_OBJECT);

    s = ObReferenceObjectByName(&name, 0, &g_probe_type, NULL, &reached);
    ASSERT_NTSTATUS(s, STATUS_SUCCESS);
    ASSERT_EQ_PTR(reached, obj);
    ObfDereferenceObject(reached);

    ASSERT_NTSTATUS(NtClose(h), STATUS_SUCCESS);
    ASSERT_EQ_U32(g_seen.frees, 1);

    reached = NULL;
    s = ObReferenceObjectByName(&name, 0, &g_probe_type, NULL, &reached);
    if (NT_SUCCESS(s)) {
        ObfDereferenceObject(reached);
        FAIL_AND_RETURN("the name outlived the object");
    }
    return true;
}

/* A second object under one name is refused, and the insert destroys
 * the object it refused. */
static bool t_a_second_insert_of_the_name_collides(void)
{
    OBJECT_ATTRIBUTES oa;
    OBJECT_STRING name;
    PVOID first, second;
    HANDLE h = NULL, again = (HANDLE)(LONG_PTR)-1;
    NTSTATUS s;

    s = create_named(&first, &name);
    if (!NT_SUCCESS(s)) FAIL_AND_RETURN("create -> 0x%08x", (unsigned)s);
    init_oa(&oa, &name, 0);
    s = ObInsertObject(first, &oa, 0, &h);
    if (!NT_SUCCESS(s)) FAIL_AND_RETURN("insert -> 0x%08x", (unsigned)s);

    s = create_named(&second, &name);
    if (!NT_SUCCESS(s)) {
        NtClose(h);
        FAIL_AND_RETURN("second create -> 0x%08x", (unsigned)s);
    }
    init_oa(&oa, &name, 0);
    s = ObInsertObject(second, &oa, 0, &again);
    if (NT_SUCCESS(s) && again != NULL)
        NtClose(again);
    NtClose(h);

    ASSERT_NTSTATUS(s, STATUS_OBJECT_NAME_COLLISION);
    ASSERT_EQ_PTR(again, NULL);
    return true;
}

/* With OBJ_OPENIF the insert opens what is already there instead, and
 * the object it was given goes away. */
static bool t_openif_returns_the_object_already_there(void)
{
    OBJECT_ATTRIBUTES oa;
    OBJECT_STRING name;
    PVOID first, second, reached = NULL;
    HANDLE h = NULL, again = NULL;
    ULONG frees_before;
    NTSTATUS s;

    s = create_named(&first, &name);
    if (!NT_SUCCESS(s)) FAIL_AND_RETURN("create -> 0x%08x", (unsigned)s);
    init_oa(&oa, &name, 0);
    s = ObInsertObject(first, &oa, 0, &h);
    if (!NT_SUCCESS(s)) FAIL_AND_RETURN("insert -> 0x%08x", (unsigned)s);

    s = create_named(&second, &name);
    if (!NT_SUCCESS(s)) {
        NtClose(h);
        FAIL_AND_RETURN("second create -> 0x%08x", (unsigned)s);
    }
    frees_before = g_seen.frees;
    init_oa(&oa, &name, OBJ_OPENIF);
    s = ObInsertObject(second, &oa, 0, &again);
    if (!NT_SUCCESS(s)) {
        NtClose(h);
        FAIL_AND_RETURN("openif insert -> 0x%08x", (unsigned)s);
    }
    ASSERT_NTSTATUS(s, STATUS_OBJECT_NAME_EXISTS);
    ASSERT_EQ_U32(g_seen.frees, frees_before + 1);

    s = ObReferenceObjectByHandle(again, &g_probe_type, &reached);
    if (NT_SUCCESS(s))
        ObfDereferenceObject(reached);
    NtClose(again);
    NtClose(h);

    ASSERT_NTSTATUS(s, STATUS_SUCCESS);
    ASSERT_EQ_PTR(reached, first);
    return true;
}

/* The type is what a handle is checked against, here as everywhere. */
static bool t_a_handle_of_another_type_is_refused(void)
{
    PVOID obj, reached = (PVOID)(LONG_PTR)-1;
    HANDLE h = NULL;
    NTSTATUS s;

    s = create_unnamed(&obj);
    if (!NT_SUCCESS(s)) FAIL_AND_RETURN("create -> 0x%08x", (unsigned)s);
    s = ObInsertObject(obj, NULL, 0, &h);
    if (!NT_SUCCESS(s)) FAIL_AND_RETURN("insert -> 0x%08x", (unsigned)s);

    s = ObReferenceObjectByHandle(h, &ExEventObjectType, &reached);
    if (NT_SUCCESS(s))
        ObfDereferenceObject(reached);
    NtClose(h);

    ASSERT_NTSTATUS(s, STATUS_OBJECT_TYPE_MISMATCH);
    return true;
}

static const test_entry_t ob_create_entries[] = {
    { "an_unnamed_object_comes_back_with_its_header",
      t_an_unnamed_object_comes_back_with_its_header, NULL },
    { "the_allocate_procedure_sizes_the_header",
      t_the_allocate_procedure_sizes_the_header, NULL },
    { "the_procedures_run_on_the_last_dereference",
      t_the_procedures_run_on_the_last_dereference, NULL },
    { "a_named_object_reserves_room_for_its_name",
      t_a_named_object_reserves_room_for_its_name, NULL },
    { "a_name_with_no_element_is_refused",
      t_a_name_with_no_element_is_refused, NULL },
    { "only_the_last_element_of_the_name_is_kept",
      t_only_the_last_element_of_the_name_is_kept, NULL },
    { "an_empty_element_inside_the_path_is_refused",
      t_an_empty_element_inside_the_path_is_refused, NULL },
    { "an_unnamed_insert_hands_back_a_handle",
      t_an_unnamed_insert_hands_back_a_handle, NULL },
    { "the_pointer_bias_outlives_the_handle",
      t_the_pointer_bias_outlives_the_handle, NULL },
    { "a_named_object_is_reachable_by_its_name",
      t_a_named_object_is_reachable_by_its_name, NULL },
    { "a_second_insert_of_the_name_collides",
      t_a_second_insert_of_the_name_collides, NULL },
    { "openif_returns_the_object_already_there",
      t_openif_returns_the_object_already_there, NULL },
    { "a_handle_of_another_type_is_refused",
      t_a_handle_of_another_type_is_refused, NULL },
};

DEFINE_GROUP(ob_create, "ob/create");
