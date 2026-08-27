/*
 * Alertable UserMode waits + user-APC delivery.
 *
 * Regression coverage for the KiCheckAlertability trim: folding the
 * WaitMode arms to KernelMode-only meant a queued
 * IO-completion user APC could no longer run, and a title's
 * overlapped-IO writer thread stalled to one IO per clock tick.
 *
 * Retail-validated semantics: the NtWriteFile
 * completion APC routine RUNS for an alertable UserMode delay, and the
 * delay itself completes with STATUS_SUCCESS (the APC for a
 * synchronously-completed write is delivered at/before wait entry, so
 * no STATUS_USER_APC early-return is observed in this shape).
 */

#include "../harness.h"
#include <string.h>

static const char APC_SCRATCH[] =
    "\\Device\\Harddisk0\\Partition1\\nxkrnl-api-apc.tmp";

#ifndef FILE_DELETE_ON_CLOSE
#define FILE_DELETE_ON_CLOSE 0x00001000
#endif
#ifndef STATUS_USER_APC
#define STATUS_USER_APC ((NTSTATUS)0x000000C0L)
#endif

static volatile LONG g_apc_ran;
static IO_STATUS_BLOCK g_apc_iosb_copy;

static VOID NTAPI io_apc(PVOID ApcContext, PIO_STATUS_BLOCK IoStatusBlock,
                         ULONG Reserved)
{
    (void)Reserved;
    if (IoStatusBlock != NULL)
        g_apc_iosb_copy = *IoStatusBlock;
    InterlockedExchange((PLONG)ApcContext, 1);
}

/* Open WITHOUT FILE_SYNCHRONOUS_IO_* so completion is delivered through
 * the APC path like a title's overlapped IO. */
static NTSTATUS open_apc_scratch(HANDLE *h, IO_STATUS_BLOCK *iosb)
{
    ANSI_STRING name = {
        .Length        = (USHORT)(sizeof(APC_SCRATCH) - 1),
        .MaximumLength = sizeof(APC_SCRATCH),
        .Buffer        = (PCHAR)APC_SCRATCH,
    };
    OBJECT_ATTRIBUTES oa = {
        .RootDirectory = NULL,
        .ObjectName    = &name,
        .Attributes    = OBJ_CASE_INSENSITIVE,
    };
    return NtCreateFile(h, GENERIC_READ | GENERIC_WRITE | SYNCHRONIZE, &oa,
                        iosb, NULL, FILE_ATTRIBUTE_NORMAL, 0,
                        FILE_OVERWRITE_IF,
                        FILE_NON_DIRECTORY_FILE | FILE_DELETE_ON_CLOSE);
}

static bool t_io_apc_breaks_alertable_userwait(void)
{
    HANDLE h;
    IO_STATUS_BLOCK open_iosb, write_iosb;
    static char buf[2048];
    NTSTATUS s;

    g_apc_ran = 0;
    memset(buf, 0x5a, sizeof(buf));
    memset(&write_iosb, 0, sizeof(write_iosb));

    s = open_apc_scratch(&h, &open_iosb);
    ASSERT_NTSTATUS(s, STATUS_SUCCESS);

    {
        /* Non-synchronous handle: an explicit offset is required. */
        LARGE_INTEGER off = { .QuadPart = 0 };
        s = NtWriteFile(h, NULL, io_apc, (PVOID)&g_apc_ran, &write_iosb,
                        buf, sizeof(buf), &off);
    }
    if (!NT_SUCCESS(s) && s != STATUS_PENDING) {
        NtClose(h);
        FAIL_AND_RETURN("NtWriteFile -> 0x%08x", (unsigned)s);
    }

    /* Retail: the queued completion APC interrupts an alertable
     * UserMode delay with STATUS_USER_APC and the routine runs before
     * the wait returns.  A 2 s timeout means a broken alertability
     * path fails fast instead of hanging the suite. */
    {
        LARGE_INTEGER timeout = { .QuadPart = -((LONGLONG)2 * 1000 * 10000) };
        s = KeDelayExecutionThread(UserMode, TRUE, &timeout);
    }

    NtClose(h);

    /* Retail: SUCCESS, with the routine already delivered. */
    ASSERT_NTSTATUS(s, STATUS_SUCCESS);
    if (g_apc_ran != 1)
        FAIL_AND_RETURN("APC routine never ran (wait=0x%08x)", (unsigned)s);
    if (!NT_SUCCESS(g_apc_iosb_copy.Status))
        FAIL_AND_RETURN("APC saw iosb.Status=0x%08x",
                        (unsigned)g_apc_iosb_copy.Status);
    return true;
}

