/*
 * Memory-pressure profile shaped like a real title's: a large
 * constrained-low contiguous pin held while anonymous VM ramps up and
 * file IO runs, release-and-repin, and clean recovery from genuine
 * exhaustion.  A typical title pins ~25 MB of contiguous memory low in
 * the physical space at boot and then commits tens of MB through XAPI
 * heaps while streaming from disk.
 *
 * The contiguous pin asks for the exact low window first (lowest
 * acceptable PA 0x61000) and falls back to lowest=0x61000 with an open
 * ceiling; which one succeeded is recorded with a TAP comment so the
 * retail baseline pins the stronger claim.
 *
 * Runs on 64 and 128 MB configurations: the ramp sizes itself from
 * MmQueryStatistics.AvailablePages and asserts a floor instead of a
 * fixed total.
 */

#include "../harness.h"
#include <stdio.h>
#include <string.h>

#ifndef STATUS_NO_MEMORY
#define STATUS_NO_MEMORY ((NTSTATUS)0xC0000017L)
#endif
#ifndef FILE_DELETE_ON_CLOSE
#define FILE_DELETE_ON_CLOSE 0x00001000
#endif

#define KB(n) ((SIZE_T)(n) * 1024)
#define MB(n) ((SIZE_T)(n) * 1024 * 1024)

#define PIN_LOW_PA   0x61000UL
#define PIN_BYTES    MB(25)
#define RAMP_MAX     40u            /* MB */
#define RAMP_FLOOR   16u            /* MB */
#define RAMP_HEADROOM MB(12)        /* left for kernel/cache/file IO */
#define EXHAUST_CAP  256u

static PVOID g_chunks[EXHAUST_CAP];

static const char PRESSURE_SCRATCH[] =
    "\\Device\\Harddisk0\\Partition1\\nxkrnl-api-pressure.tmp";

/* Constrained-low pin: a 25 MB allocation constrained to the exact
 * window starting at PA 0x61000 -- one legal placement, so success
 * means the base PA is 0x61000.  Retail-validated: every pin in this
 * file lands there. */
static PVOID pin_constrained(const char *who)
{
    PVOID p = MmAllocateContiguousMemoryEx(PIN_BYTES, PIN_LOW_PA,
                                           PIN_LOW_PA + PIN_BYTES - 1, 0,
                                           PAGE_READWRITE);
    if (p != NULL)
        tap_comment("pressure(%s): pin at PA 0x%08lx", who,
                    (unsigned long)MmGetPhysicalAddress(p));
    else {
        /* Diagnose: which MBs of the window are busy?  (Failure path
         * only; the release kernel's own prints don't reach serial.) */
        MM_STATISTICS st = { .Length = sizeof(MM_STATISTICS) };
        if (MmQueryStatistics(&st) == STATUS_SUCCESS)
            tap_comment("pressure(%s): pin FAILED, avail=%lu pages", who,
                        (unsigned long)st.AvailablePages);
        for (SIZE_T off = 0; off < PIN_BYTES; off += MB(1)) {
            PVOID q = MmAllocateContiguousMemoryEx(MB(1), PIN_LOW_PA + off,
                                                   PIN_LOW_PA + off + MB(1) - 1,
                                                   0, PAGE_READWRITE);
            if (q == NULL)
                tap_comment("pressure(%s): busy MB at +0x%08lx", who,
                            (unsigned long)off);
            else
                MmFreeContiguousMemory(q);
        }
    }
    return p;
}

static void fill_pages(unsigned char *p, SIZE_T bytes, unsigned char salt)
{
    for (SIZE_T off = 0; off < bytes; off += 4096) {
        ULONG *w = (ULONG *)(p + off);
        w[0] = 0x4E584B00u | salt;
        w[1] = (ULONG)off;
        w[1023] = 0x4E584BFFu ^ salt;
    }
}

