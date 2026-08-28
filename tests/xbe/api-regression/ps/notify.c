/*
 * PsSetCreateThreadNotifyRoutine -- the thread-lifetime callback.
 *
 * The console's callback signature is not NT's: the first argument is
 * the ETHREAD itself, where NT passes the owning process id.  A title
 * uses it to hang per-thread state off a thread it did not create, so
 * the pointer, the id and the create/destroy flag all have to be right.
 *
 * There is no export that takes a routine back off the list, so the one
 * registered here stays for the rest of the boot.  It records into a
 * small table while a test is watching and costs nothing otherwise.
 */

#include "../harness.h"

#define SEEN 16

typedef struct {
    PVOID  thread;
    HANDLE id;
} seen_t;

static volatile LONG g_watching;
static seen_t g_created[SEEN];
static seen_t g_destroyed[SEEN];
static volatile LONG g_created_count;
static volatile LONG g_destroyed_count;
static KEVENT g_exited;

static void NTAPI notify(PETHREAD Thread, HANDLE ThreadId, BOOLEAN Create)
{
    LONG slot;

    if (InterlockedCompareExchange(&g_watching, 0, 0) == 0)
        return;

    if (Create) {
        slot = InterlockedIncrement(&g_created_count) - 1;
        if (slot < SEEN) {
            g_created[slot].thread = Thread;
            g_created[slot].id = ThreadId;
        }
    } else {
        slot = InterlockedIncrement(&g_destroyed_count) - 1;
        if (slot < SEEN) {
            g_destroyed[slot].thread = Thread;
            g_destroyed[slot].id = ThreadId;
        }
    }
}

static void start_watching(void)
{
    RtlZeroMemory(g_created, sizeof(g_created));
    RtlZeroMemory(g_destroyed, sizeof(g_destroyed));
    g_created_count = 0;
    g_destroyed_count = 0;
    InterlockedExchange(&g_watching, 1);
}

static PVOID reported(const seen_t *table, LONG count, HANDLE id)
{
    LONG i;

    if (count > SEEN)
        count = SEEN;
    for (i = 0; i < count; i++) {
        if (table[i].id == id)
            return table[i].thread;
    }
    return NULL;
}

static void NTAPI test_system_routine(PKSTART_ROUTINE StartRoutine,
                                      PVOID StartContext)
{
    if (StartRoutine != NULL)
        StartRoutine(StartContext);
    PsTerminateSystemThread(STATUS_SUCCESS);
}

static void NTAPI briefly(PVOID arg)
{
    (void)arg;
    KeSetEvent(&g_exited, 0, FALSE);
}

/* A suspended thread and the object behind its handle.  Suspended, so
 * the create-side callback has run and the destroy-side one cannot
 * have. */
static NTSTATUS start_watched(HANDLE *h, HANDLE *id, PVOID *object)
{
    NTSTATUS s;

    KeInitializeEvent(&g_exited, NotificationEvent, FALSE);
    *h = NULL;
    *id = NULL;
    *object = NULL;

    s = PsCreateSystemThreadEx(h, 0, 0, 0, id, briefly, NULL, TRUE, FALSE,
                               test_system_routine);
    if (!NT_SUCCESS(s))
        return s;

    s = ObReferenceObjectByHandle(*h, &PsThreadObjectType, object);
    if (!NT_SUCCESS(s)) {
        NtResumeThread(*h, NULL);
        NtClose(*h);
        return s;
    }
    return STATUS_SUCCESS;
}

/* Let it run to the end and reap it. */
static NTSTATUS finish_watched(HANDLE h, PVOID object)
{
    LARGE_INTEGER timeout;
    NTSTATUS s;

    timeout.QuadPart = -((LONGLONG)2 * 1000 * 10000);
    NtResumeThread(h, NULL);
    KeWaitForSingleObject(&g_exited, Executive, KernelMode, FALSE, &timeout);
    /* The destroy-side callback runs after the routine returns, so wait
     * on the thread itself rather than on what it signalled. */
    s = NtWaitForSingleObjectEx(h, KernelMode, FALSE, &timeout);
    if (object != NULL)
        ObfDereferenceObject(object);
    NtClose(h);
    return s;
}

static bool t_a_routine_is_accepted(void)
{
    ASSERT_NTSTATUS(PsSetCreateThreadNotifyRoutine(notify), STATUS_SUCCESS);
    return true;
}

static bool t_the_create_side_reports_the_new_thread(void)
{
    HANDLE h, id;
    PVOID object, on_create;
    NTSTATUS s;

    start_watching();
    s = start_watched(&h, &id, &object);
    if (!NT_SUCCESS(s)) {
        InterlockedExchange(&g_watching, 0);
        FAIL_AND_RETURN("create thread: 0x%08x", (unsigned)s);
    }

    on_create = reported(g_created, g_created_count, id);
    s = finish_watched(h, object);
    InterlockedExchange(&g_watching, 0);

    if (!NT_SUCCESS(s))
        FAIL_AND_RETURN("waiting for the thread: 0x%08x", (unsigned)s);
    ASSERT_NOT_NULL(id);
    ASSERT_EQ_PTR(on_create, object);
    return true;
}

static bool t_the_destroy_side_reports_it_too(void)
{
    HANDLE h, id;
    PVOID object, on_destroy;
    NTSTATUS s;

    start_watching();
    s = start_watched(&h, &id, &object);
    if (!NT_SUCCESS(s)) {
        InterlockedExchange(&g_watching, 0);
        FAIL_AND_RETURN("create thread: 0x%08x", (unsigned)s);
    }

    if (reported(g_destroyed, g_destroyed_count, id) != NULL) {
        finish_watched(h, object);
        InterlockedExchange(&g_watching, 0);
        FAIL_AND_RETURN("destroy reported while the thread was suspended");
    }

    s = finish_watched(h, object);
    on_destroy = reported(g_destroyed, g_destroyed_count, id);
    InterlockedExchange(&g_watching, 0);

    if (!NT_SUCCESS(s))
        FAIL_AND_RETURN("waiting for the thread: 0x%08x", (unsigned)s);
    ASSERT_EQ_PTR(on_destroy, object);
    return true;
}

/* Nothing fires once the test stops watching -- which is only worth
 * stating because there is no way to unregister: the callback stays on
 * the list for the rest of the suite. */
static bool t_the_routine_stays_registered(void)
{
    HANDLE h, id;
    PVOID object;
    NTSTATUS s;
    LONG before;

    start_watching();
    before = g_created_count;
    s = start_watched(&h, &id, &object);
    if (!NT_SUCCESS(s)) {
        InterlockedExchange(&g_watching, 0);
        FAIL_AND_RETURN("create thread: 0x%08x", (unsigned)s);
    }
    s = finish_watched(h, object);
    InterlockedExchange(&g_watching, 0);

    if (!NT_SUCCESS(s))
        FAIL_AND_RETURN("waiting for the thread: 0x%08x", (unsigned)s);
    ASSERT_TRUE(g_created_count > before);
    return true;
}

static const test_entry_t ps_notify_entries[] = {
    { "a_routine_is_accepted", t_a_routine_is_accepted, NULL },
    { "the_create_side_reports_the_new_thread",
      t_the_create_side_reports_the_new_thread, NULL },
    { "the_destroy_side_reports_it_too",
      t_the_destroy_side_reports_it_too, NULL },
    { "the_routine_stays_registered", t_the_routine_stays_registered, NULL },
};

DEFINE_GROUP(ps_notify, "ps/notify");