static bool t_nonalertable_userwait_ignores_apc(void)
{
    HANDLE h;
    IO_STATUS_BLOCK open_iosb, write_iosb;
    static char buf[512];
    NTSTATUS s;

    g_apc_ran = 0;
    memset(buf, 0xa5, sizeof(buf));
    memset(&write_iosb, 0, sizeof(write_iosb));

    s = open_apc_scratch(&h, &open_iosb);
    ASSERT_NTSTATUS(s, STATUS_SUCCESS);

    {
        LARGE_INTEGER off = { .QuadPart = 0 };
        s = NtWriteFile(h, NULL, io_apc, (PVOID)&g_apc_ran, &write_iosb,
                        buf, sizeof(buf), &off);
    }
    if (!NT_SUCCESS(s) && s != STATUS_PENDING) {
        NtClose(h);
        FAIL_AND_RETURN("NtWriteFile -> 0x%08x", (unsigned)s);
    }

    /* Non-alertable: the delay must run to its (short) timeout without
     * being broken by the pending APC. */
    {
        LARGE_INTEGER timeout = { .QuadPart = -(LONGLONG)(50 * 10000) };
        s = KeDelayExecutionThread(UserMode, FALSE, &timeout);
    }
    ASSERT_NTSTATUS(s, STATUS_SUCCESS);

    /* Drain the APC so it doesn't leak into a later test's wait. */
    {
        LARGE_INTEGER timeout = { .QuadPart = -(LONGLONG)(2000 * 10000) };
        s = KeDelayExecutionThread(UserMode, TRUE, &timeout);
    }
    NtClose(h);
    ASSERT_NTSTATUS(s, STATUS_SUCCESS);
    if (g_apc_ran != 1)
        FAIL_AND_RETURN("queued APC never delivered (drain=0x%08x)",
                        (unsigned)s);
    return true;
}

#ifndef STATUS_NO_YIELD_PERFORMED
#define STATUS_NO_YIELD_PERFORMED ((NTSTATUS)0x40000024L)
#endif

static bool t_sleep_zero_usermode(void)
{
    /* Sleep(0) maps to a zero-interval UserMode delay; the KeDelay trim
     * dropped its yield fast-path.  The yield reports
     * STATUS_NO_YIELD_PERFORMED when no other thread is ready, so the
     * exact status depends on scheduler state -- accept either
     * success-class result, just not an error or a hang. */
    LARGE_INTEGER zero = { .QuadPart = 0 };
    NTSTATUS s = KeDelayExecutionThread(UserMode, FALSE, &zero);
    if (s != STATUS_SUCCESS && s != STATUS_NO_YIELD_PERFORMED)
        FAIL_AND_RETURN("Sleep(0) -> 0x%08x", (unsigned)s);
    return true;
}

/* KeEnterCriticalRegion suppresses normal kernel-APC delivery until the
 * matching leave.  Nothing a title can queue is a kernel APC, so what is
 * observable from here is the balance: an unbalanced or missing enter
 * leaves the thread's disable count wrong, and the completion-APC round
 * trip above is the machinery that notices.  Nesting is safe for the
 * plain pair -- unlike the AndRegion form, it keeps no recursion count
 * of its own and simply counts down. */
static bool t_critical_region_pairs_are_balanced(void)
{
    HANDLE h;
    IO_STATUS_BLOCK open_iosb, write_iosb;
    static char buf[512];
    NTSTATUS s;

    KeEnterCriticalRegion();
    KeEnterCriticalRegion();
    KeLeaveCriticalRegion();
    KeLeaveCriticalRegion();

    g_apc_ran = 0;
    memset(buf, 0x33, sizeof(buf));
    memset(&write_iosb, 0, sizeof(write_iosb));

    s = open_apc_scratch(&h, &open_iosb);
    ASSERT_NTSTATUS(s, STATUS_SUCCESS);

    {
        LARGE_INTEGER off = { .QuadPart = 0 };
        s = NtWriteFile(h, NULL, io_apc, (PVOID)&g_apc_ran, &write_iosb,
                        buf, sizeof(buf), &off);
    }
    if (!NT_SUCCESS(s) && s != STATUS_PENDING) {
        NtClose(h);
        FAIL_AND_RETURN("NtWriteFile -> 0x%08x", (unsigned)s);
    }

    {
        LARGE_INTEGER timeout = { .QuadPart = -((LONGLONG)2 * 1000 * 10000) };
        s = KeDelayExecutionThread(UserMode, TRUE, &timeout);
    }
    NtClose(h);

    ASSERT_NTSTATUS(s, STATUS_SUCCESS);
    if (g_apc_ran != 1)
        FAIL_AND_RETURN("APC never ran after a nested region pair "
                        "(wait=0x%08x)", (unsigned)s);
    return true;
}

static const test_entry_t ke_wait_apc_entries[] = {
    /* sleep_zero runs first: the APC tests below intentionally leave a
     * queued-but-undeliverable APC behind while delivery is a TODO. */
    {"sleep_zero_usermode",              t_sleep_zero_usermode},
    {"io_apc_delivered_around_alertable_userwait",
     t_io_apc_breaks_alertable_userwait},
    {"apc_delivered_after_nonalertable_userwait",
     t_nonalertable_userwait_ignores_apc},
    {"critical_region_pairs_are_balanced",
     t_critical_region_pairs_are_balanced},
};

DEFINE_GROUP(ke_wait_apc, "ke/wait-apc");