static bool check_pages(const unsigned char *p, SIZE_T bytes,
                        unsigned char salt, const char *what)
{
    for (SIZE_T off = 0; off < bytes; off += 4096) {
        const ULONG *w = (const ULONG *)(p + off);
        if (w[0] != (0x4E584B00u | salt) || w[1] != (ULONG)off ||
            w[1023] != (0x4E584BFFu ^ salt)) {
            test_record_failure(__FILE__, __LINE__,
                                "%s: page +0x%lx corrupt (%08lx %08lx %08lx)",
                                what, (unsigned long)off,
                                (unsigned long)w[0], (unsigned long)w[1],
                                (unsigned long)w[1023]);
            return false;
        }
    }
    return true;
}

static void free_chunks(unsigned int n)
{
    for (unsigned int i = 0; i < n; i++) {
        if (g_chunks[i] != NULL) {
            PVOID b = g_chunks[i];
            SIZE_T sz = 0;
            NtFreeVirtualMemory(&b, &sz, MEM_RELEASE);
            g_chunks[i] = NULL;
        }
    }
}

/* Size the ramp from what's actually available right now. */
static unsigned int ramp_target_mb(void)
{
    MM_STATISTICS st = { .Length = sizeof(MM_STATISTICS) };
    if (MmQueryStatistics(&st) != STATUS_SUCCESS) return 0;
    SIZE_T avail = (SIZE_T)st.AvailablePages * 4096;
    if (avail <= RAMP_HEADROOM) return 0;
    SIZE_T budget = (avail - RAMP_HEADROOM) / MB(1);
    return budget > RAMP_MAX ? RAMP_MAX : (unsigned int)budget;
}

/* 25 MB constrained pin + 1 MB-step commit ramp; every page of both
 * stays intact through the whole climb. */
static bool t_commit_ramp_under_pin(void)
{
    PVOID pin = pin_constrained("ramp");
    ASSERT_NOT_NULL(pin);
    fill_pages(pin, PIN_BYTES, 0xEE);

    unsigned int target = ramp_target_mb();
    unsigned int got = 0;
    bool ok = true;

    for (; got < target; got++) {
        PVOID b = NULL;
        SIZE_T sz = MB(1);
        NTSTATUS s = NtAllocateVirtualMemory(&b, 0, &sz,
                                             MEM_RESERVE | MEM_COMMIT,
                                             PAGE_READWRITE);
        if (s != STATUS_SUCCESS) break;
        g_chunks[got] = b;
        fill_pages(b, MB(1), (unsigned char)got);
    }

    if (got < RAMP_FLOOR) {
        test_record_failure(__FILE__, __LINE__,
                            "ramp stalled at %u MB (target %u, floor %u)",
                            got, target, RAMP_FLOOR);
        ok = false;
    }

    for (unsigned int i = 0; ok && i < got; i++) {
        char what[32];
        snprintf(what, sizeof(what), "ramp chunk %u", i);
        ok = check_pages(g_chunks[i], MB(1), (unsigned char)i, what);
    }
    if (ok) ok = check_pages(pin, PIN_BYTES, 0xEE, "pin under ramp");

    free_chunks(got);
    MmFreeContiguousMemory(pin);
    return ok;
}

/* File writes bigger than any plausible cache interleaved into the
 * commit ramp; both the file content and the VM pages must survive. */
