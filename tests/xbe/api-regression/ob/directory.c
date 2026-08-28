/*
 * NtCreateDirectoryObject -- a title's own corner of the object
 * namespace.
 *
 * The console's form takes no desired access, only a handle out and the
 * attributes, so a directory is created wide open.  What matters to a
 * caller is that the handle it gets back is a real container: names
 * created under it resolve against it and nowhere else, and the
 * directory enumerates them.
 *
 * Every directory here is created without a name, which keeps the tests
 * out of the shared namespace: the handle is the only way to reach it,
 * and closing it takes the directory with it.
 */

#include "../harness.h"
#include <string.h>

#ifndef OBJ_OPENIF
#define OBJ_OPENIF 0x00000080L
#endif

#ifndef STATUS_OBJECT_NAME_COLLISION
#define STATUS_OBJECT_NAME_COLLISION ((NTSTATUS)0xC0000035L)
#endif

static void init_str(OBJECT_STRING *s, const char *str)
{
    s->Buffer = (PCHAR)str;
    s->Length = (USHORT)strlen(str);
    s->MaximumLength = s->Length + 1;
}

static NTSTATUS create_anonymous_directory(HANDLE *h)
{
    OBJECT_ATTRIBUTES oa = {
        .RootDirectory = NULL,
        .ObjectName    = NULL,
        .Attributes    = 0,
    };

    *h = NULL;
    return NtCreateDirectoryObject(h, &oa);
}

/* A named event under `root`, created or opened. */
static NTSTATUS create_event_in(HANDLE root, const char *name, HANDLE *h)
{
    OBJECT_STRING str;
    OBJECT_ATTRIBUTES oa;

    init_str(&str, name);
    oa.RootDirectory = root;
    oa.ObjectName    = &str;
    oa.Attributes    = OBJ_OPENIF | OBJ_CASE_INSENSITIVE;

    *h = NULL;
    return NtCreateEvent(h, &oa, NotificationEvent, FALSE);
}

static bool t_a_directory_comes_back_as_a_handle(void)
{
    HANDLE h;
    NTSTATUS s;

    s = create_anonymous_directory(&h);
    ASSERT_NTSTATUS(s, STATUS_SUCCESS);
    ASSERT_NOT_NULL(h);
    ASSERT_NTSTATUS(NtClose(h), STATUS_SUCCESS);
    return true;
}

/* Two creates of one name under the same directory are one object. */
static bool t_a_name_under_it_resolves_to_one_object(void)
{
    HANDLE dir, first = NULL, second = NULL;
    EVENT_BASIC_INFORMATION info;
    NTSTATUS s;

    s = create_anonymous_directory(&dir);
    if (!NT_SUCCESS(s))
        FAIL_AND_RETURN("create directory -> 0x%08x", (unsigned)s);

    s = create_event_in(dir, "apireg_shared", &first);
    if (!NT_SUCCESS(s)) {
        NtClose(dir);
        FAIL_AND_RETURN("first create -> 0x%08x", (unsigned)s);
    }
    s = create_event_in(dir, "apireg_shared", &second);
    if (!NT_SUCCESS(s)) {
        NtClose(first);
        NtClose(dir);
        FAIL_AND_RETURN("second create -> 0x%08x", (unsigned)s);
    }

    NtSetEvent(first, NULL);
    memset(&info, 0xCC, sizeof(info));
    s = NtQueryEvent(second, &info);

    NtClose(second);
    NtClose(first);
    NtClose(dir);

    ASSERT_NTSTATUS(s, STATUS_SUCCESS);
    ASSERT_EQ_U32(info.EventState, 1);
    return true;
}

/* And two directories keep one name apart. */
static bool t_the_same_name_in_two_directories_is_two_objects(void)
{
    HANDLE left, right, in_left = NULL, in_right = NULL;
    EVENT_BASIC_INFORMATION info;
    NTSTATUS s;

    s = create_anonymous_directory(&left);
    if (!NT_SUCCESS(s))
        FAIL_AND_RETURN("create first directory -> 0x%08x", (unsigned)s);
    s = create_anonymous_directory(&right);
    if (!NT_SUCCESS(s)) {
        NtClose(left);
        FAIL_AND_RETURN("create second directory -> 0x%08x", (unsigned)s);
    }

    s = create_event_in(left, "apireg_apart", &in_left);
    if (NT_SUCCESS(s))
        s = create_event_in(right, "apireg_apart", &in_right);
    if (!NT_SUCCESS(s)) {
        if (in_left != NULL) NtClose(in_left);
        NtClose(right);
        NtClose(left);
        FAIL_AND_RETURN("create event -> 0x%08x", (unsigned)s);
    }

    NtSetEvent(in_left, NULL);
    memset(&info, 0xCC, sizeof(info));
    s = NtQueryEvent(in_right, &info);

    NtClose(in_right);
    NtClose(in_left);
    NtClose(right);
    NtClose(left);

    ASSERT_NTSTATUS(s, STATUS_SUCCESS);
    ASSERT_EQ_U32(info.EventState, 0);
    return true;
}

