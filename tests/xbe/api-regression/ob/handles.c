/*
 * Handle-table growth: allocate far past the initial table so the
 * slow-path table expansion runs, spot-check that early, middle, and
 * late handles all still resolve, and free everything.
 */

#include "../harness.h"

#define NHANDLES 1500

static bool t_table_growth(void)
{
    static HANDLE h[NHANDLES];
    ULONG created = 0;
    NTSTATUS s = STATUS_SUCCESS;

    for (ULONG i = 0; i < NHANDLES; i++) {
        s = NtCreateEvent(&h[i], NULL, NotificationEvent, TRUE);
        if (!NT_SUCCESS(s))
            break;
        created++;
    }

    bool ok = true;
    if (created == NHANDLES) {
        /* Handles from every region of the grown table must resolve. */
        static const ULONG probe[] = { 0, NHANDLES / 2, NHANDLES - 1 };
        LARGE_INTEGER zero = { .QuadPart = 0 };
        for (ULONG i = 0; ok && i < 3; i++) {
            HANDLE hs[1] = { h[probe[i]] };
            NTSTATUS ws = NtWaitForMultipleObjectsEx(1, hs, WaitAny,
                                                     UserMode, FALSE, &zero);
            if (ws != STATUS_WAIT_0) {
                test_record_failure(__FILE__, __LINE__,
                                    "handle %u: wait -> 0x%08x",
                                    (unsigned)probe[i], (unsigned)ws);
                ok = false;
            }
        }
    } else {
        test_record_failure(__FILE__, __LINE__,
                            "create %u/%u -> 0x%08x",
                            (unsigned)created, NHANDLES, (unsigned)s);
        ok = false;
    }

    for (ULONG i = 0; i < created; i++)
        NtClose(h[i]);
    return ok;
}

/* The freed table slots must be reusable afterwards. */
static bool t_slots_recycle(void)
{
    HANDLE h = NULL;
    NTSTATUS s = NtCreateEvent(&h, NULL, NotificationEvent, FALSE);
    ASSERT_NTSTATUS(s, STATUS_SUCCESS);
    NtClose(h);
    return true;
}

static const test_entry_t ob_handles_entries[] = {
    {"table_growth",  t_table_growth},
    {"slots_recycle", t_slots_recycle},
};

DEFINE_GROUP(ob_handles, "ob/handles");