static bool t_ramp_with_file_io(void)
{
    HANDLE h = NULL;
    IO_STATUS_BLOCK iosb;
    ANSI_STRING name;
    OBJECT_ATTRIBUTES oa;
    NTSTATUS s;
    bool ok = true;
    unsigned int got = 0;
    static unsigned char filebuf[KB(64)];

    PVOID pin = pin_constrained("file-io");
    ASSERT_NOT_NULL(pin);
    fill_pages(pin, PIN_BYTES, 0xBA);

    name.Length        = (USHORT)strlen(PRESSURE_SCRATCH);
    name.MaximumLength = name.Length + 1;
    name.Buffer        = (PCHAR)PRESSURE_SCRATCH;
    oa.RootDirectory   = NULL;
    oa.ObjectName      = &name;
    oa.Attributes      = OBJ_CASE_INSENSITIVE;
    s = NtCreateFile(&h, GENERIC_READ | GENERIC_WRITE | SYNCHRONIZE, &oa,
                     &iosb, NULL, FILE_ATTRIBUTE_NORMAL, 0,
                     FILE_OVERWRITE_IF,
                     FILE_SYNCHRONOUS_IO_NONALERT | FILE_NON_DIRECTORY_FILE);
    if (s != STATUS_SUCCESS) {
        MmFreeContiguousMemory(pin);
        FAIL_AND_RETURN("create scratch -> 0x%08x", (unsigned)s);
    }

    /* 8 MB ramp, one 64 KB pattern write per committed MB (8 MB file
     * by the end -- far beyond the boot-time file cache). */
    unsigned int target = ramp_target_mb();
    if (target > 8) target = 8;
    ULONG file_off = 0;

    for (; got < target && ok; got++) {
        PVOID b = NULL;
        SIZE_T sz = MB(1);
        s = NtAllocateVirtualMemory(&b, 0, &sz, MEM_RESERVE | MEM_COMMIT,
                                    PAGE_READWRITE);
        if (s != STATUS_SUCCESS) break;
        g_chunks[got] = b;
        fill_pages(b, MB(1), (unsigned char)(0x40 + got));

        for (unsigned int k = 0; k < 16 && ok; k++) {
            for (ULONG i = 0; i < sizeof(filebuf); i++)
                filebuf[i] = (unsigned char)(((file_off + i) * 2654435761u) >> 24);
            s = NtWriteFile(h, NULL, NULL, NULL, &iosb, filebuf,
                            sizeof(filebuf), NULL);
            if (s != STATUS_SUCCESS || iosb.Information != sizeof(filebuf)) {
                test_record_failure(__FILE__, __LINE__,
                                    "write @%lu -> 0x%08x len=%u",
                                    (unsigned long)file_off, (unsigned)s,
                                    (unsigned)iosb.Information);
                ok = false;
            }
            file_off += sizeof(filebuf);
        }
    }

    if (ok && got < 4) {
        test_record_failure(__FILE__, __LINE__,
                            "interleaved ramp stalled at %u MB", got);
        ok = false;
    }

    /* Read the whole file back and verify the offset pattern. */
    if (ok) {
        LARGE_INTEGER off = { .QuadPart = 0 };
        for (ULONG pos = 0; pos < file_off && ok; pos += sizeof(filebuf)) {
            off.QuadPart = pos;
            s = NtReadFile(h, NULL, NULL, NULL, &iosb, filebuf,
                           sizeof(filebuf), &off);
            if (s != STATUS_SUCCESS || iosb.Information != sizeof(filebuf)) {
                test_record_failure(__FILE__, __LINE__,
                                    "read @%lu -> 0x%08x len=%u",
                                    (unsigned long)pos, (unsigned)s,
                                    (unsigned)iosb.Information);
                ok = false;
                break;
            }
            for (ULONG i = 0; i < sizeof(filebuf); i++) {
                unsigned char want =
                    (unsigned char)(((pos + i) * 2654435761u) >> 24);
                if (filebuf[i] != want) {
                    test_record_failure(__FILE__, __LINE__,
                                        "file offset %lu: got 0x%02x want 0x%02x",
                                        (unsigned long)(pos + i), filebuf[i],
                                        want);
                    ok = false;
                    break;
                }
            }
        }
    }

    for (unsigned int i = 0; ok && i < got; i++) {
        char what[32];
        snprintf(what, sizeof(what), "interleave chunk %u", i);
        ok = check_pages(g_chunks[i], MB(1), (unsigned char)(0x40 + i), what);
    }
    if (ok) ok = check_pages(pin, PIN_BYTES, 0xBA, "pin under file IO");

    NtClose(h);
    /* Unlink the scratch file. */
    {
        HANDLE hd;
        s = NtCreateFile(&hd, GENERIC_READ | SYNCHRONIZE, &oa, &iosb, NULL,
                         FILE_ATTRIBUTE_NORMAL, 0, FILE_OPEN,
                         FILE_SYNCHRONOUS_IO_NONALERT |
                         FILE_NON_DIRECTORY_FILE | FILE_DELETE_ON_CLOSE);
        if (NT_SUCCESS(s)) NtClose(hd);
    }
    free_chunks(got);
    MmFreeContiguousMemory(pin);
    return ok;
}

