/*
 * PsCreateSystemThreadEx with explicit KernelStackSize. Titles
 * crash if the requested stack size isn't honored and the
 * thread drops to a 48 KB default.
 */

#include "../harness.h"

typedef struct {
    KEVENT done;
    BOOLEAN ok;
    SIZE_T  consumed;
} ctx_t;

/* Retail's created thread enters through SystemRoutine(StartRoutine,
 * StartContext) -- XAPI passes XapiThreadStartup here.  Passing NULL
 * makes retail jump to address 0 (bugcheck 0x1E), which is what kept
 * the Ex-form tests disabled.  Provide the XAPI-shaped wrapper. */
static void NTAPI test_system_routine(PKSTART_ROUTINE StartRoutine,
                                      PVOID StartContext)
{
    if (StartRoutine != NULL)
        StartRoutine(StartContext);
    PsTerminateSystemThread(STATUS_SUCCESS);
}

/* Smoke test of the PsCreateSystemThreadEx dispatch -- specifically the
 * 10-argument ordinal-255 signature. We don't probe the KernelStackSize
 * hint here because stack-probe behavior (compiler-emitted __chkstk vs
 * the kernel's stack-guard layout) is too implementation-defined to
 * assert from a portable XBE.
 *
 * Previously disabled on retail: the harness passed SystemRoutine=NULL
 * and retail's new thread enters through SystemRoutine, so it jumped
 * to 0.  Fixed by test_system_routine above. */
static void NTAPI explicit_thread(PVOID arg)
{
    ctx_t *ctx = (ctx_t *)arg;
    ctx->ok = TRUE;
    KeSetEvent(&ctx->done, 0, FALSE);
    /* Return: test_system_routine terminates the thread. */
}

static bool t_create_with_explicit_stack(void)
{
    ctx_t ctx;
    KeInitializeEvent(&ctx.done, NotificationEvent, FALSE);
    ctx.ok = FALSE;
    ctx.consumed = 0;

    HANDLE h = NULL;
    NTSTATUS s = PsCreateSystemThreadEx(&h, 0, 64 * 1024, 0, NULL,
                                        explicit_thread, &ctx,
                                        FALSE, FALSE, test_system_routine);
    ASSERT_NTSTATUS(s, STATUS_SUCCESS);
    ASSERT_NOT_NULL(h);

    LARGE_INTEGER timeout = { .QuadPart = -((LONGLONG)2 * 1000 * 10000) };  /* 2 s */
    s = KeWaitForSingleObject(&ctx.done, Executive, KernelMode, FALSE, &timeout);
    ASSERT_NTSTATUS(s, STATUS_SUCCESS);
    if (!ctx.ok) FAIL_AND_RETURN("thread did not signal completion");
    NtClose(h);
    return true;
}

static void NTAPI tiny_thread(PVOID arg)
{
    KEVENT *done = (KEVENT *)arg;
    KeSetEvent(done, 0, FALSE);
    PsTerminateSystemThread(STATUS_SUCCESS);
}

static bool t_create_default(void)
{
    KEVENT done;
    KeInitializeEvent(&done, NotificationEvent, FALSE);

    HANDLE h = NULL;
    NTSTATUS s = PsCreateSystemThread(&h, NULL, tiny_thread, &done, FALSE);
    ASSERT_NTSTATUS(s, STATUS_SUCCESS);
    ASSERT_NOT_NULL(h);

    LARGE_INTEGER timeout = { .QuadPart = -((LONGLONG)1 * 1000 * 10000) };  /* 1 s */
    s = KeWaitForSingleObject(&done, Executive, KernelMode, FALSE, &timeout);
    ASSERT_NTSTATUS(s, STATUS_SUCCESS);
    NtClose(h);
    return true;
}


/* CreateSuspended is part of the ordinal contract: XAPI's
 * CreateThread(CREATE_SUSPENDED) relies on the thread not running
 * until NtResumeThread so the creator can prime priority/affinity and
 * shared state first.  The kernel-side arms were once trimmed on the
 * assumption the flag was always FALSE (reverted), and the ordinal shim
 * used to drop the flag entirely. */
static volatile LONG g_susp_ran;

static void NTAPI suspended_thread(PVOID arg)
{
    ctx_t *ctx = (ctx_t *)arg;
    InterlockedExchange(&g_susp_ran, 1);
    KeSetEvent(&ctx->done, 0, FALSE);
    /* Return: test_system_routine terminates the thread. */
}

static bool t_create_suspended(void)
{
    ctx_t ctx;
    HANDLE h = NULL;
    NTSTATUS s;

    KeInitializeEvent(&ctx.done, NotificationEvent, FALSE);
    g_susp_ran = 0;

    s = PsCreateSystemThreadEx(&h, 0, 0, 0, NULL, suspended_thread, &ctx,
                               TRUE /* CreateSuspended */, FALSE,
                               test_system_routine);
    ASSERT_NTSTATUS(s, STATUS_SUCCESS);
    ASSERT_NOT_NULL(h);

    /* Give a wrongly-running thread ample time to betray itself. */
    {
        LARGE_INTEGER delay = { .QuadPart = -(LONGLONG)(100 * 10000) };
        KeDelayExecutionThread(KernelMode, FALSE, &delay);
    }
    if (g_susp_ran != 0) {
        NtResumeThread(h, NULL);
        NtClose(h);
        FAIL_AND_RETURN("thread ran before NtResumeThread");
    }

    /* NtResumeThread writes back the pre-resume suspend count; a
     * CreateSuspended thread carries exactly one suspend.  Retail
     * returns prev=1 here.  The write-back is NOT SEH-protected on
     * retail: a garbage non-NULL SuspendCount pointer bugchecks retail
     * 0x1E (0xC0000005 write), so only the good-pointer path is
     * contract.  (NtSuspendThread on a terminated thread returns
     * STATUS_THREAD_IS_TERMINATING with the count untouched -- recorded
     * here for when that ordinal gets wired; it is a bugcheck scaffold
     * today, so it cannot be asserted in-suite.) */
    {
        ULONG prev = 0xeeeeeeee;
        s = NtResumeThread(h, &prev);
        ASSERT_NTSTATUS(s, STATUS_SUCCESS);
        ASSERT_EQ_U32(prev, 1);
    }

    {
        LARGE_INTEGER timeout = { .QuadPart = -((LONGLONG)2 * 1000 * 10000) };
        s = KeWaitForSingleObject(&ctx.done, Executive, KernelMode, FALSE,
                                  &timeout);
    }
    NtClose(h);
    ASSERT_NTSTATUS(s, STATUS_SUCCESS);
    if (g_susp_ran != 1)
        FAIL_AND_RETURN("thread never ran after resume");
    return true;
}

static const test_entry_t ps_thread_entries[] = {
    {"create_default",              t_create_default},
    {"create_suspended",            t_create_suspended},
    {"create_with_explicit_stack",  t_create_with_explicit_stack},
};

DEFINE_GROUP(ps_thread, "ps/thread");