/* A directory created under another one is reached through it, and a
 * second create of that name without OBJ_OPENIF collides. */
static bool t_a_second_create_of_one_name_collides(void)
{
    HANDLE parent, child = NULL, again = NULL;
    OBJECT_STRING name;
    OBJECT_ATTRIBUTES oa;
    NTSTATUS s;

    s = create_anonymous_directory(&parent);
    if (!NT_SUCCESS(s))
        FAIL_AND_RETURN("create parent -> 0x%08x", (unsigned)s);

    init_str(&name, "apireg_sub");
    oa.RootDirectory = parent;
    oa.ObjectName    = &name;
    oa.Attributes    = OBJ_CASE_INSENSITIVE;

    s = NtCreateDirectoryObject(&child, &oa);
    if (!NT_SUCCESS(s)) {
        NtClose(parent);
        FAIL_AND_RETURN("create child -> 0x%08x", (unsigned)s);
    }

    s = NtCreateDirectoryObject(&again, &oa);
    if (NT_SUCCESS(s))
        NtClose(again);
    NtClose(child);
    NtClose(parent);

    ASSERT_NTSTATUS(s, STATUS_OBJECT_NAME_COLLISION);
    return true;
}

/* The directory enumerates what was created in it.  The names are ANSI
 * in-buffer, so a byte scan finds one without depending on the entry
 * stride. */
static bool t_the_directory_enumerates_its_entries(void)
{
    static const char NEEDLE[] = "apireg_listed";
    HANDLE dir, ev = NULL;
    char buf[512];
    ULONG ctx = 0, retlen = 0;
    BOOLEAN restart = TRUE;
    bool found = false;
    int calls = 0;
    NTSTATUS s;
    size_t i;

    s = create_anonymous_directory(&dir);
    if (!NT_SUCCESS(s))
        FAIL_AND_RETURN("create directory -> 0x%08x", (unsigned)s);

    s = create_event_in(dir, NEEDLE, &ev);
    if (!NT_SUCCESS(s)) {
        NtClose(dir);
        FAIL_AND_RETURN("create event -> 0x%08x", (unsigned)s);
    }

    for (;;) {
        memset(buf, 0, sizeof(buf));
        s = NtQueryDirectoryObject(dir, buf, sizeof(buf), restart, &ctx,
                                   &retlen);
        restart = FALSE;
        if (!NT_SUCCESS(s))
            break;
        calls++;
        for (i = 0; i + sizeof(NEEDLE) - 1 <= sizeof(buf); i++) {
            if (memcmp(buf + i, NEEDLE, sizeof(NEEDLE) - 1) == 0) {
                found = true;
                break;
            }
        }
        if (found || calls > 16)
            break;
    }

    NtClose(ev);
    NtClose(dir);

    if (calls == 0)
        FAIL_AND_RETURN("first enumeration -> 0x%08x", (unsigned)s);
    ASSERT_TRUE(found);
    return true;
}

static const test_entry_t ob_directory_entries[] = {
    { "a_directory_comes_back_as_a_handle",
      t_a_directory_comes_back_as_a_handle, NULL },
    { "a_name_under_it_resolves_to_one_object",
      t_a_name_under_it_resolves_to_one_object, NULL },
    { "the_same_name_in_two_directories_is_two_objects",
      t_the_same_name_in_two_directories_is_two_objects, NULL },
    { "a_second_create_of_one_name_collides",
      t_a_second_create_of_one_name_collides, NULL },
    { "the_directory_enumerates_its_entries",
      t_the_directory_enumerates_its_entries, NULL },
};

DEFINE_GROUP(ob_directory, "ob/directory");