/* Release everything, then re-pin the same constrained window: heavy
 * use must not strand the low anchor. */
static bool t_repin_after_release(void)
{
    PVOID pin = pin_constrained("first");
    ASSERT_NOT_NULL(pin);
    ULONG_PTR pa1 = MmGetPhysicalAddress(pin);

    /* Churn: commit and touch 8 MB while the pin is held. */
    unsigned int got = 0;
    for (; got < 8; got++) {
        PVOID b = NULL;
        SIZE_T sz = MB(1);
        if (NtAllocateVirtualMemory(&b, 0, &sz, MEM_RESERVE | MEM_COMMIT,
                                    PAGE_READWRITE) != STATUS_SUCCESS) break;
        g_chunks[got] = b;
        memset(b, 0x66, MB(1));
    }

    free_chunks(got);
    MmFreeContiguousMemory(pin);

    pin = pin_constrained("repin");
    ASSERT_NOT_NULL(pin);
    ULONG_PTR pa2 = MmGetPhysicalAddress(pin);
    tap_comment("pressure: repin PA 0x%08lx (first 0x%08lx)",
                (unsigned long)pa2, (unsigned long)pa1);

    fill_pages(pin, PIN_BYTES, 0x5C);
    bool ok = check_pages(pin, PIN_BYTES, 0x5C, "repinned block");
    MmFreeContiguousMemory(pin);
    return ok;
}

/* Commit+touch to genuine exhaustion: the failing call returns
 * STATUS_NO_MEMORY (no bugcheck, no hang), and after release a fresh
 * commit succeeds.  The climb runs on a worker thread with a bounded
 * wait so a kernel that hangs at exhaustion instead of failing the
 * call costs this one test, not the whole suite (the wedged worker
 * and its memory are deliberately leaked; this runs last). */

typedef struct {
    KEVENT done;
    unsigned int got;
    NTSTATUS fail_status;
    BOOLEAN verify_ok;
    BOOLEAN recovered;
} exhaust_ctx_t;

static void NTAPI exhaust_system_routine(PKSTART_ROUTINE StartRoutine,
                                         PVOID StartContext)
{
    if (StartRoutine != NULL)
        StartRoutine(StartContext);
    PsTerminateSystemThread(STATUS_SUCCESS);
}

static void NTAPI exhaust_worker(PVOID arg)
{
    exhaust_ctx_t *ctx = (exhaust_ctx_t *)arg;
    unsigned int got = 0;

    for (; got < EXHAUST_CAP; got++) {
        PVOID b = NULL;
        SIZE_T sz = MB(1);
        NTSTATUS s = NtAllocateVirtualMemory(&b, 0, &sz,
                                             MEM_RESERVE | MEM_COMMIT,
                                             PAGE_READWRITE);
        if (s != STATUS_SUCCESS) {
            ctx->fail_status = s;
            break;
        }
        g_chunks[got] = b;
        fill_pages(b, MB(1), (unsigned char)(got ^ 0xA7));
    }
    ctx->got = got;

    /* Verify the climb survived intact, then release it. */
    ctx->verify_ok = TRUE;
    for (unsigned int i = 0; ctx->verify_ok && i < got; i += 7) {
        ctx->verify_ok = check_pages(g_chunks[i], MB(1),
                                     (unsigned char)(i ^ 0xA7),
                                     "exhaust chunk");
    }
    free_chunks(got);

    /* Recovery: a fresh 4 MB commit works again. */
    if (ctx->verify_ok && ctx->fail_status == STATUS_NO_MEMORY) {
        PVOID b = NULL;
        SIZE_T sz = MB(4);
        if (NtAllocateVirtualMemory(&b, 0, &sz, MEM_RESERVE | MEM_COMMIT,
                                    PAGE_READWRITE) == STATUS_SUCCESS) {
            memset(b, 0x11, MB(4));
            PVOID bb = b;
            SIZE_T zero = 0;
            NtFreeVirtualMemory(&bb, &zero, MEM_RELEASE);
            ctx->recovered = TRUE;
        }
    }

    KeSetEvent(&ctx->done, 0, FALSE);
}

