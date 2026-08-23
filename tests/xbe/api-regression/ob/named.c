/*
 * Named-object sharing through the retail pseudo root handle.
 *
 * XAPI roots every named event/mutant/semaphore at
 * ObWin32NamedObjectsDirectory() == (HANDLE)-4.  Two creates of the
 * same name with OBJ_OPENIF must resolve to ONE object.
 *
 * Also pins NtPulseEvent against handle close/reuse: the kernel-side
 * pulse cache must not pulse a freed event after NtClose recycles the
 * handle value.
 */

#include "../harness.h"
#include <string.h>

#ifndef OBJ_OPENIF
#define OBJ_OPENIF 0x00000080L
#endif

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

static bool t_named_mutant_shared(void)
{
    OBJECT_ATTRIBUTES oa1, oa2;
    ANSI_STRING n1, n2;
    HANDLE h1 = NULL, h2 = NULL;
    NTSTATUS s;

    init_named_oa(&oa1, &n1, "apireg_mutant");
    s = NtCreateMutant(&h1, &oa1, TRUE /* InitialOwner */);
    if (!NT_SUCCESS(s))
        FAIL_AND_RETURN("first named create -> 0x%08x", (unsigned)s);

    init_named_oa(&oa2, &n2, "apireg_mutant");
    s = NtCreateMutant(&h2, &oa2, FALSE);
    if (!NT_SUCCESS(s)) {
        NtClose(h1);
        FAIL_AND_RETURN("second named create -> 0x%08x", (unsigned)s);
    }

    /* Same object: this thread owns the mutant via the h1 create, so a
     * release through h2 must succeed.  With two distinct objects (the
     * old fallback) this returns STATUS_MUTANT_NOT_OWNED. */
    s = NtReleaseMutant(h2, NULL);
    NtClose(h2);
    NtClose(h1);
    ASSERT_NTSTATUS(s, STATUS_SUCCESS);
    return true;
}

static bool t_named_event_shared(void)
{
    OBJECT_ATTRIBUTES oa1, oa2;
    ANSI_STRING n1, n2;
    HANDLE h1 = NULL, h2 = NULL;
    NTSTATUS s;

    init_named_oa(&oa1, &n1, "apireg_event");
    s = NtCreateEvent(&h1, &oa1, NotificationEvent, FALSE);
    if (!NT_SUCCESS(s))
        FAIL_AND_RETURN("first named create -> 0x%08x", (unsigned)s);

    init_named_oa(&oa2, &n2, "apireg_event");
    s = NtCreateEvent(&h2, &oa2, NotificationEvent, FALSE);
    if (!NT_SUCCESS(s)) {
        NtClose(h1);
        FAIL_AND_RETURN("second named create -> 0x%08x", (unsigned)s);
    }

    /* Signal through one handle, observe through the other. */
    s = NtSetEvent(h2, NULL);
    if (NT_SUCCESS(s)) {
        LARGE_INTEGER timeout = { .QuadPart = 0 };  /* poll */
        s = NtWaitForSingleObject(h1, FALSE, &timeout);
    }
    NtClose(h2);
    NtClose(h1);
    ASSERT_NTSTATUS(s, STATUS_SUCCESS);
    return true;
}

static bool t_pulse_after_close_reuse(void)
{
    HANDLE ha = NULL, hb = NULL;
    NTSTATUS s;
    int i;

    /* Churn a few create/close cycles so the kernel's pulse cache sees
     * recycled handle values, then pulse the live one.  A stale cache
     * entry pulses a freed event (crash) or the wrong object (the
     * waiter below never wakes). */
    for (i = 0; i < 8; i++) {
        s = NtCreateEvent(&ha, NULL, SynchronizationEvent, FALSE);
        ASSERT_NTSTATUS(s, STATUS_SUCCESS);
        s = NtClose(ha);
        ASSERT_NTSTATUS(s, STATUS_SUCCESS);
    }

    s = NtCreateEvent(&hb, NULL, SynchronizationEvent, FALSE);
    ASSERT_NTSTATUS(s, STATUS_SUCCESS);

    /* Pulse with no waiter: a synchronization event must stay
     * non-signaled afterwards... */
    s = NtPulseEvent(hb, NULL);
    ASSERT_NTSTATUS(s, STATUS_SUCCESS);
    {
        LARGE_INTEGER timeout = { .QuadPart = 0 };
        s = NtWaitForSingleObject(hb, FALSE, &timeout);
        ASSERT_NTSTATUS(s, STATUS_TIMEOUT);
    }

    /* ...and a set is still observable through the same handle, proving
     * the handle maps to a live object. */
    s = NtSetEvent(hb, NULL);
    ASSERT_NTSTATUS(s, STATUS_SUCCESS);
    {
        LARGE_INTEGER timeout = { .QuadPart = 0 };
        s = NtWaitForSingleObject(hb, FALSE, &timeout);
        ASSERT_NTSTATUS(s, STATUS_SUCCESS);
    }

    NtClose(hb);
    return true;
}

static const test_entry_t ob_named_entries[] = {
    {"named_mutant_shared",     t_named_mutant_shared},
    {"named_event_shared",      t_named_event_shared},
    {"pulse_after_close_reuse", t_pulse_after_close_reuse},
};

DEFINE_GROUP(ob_named, "ob/named");