static bool t_exhaustion_recovers(void)
{
    static exhaust_ctx_t ctx;  /* outlives a wedged worker */
    HANDLE h = NULL;
    NTSTATUS s;

    /* The climb is only survivable on a kernel that charges commit:
     * without the charge, every commit succeeds and the first touch
     * past the last page dies in an unrecoverable in-page fault.
     * Probe the charge before going anywhere near the edge. */
    {
        MM_STATISTICS pre = { .Length = sizeof(MM_STATISTICS) };
        MM_STATISTICS post = { .Length = sizeof(MM_STATISTICS) };
        PVOID b = NULL;
        SIZE_T sz = MB(1);

        ASSERT_NTSTATUS(MmQueryStatistics(&pre), STATUS_SUCCESS);
        s = NtAllocateVirtualMemory(&b, 0, &sz, MEM_RESERVE | MEM_COMMIT,
                                    PAGE_READWRITE);
        ASSERT_NTSTATUS(s, STATUS_SUCCESS);
        ASSERT_NTSTATUS(MmQueryStatistics(&post), STATUS_SUCCESS);
        {
            PVOID bb = b;
            SIZE_T zero = 0;
            NtFreeVirtualMemory(&bb, &zero, MEM_RELEASE);
        }
        if (post.VirtualMemoryBytesCommitted ==
            pre.VirtualMemoryBytesCommitted) {
            FAIL_AND_RETURN("commit is not charged; touch-to-exhaustion "
                            "would bugcheck, skipping the climb");
        }
    }

    KeInitializeEvent(&ctx.done, NotificationEvent, FALSE);
    ctx.got = 0;
    ctx.fail_status = STATUS_SUCCESS;
    ctx.verify_ok = FALSE;
    ctx.recovered = FALSE;

    s = PsCreateSystemThreadEx(&h, 0, 64 * 1024, 0, NULL, exhaust_worker,
                               &ctx, FALSE, FALSE, exhaust_system_routine);
    ASSERT_NTSTATUS(s, STATUS_SUCCESS);

    LARGE_INTEGER timeout = { .QuadPart = -((LONGLONG)45 * 1000 * 10000) };
    s = KeWaitForSingleObject(&ctx.done, Executive, KernelMode, FALSE,
                              &timeout);
    NtClose(h);

    if (s != STATUS_SUCCESS) {
        test_record_failure(__FILE__, __LINE__,
                            "exhaustion hung: no failing status after %u MB "
                            "in 45 s", ctx.got);
        return false;
    }

    tap_comment("pressure: exhaustion after %u MB -> 0x%08x", ctx.got,
                (unsigned)ctx.fail_status);

    if (ctx.got == EXHAUST_CAP)
        FAIL_AND_RETURN("no exhaustion after %u MB", EXHAUST_CAP);
    if (!ctx.verify_ok)
        return false;
    if (ctx.fail_status != STATUS_NO_MEMORY) {
        test_record_failure(__FILE__, __LINE__,
                            "exhaustion status 0x%08x, want STATUS_NO_MEMORY",
                            (unsigned)ctx.fail_status);
        return false;
    }
    if (!ctx.recovered)
        FAIL_AND_RETURN("post-exhaustion commit failed");
    return true;
}

static const test_entry_t mm_pressure_entries[] = {
    {"commit_ramp_under_pin", t_commit_ramp_under_pin},
    {"ramp_with_file_io",     t_ramp_with_file_io},
    {"repin_after_release",   t_repin_after_release},
    {"exhaustion_recovers",   t_exhaustion_recovers},
};

DEFINE_GROUP(mm_pressure, "mm/pressure");
