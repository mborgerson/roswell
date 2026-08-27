/*
 * NtReadFileScatter / NtWriteFileGather: page-granular file IO whose
 * destination is an array of FILE_SEGMENT_ELEMENTs rather than one flat
 * buffer.
 *
 * Every segment is one page, so the pages a transfer lands in need not be
 * adjacent -- which is the whole point of the call and the one thing a
 * flat-buffer implementation gets wrong.  The cases below therefore hand
 * the kernel every other page of one allocation and check that each page
 * gets its own quarter of the file, and that the pages skipped between
 * them are left alone.
 */

#include "../harness.h"
#include <stdio.h>
#include <string.h>

#ifndef FILE_DELETE_ON_CLOSE
#define FILE_DELETE_ON_CLOSE 0x00001000
#endif

#define NPAGES     3                       /* pages in the scratch file */
#define STRIDE     2                       /* segment n = page n*STRIDE */
#define FILE_BYTES (NPAGES * PAGE_SIZE)
#define SKEW       512                     /* deliberate segment misalignment */

/* nxdk declares NtWriteFileGather as returning BOOLEAN, which truncates the
 * status to its low byte -- 0xC0000001 would read as TRUE and STATUS_SUCCESS
 * as FALSE.  Call it through a correctly typed pointer instead. */
typedef NTSTATUS (NTAPI *gather_fn)(HANDLE, HANDLE, PIO_APC_ROUTINE, PVOID,
                                    PIO_STATUS_BLOCK, PFILE_SEGMENT_ELEMENT,
                                    ULONG, PLARGE_INTEGER);

static const char SCATTER_PATH[] =
    "\\Device\\Harddisk0\\Partition1\\nxkrnl-api-scatter.tmp";

static PVOID g_pages;                      /* NPAGES*STRIDE pages, page-aligned */
static bool  g_setup_done;
static bool  g_setup_ok;

static NTSTATUS open_path(const char *path, HANDLE *h, ACCESS_MASK access,
                          ULONG disposition, ULONG opts, IO_STATUS_BLOCK *iosb)
{
    ANSI_STRING name;
    OBJECT_ATTRIBUTES oa;

    name.Length        = (USHORT)strlen(path);
    name.MaximumLength = name.Length + 1;
    name.Buffer        = (PCHAR)path;
    oa.RootDirectory   = NULL;
    oa.ObjectName      = &name;
    oa.Attributes      = OBJ_CASE_INSENSITIVE;

    return NtCreateFile(h, access | SYNCHRONIZE, &oa, iosb, NULL,
                        FILE_ATTRIBUTE_NORMAL, 0, disposition,
                        opts | FILE_SYNCHRONOUS_IO_NONALERT
                             | FILE_NON_DIRECTORY_FILE);
}

static UCHAR page_tag(unsigned page)  { return (UCHAR)(0xA0 + page); }

static PVOID page_at(unsigned page)
{
    return (PVOID)((ULONG_PTR)g_pages + (ULONG_PTR)page * PAGE_SIZE);
}

static void fill_pages(void)
{
    unsigned p;

    for (p = 0; p < NPAGES * STRIDE; p++)
        memset(page_at(p), 0xEE, PAGE_SIZE);
}

/* seg[i] = the i'th strided page; NULL-terminated the way a caller would. */
static void build_segments(FILE_SEGMENT_ELEMENT *seg, unsigned n)
{
    unsigned i;

    for (i = 0; i < n; i++) {
        seg[i].Alignment = 0;
        seg[i].Buffer    = page_at(i * STRIDE);
    }
    seg[n].Alignment = 0;
    seg[n].Buffer    = NULL;
}

static bool page_holds(unsigned page, UCHAR tag)
{
    const UCHAR *p = (const UCHAR *)page_at(page);
    unsigned i;

    for (i = 0; i < PAGE_SIZE; i++)
        if (p[i] != tag)
            return false;
    return true;
}

/* Lay down NPAGES pages, page p filled with page_tag(p), through ordinary
 * buffered writes -- the scatter path is what is under test, not the setup. */
static bool setup(void)
{
    IO_STATUS_BLOCK iosb;
    HANDLE h;
    NTSTATUS s;
    unsigned p;

    if (g_setup_done)
        return g_setup_ok;
    g_setup_done = true;

    g_pages = MmAllocateContiguousMemory(NPAGES * STRIDE * PAGE_SIZE);
    if (g_pages == NULL || ((ULONG_PTR)g_pages & (PAGE_SIZE - 1)) != 0)
        return false;

    s = open_path(SCATTER_PATH, &h, GENERIC_WRITE, FILE_SUPERSEDE, 0, &iosb);
    if (!NT_SUCCESS(s))
        return false;

    for (p = 0; p < NPAGES; p++) {
        memset(page_at(0), page_tag(p), PAGE_SIZE);
        s = NtWriteFile(h, NULL, NULL, NULL, &iosb, page_at(0), PAGE_SIZE,
                        NULL);
        if (!NT_SUCCESS(s) || iosb.Information != PAGE_SIZE) {
            NtClose(h);
            return false;
        }
    }
    NtClose(h);

    g_setup_ok = true;
    return true;
}

#define FAIL_SETUP() FAIL_AND_RETURN("setup failed (pages=%p)", g_pages)

/* Scatter/gather is unbuffered IO, so the handle it runs on is too. */
static NTSTATUS open_unbuffered(HANDLE *h, ACCESS_MASK access,
                                IO_STATUS_BLOCK *iosb)
{
    return open_path(SCATTER_PATH, h, access, FILE_OPEN,
                     FILE_NO_INTERMEDIATE_BUFFERING, iosb);
}


static bool scatter_reads_into_each_page(void)
{
    FILE_SEGMENT_ELEMENT seg[NPAGES + 1];
    IO_STATUS_BLOCK iosb;
    LARGE_INTEGER off;
    HANDLE h;
    NTSTATUS s;
    unsigned p;

    if (!setup())
        FAIL_SETUP();

    s = open_unbuffered(&h, GENERIC_READ, &iosb);
    if (!NT_SUCCESS(s))
        FAIL_AND_RETURN("open unbuffered: 0x%08x", (unsigned)s);

    fill_pages();
    build_segments(seg, NPAGES);
    off.QuadPart = 0;
    memset(&iosb, 0, sizeof(iosb));
    s = NtReadFileScatter(h, NULL, NULL, NULL, &iosb, seg, FILE_BYTES, &off);
    NtClose(h);

    ASSERT_NTSTATUS(s, STATUS_SUCCESS);
    ASSERT_EQ_U32(iosb.Information, FILE_BYTES);

    for (p = 0; p < NPAGES; p++)
        if (!page_holds(p * STRIDE, page_tag(p)))
            FAIL_AND_RETURN("segment %u holds 0x%02x, expected 0x%02x", p,
                            *(const UCHAR *)page_at(p * STRIDE), page_tag(p));

    /* The pages between the segments must not have been written. */
    for (p = 1; p < NPAGES * STRIDE; p += STRIDE)
        if (!page_holds(p, 0xEE))
            FAIL_AND_RETURN("page %u between segments was overwritten", p);
    return true;
}

static bool scatter_reads_from_an_offset(void)
{
    FILE_SEGMENT_ELEMENT seg[NPAGES + 1];
    IO_STATUS_BLOCK iosb;
    LARGE_INTEGER off;
    HANDLE h;
    NTSTATUS s;
    unsigned p;

    if (!setup())
        FAIL_SETUP();

    s = open_unbuffered(&h, GENERIC_READ, &iosb);
    if (!NT_SUCCESS(s))
        FAIL_AND_RETURN("open unbuffered: 0x%08x", (unsigned)s);

    fill_pages();
    build_segments(seg, NPAGES - 1);
    off.QuadPart = PAGE_SIZE;
    memset(&iosb, 0, sizeof(iosb));
    s = NtReadFileScatter(h, NULL, NULL, NULL, &iosb, seg,
                          (NPAGES - 1) * PAGE_SIZE, &off);
    NtClose(h);

    ASSERT_NTSTATUS(s, STATUS_SUCCESS);
    ASSERT_EQ_U32(iosb.Information, (NPAGES - 1) * PAGE_SIZE);

    for (p = 0; p < NPAGES - 1; p++)
        if (!page_holds(p * STRIDE, page_tag(p + 1)))
            FAIL_AND_RETURN("segment %u holds 0x%02x, expected 0x%02x", p,
                            *(const UCHAR *)page_at(p * STRIDE),
                            page_tag(p + 1));
    return true;
}

static bool gather_writes_from_each_page(void)
{
    FILE_SEGMENT_ELEMENT seg[NPAGES + 1];
    IO_STATUS_BLOCK iosb;
    LARGE_INTEGER off;
    HANDLE h;
    NTSTATUS s;
    unsigned p;
    gather_fn gather = (gather_fn)NtWriteFileGather;

    if (!setup())
        FAIL_SETUP();

    fill_pages();
    for (p = 0; p < NPAGES; p++)
        memset(page_at(p * STRIDE), (UCHAR)(0x50 + p), PAGE_SIZE);
    build_segments(seg, NPAGES);

    s = open_unbuffered(&h, GENERIC_WRITE, &iosb);
    if (!NT_SUCCESS(s))
        FAIL_AND_RETURN("open unbuffered: 0x%08x", (unsigned)s);

    off.QuadPart = 0;
    memset(&iosb, 0, sizeof(iosb));
    s = gather(h, NULL, NULL, NULL, &iosb, seg, FILE_BYTES, &off);
    NtClose(h);

    ASSERT_NTSTATUS(s, STATUS_SUCCESS);
    ASSERT_EQ_U32(iosb.Information, FILE_BYTES);

    /* Read it back the ordinary way: each page must carry its own byte. */
    s = open_path(SCATTER_PATH, &h, GENERIC_READ, FILE_OPEN, 0, &iosb);
    if (!NT_SUCCESS(s))
        FAIL_AND_RETURN("reopen buffered: 0x%08x", (unsigned)s);

    for (p = 0; p < NPAGES; p++) {
        memset(page_at(1), 0, PAGE_SIZE);
        s = NtReadFile(h, NULL, NULL, NULL, &iosb, page_at(1), PAGE_SIZE,
                       NULL);
        if (!NT_SUCCESS(s) || iosb.Information != PAGE_SIZE) {
            NtClose(h);
            FAIL_AND_RETURN("readback %u: 0x%08x info=%lu", p, (unsigned)s,
                            (unsigned long)iosb.Information);
        }
        if (!page_holds(1, (UCHAR)(0x50 + p))) {
            NtClose(h);
            FAIL_AND_RETURN("readback %u holds 0x%02x, expected 0x%02x", p,
                            *(const UCHAR *)page_at(1), (UCHAR)(0x50 + p));
        }
    }
    NtClose(h);

    /* Put the file back the way setup() left it for the read cases. */
    g_setup_done = false;
    g_setup_ok = false;
    if (!setup())
        FAIL_SETUP();
    return true;
}

/* An element addresses a page, but Length is a byte count: a sub-page
 * Length transfers exactly that much into the first element, and an element
 * whose low bits are set still lands at the top of its page. */

static bool sub_page_length_transfers_that_much(void)
{
    FILE_SEGMENT_ELEMENT seg[2];
    IO_STATUS_BLOCK iosb;
    LARGE_INTEGER off;
    HANDLE h;
    NTSTATUS s;
    const UCHAR *p;
    unsigned i;

    if (!setup())
        FAIL_SETUP();
    s = open_unbuffered(&h, GENERIC_READ, &iosb);
    if (!NT_SUCCESS(s))
        FAIL_AND_RETURN("open unbuffered: 0x%08x", (unsigned)s);

    fill_pages();
    build_segments(seg, 1);
    off.QuadPart = 0;
    memset(&iosb, 0, sizeof(iosb));
    s = NtReadFileScatter(h, NULL, NULL, NULL, &iosb, seg, PAGE_SIZE / 2,
                          &off);
    NtClose(h);

    ASSERT_NTSTATUS(s, STATUS_SUCCESS);
    ASSERT_EQ_U32(iosb.Information, PAGE_SIZE / 2);

    p = (const UCHAR *)page_at(0);
    for (i = 0; i < PAGE_SIZE; i++) {
        UCHAR want = i < PAGE_SIZE / 2 ? page_tag(0) : 0xEE;
        if (p[i] != want)
            FAIL_AND_RETURN("byte %u is 0x%02x, expected 0x%02x", i, p[i],
                            want);
    }
    return true;
}

static bool segment_low_bits_are_ignored(void)
{
    FILE_SEGMENT_ELEMENT seg[2];
    IO_STATUS_BLOCK iosb;
    LARGE_INTEGER off;
    HANDLE h;
    NTSTATUS s;
    const UCHAR *p;
    unsigned i;

    if (!setup())
        FAIL_SETUP();
    s = open_unbuffered(&h, GENERIC_READ, &iosb);
    if (!NT_SUCCESS(s))
        FAIL_AND_RETURN("open unbuffered: 0x%08x", (unsigned)s);

    fill_pages();
    seg[0].Alignment = 0;
    seg[0].Buffer    = (PVOID)((ULONG_PTR)page_at(0) + SKEW);
    seg[1].Alignment = 0;
    seg[1].Buffer    = NULL;
    off.QuadPart = 0;
    memset(&iosb, 0, sizeof(iosb));
    s = NtReadFileScatter(h, NULL, NULL, NULL, &iosb, seg, PAGE_SIZE, &off);
    NtClose(h);

    ASSERT_NTSTATUS(s, STATUS_SUCCESS);
    ASSERT_EQ_U32(iosb.Information, PAGE_SIZE);

    /* The page the element points into is what gets filled -- from its
     * base, not from the skewed pointer -- and nothing spills past it. */
    p = (const UCHAR *)page_at(0);
    for (i = 0; i < 2 * PAGE_SIZE; i++) {
        UCHAR want = i < PAGE_SIZE ? page_tag(0) : 0xEE;
        if (p[i] != want)
            FAIL_AND_RETURN("byte %u is 0x%02x, expected 0x%02x", i, p[i],
                            want);
    }
    return true;
}

static bool buffered_handle_is_rejected(void)
{
    FILE_SEGMENT_ELEMENT seg[2];
    IO_STATUS_BLOCK iosb;
    LARGE_INTEGER off;
    HANDLE h;
    NTSTATUS s;

    if (!setup())
        FAIL_SETUP();
    s = open_path(SCATTER_PATH, &h, GENERIC_READ, FILE_OPEN, 0, &iosb);
    if (!NT_SUCCESS(s))
        FAIL_AND_RETURN("open buffered: 0x%08x", (unsigned)s);

    fill_pages();
    build_segments(seg, 1);
    off.QuadPart = 0;
    memset(&iosb, 0, sizeof(iosb));
    s = NtReadFileScatter(h, NULL, NULL, NULL, &iosb, seg, PAGE_SIZE, &off);
    NtClose(h);

    ASSERT_NTSTATUS(s, STATUS_INVALID_PARAMETER);
    ASSERT_EQ_U32(iosb.Information, 0);
    if (!page_holds(0, 0xEE))
        FAIL_AND_RETURN("rejected read still touched the segment");
    return true;
}

static bool straddling_eof_is_a_short_read(void)
{
    FILE_SEGMENT_ELEMENT seg[NPAGES + 1];
    IO_STATUS_BLOCK iosb;
    LARGE_INTEGER off;
    HANDLE h;
    NTSTATUS s;
    unsigned p;

    if (!setup())
        FAIL_SETUP();
    s = open_unbuffered(&h, GENERIC_READ, &iosb);
    if (!NT_SUCCESS(s))
        FAIL_AND_RETURN("open unbuffered: 0x%08x", (unsigned)s);

    /* One page of file left, three segments asked for. */
    fill_pages();
    build_segments(seg, NPAGES);
    off.QuadPart = (NPAGES - 1) * PAGE_SIZE;
    memset(&iosb, 0, sizeof(iosb));
    s = NtReadFileScatter(h, NULL, NULL, NULL, &iosb, seg, FILE_BYTES, &off);
    NtClose(h);

    ASSERT_NTSTATUS(s, STATUS_SUCCESS);
    ASSERT_EQ_U32(iosb.Information, PAGE_SIZE);
    if (!page_holds(0, page_tag(NPAGES - 1)))
        FAIL_AND_RETURN("first segment holds 0x%02x, expected 0x%02x",
                        *(const UCHAR *)page_at(0), page_tag(NPAGES - 1));
    for (p = 1; p < NPAGES; p++)
        if (!page_holds(p * STRIDE, 0xEE))
            FAIL_AND_RETURN("segment %u past EOF was written", p);
    return true;
}

static bool read_past_eof_is_end_of_file(void)
{
    FILE_SEGMENT_ELEMENT seg[2];
    IO_STATUS_BLOCK iosb;
    LARGE_INTEGER off;
    HANDLE h;
    NTSTATUS s;

    if (!setup())
        FAIL_SETUP();
    s = open_unbuffered(&h, GENERIC_READ, &iosb);
    if (!NT_SUCCESS(s))
        FAIL_AND_RETURN("open unbuffered: 0x%08x", (unsigned)s);

    fill_pages();
    build_segments(seg, 1);
    off.QuadPart = FILE_BYTES;
    memset(&iosb, 0, sizeof(iosb));
    s = NtReadFileScatter(h, NULL, NULL, NULL, &iosb, seg, PAGE_SIZE, &off);
    NtClose(h);

    ASSERT_NTSTATUS(s, STATUS_END_OF_FILE);
    ASSERT_EQ_U32(iosb.Information, 0);
    if (!page_holds(0, 0xEE))
        FAIL_AND_RETURN("read past EOF still touched the segment");
    return true;
}

/* The console leaves an asynchronous request pending and completes it out
 * of line; completing it inline is just as legal, so what is asserted here
 * is the outcome both share -- event signalled, block filled in, every
 * segment carrying its own page.  The status is reported, not asserted. */
static bool asynchronous_handle_signals_its_event(void)
{
    FILE_SEGMENT_ELEMENT seg[NPAGES + 1];
    IO_STATUS_BLOCK iosb;
    LARGE_INTEGER off, poll;
    ANSI_STRING name;
    OBJECT_ATTRIBUTES oa;
    HANDLE h, ev;
    NTSTATUS s, w;
    unsigned p;

    if (!setup())
        FAIL_SETUP();

    ev = NULL;
    s = NtCreateEvent(&ev, NULL, NotificationEvent, FALSE);
    if (!NT_SUCCESS(s))
        FAIL_AND_RETURN("NtCreateEvent: 0x%08x", (unsigned)s);

    /* No FILE_SYNCHRONOUS_IO_NONALERT: the handle is asynchronous. */
    name.Length        = (USHORT)strlen(SCATTER_PATH);
    name.MaximumLength = name.Length + 1;
    name.Buffer        = (PCHAR)SCATTER_PATH;
    oa.RootDirectory   = NULL;
    oa.ObjectName      = &name;
    oa.Attributes      = OBJ_CASE_INSENSITIVE;
    s = NtCreateFile(&h, GENERIC_READ, &oa, &iosb, NULL,
                     FILE_ATTRIBUTE_NORMAL, 0, FILE_OPEN,
                     FILE_NO_INTERMEDIATE_BUFFERING | FILE_NON_DIRECTORY_FILE);
    if (!NT_SUCCESS(s)) {
        NtClose(ev);
        FAIL_AND_RETURN("open async: 0x%08x", (unsigned)s);
    }

    fill_pages();
    build_segments(seg, NPAGES);
    off.QuadPart = 0;
    memset(&iosb, 0, sizeof(iosb));
    s = NtReadFileScatter(h, ev, NULL, NULL, &iosb, seg, FILE_BYTES, &off);
    w = STATUS_SUCCESS;
    if (s == STATUS_PENDING)
        w = NtWaitForSingleObject(ev, FALSE, NULL);
    NtClose(h);
    tap_comment("async scatter read returned 0x%08x", (unsigned)s);

    if (s != STATUS_PENDING && !NT_SUCCESS(s)) {
        NtClose(ev);
        FAIL_AND_RETURN("async read: 0x%08x", (unsigned)s);
    }
    ASSERT_NTSTATUS(w, STATUS_SUCCESS);

    /* Signalled either way: a poll finds it set even when the call
     * completed inline and there was nothing to wait for. */
    poll.QuadPart = 0;
    w = NtWaitForSingleObject(ev, FALSE, &poll);
    NtClose(ev);
    ASSERT_NTSTATUS(w, STATUS_SUCCESS);

    ASSERT_NTSTATUS(iosb.Status, STATUS_SUCCESS);
    ASSERT_EQ_U32(iosb.Information, FILE_BYTES);
    for (p = 0; p < NPAGES; p++)
        if (!page_holds(p * STRIDE, page_tag(p)))
            FAIL_AND_RETURN("segment %u holds 0x%02x, expected 0x%02x", p,
                            *(const UCHAR *)page_at(p * STRIDE), page_tag(p));
    return true;
}

/* A completion APC is delivered once, after the whole array has moved: the
 * block it is handed carries the total, not the last element's share.  The
 * console delivers it at the next alertable wait; delivering it inline is
 * equally legal, so the wait below is a chance to run it, not an assertion
 * that it had not run already. */

static ULONG g_apc_calls;
static PVOID g_apc_context;
static ULONG g_apc_information;
static NTSTATUS g_apc_status;

static VOID NTAPI scatter_apc(PVOID ApcContext, PIO_STATUS_BLOCK Iosb,
                              ULONG Reserved)
{
    (void)Reserved;
    g_apc_calls++;
    g_apc_context = ApcContext;
    g_apc_information = Iosb != NULL ? (ULONG)Iosb->Information : 0;
    g_apc_status = Iosb != NULL ? Iosb->Status : STATUS_UNSUCCESSFUL;
}

static bool completion_apc_reports_the_whole_transfer(void)
{
    static int marker;
    FILE_SEGMENT_ELEMENT seg[NPAGES + 1];
    IO_STATUS_BLOCK iosb;
    LARGE_INTEGER off, wait;
    HANDLE h, ev;
    NTSTATUS s, w;

    if (!setup())
        FAIL_SETUP();

    ev = NULL;
    s = NtCreateEvent(&ev, NULL, NotificationEvent, FALSE);
    if (!NT_SUCCESS(s))
        FAIL_AND_RETURN("NtCreateEvent: 0x%08x", (unsigned)s);

    s = open_unbuffered(&h, GENERIC_READ, &iosb);
    if (!NT_SUCCESS(s)) {
        NtClose(ev);
        FAIL_AND_RETURN("open unbuffered: 0x%08x", (unsigned)s);
    }

    g_apc_calls = 0;
    g_apc_context = NULL;
    g_apc_information = 0;
    g_apc_status = STATUS_UNSUCCESSFUL;

    fill_pages();
    build_segments(seg, NPAGES);
    off.QuadPart = 0;
    memset(&iosb, 0, sizeof(iosb));
    s = NtReadFileScatter(h, NULL, scatter_apc, &marker, &iosb, seg,
                          FILE_BYTES, &off);
    if (s != STATUS_PENDING && !NT_SUCCESS(s)) {
        NtClose(h);
        NtClose(ev);
        FAIL_AND_RETURN("scatter read: 0x%08x", (unsigned)s);
    }

    /* Alertable, so a queued APC has somewhere to run. */
    wait.QuadPart = -1000000;                  /* 100 ms, relative */
    w = NtWaitForSingleObjectEx(ev, UserMode, TRUE, &wait);
    NtClose(h);
    NtClose(ev);
    tap_comment("apc scatter read: status=0x%08x wait=0x%08x calls=%lu",
                (unsigned)s, (unsigned)w, (unsigned long)g_apc_calls);

    ASSERT_EQ_U32(g_apc_calls, 1);
    ASSERT_EQ_PTR(g_apc_context, &marker);
    ASSERT_NTSTATUS(g_apc_status, STATUS_SUCCESS);
    ASSERT_EQ_U32(g_apc_information, FILE_BYTES);
    return true;
}

static bool leaves_no_scratch_file(void)
{
    IO_STATUS_BLOCK iosb;
    HANDLE h;
    NTSTATUS s;

    s = open_path(SCATTER_PATH, &h, GENERIC_READ, FILE_OPEN,
                  FILE_DELETE_ON_CLOSE, &iosb);
    if (!NT_SUCCESS(s))
        FAIL_AND_RETURN("unlink open: 0x%08x", (unsigned)s);
    NtClose(h);

    s = open_path(SCATTER_PATH, &h, GENERIC_READ, FILE_OPEN, 0, &iosb);
    if (NT_SUCCESS(s)) {
        NtClose(h);
        FAIL_AND_RETURN("scratch file still present");
    }
    return true;
}

static const test_entry_t io_scatter_entries[] = {
    { "scatter_reads_into_each_page", scatter_reads_into_each_page, NULL },
    { "scatter_reads_from_an_offset", scatter_reads_from_an_offset, NULL },
    { "gather_writes_from_each_page", gather_writes_from_each_page, NULL },
    { "sub_page_length_transfers_that_much",
      sub_page_length_transfers_that_much, NULL },
    { "segment_low_bits_are_ignored", segment_low_bits_are_ignored, NULL },
    { "buffered_handle_is_rejected",  buffered_handle_is_rejected,  NULL },
    { "straddling_eof_is_a_short_read",
      straddling_eof_is_a_short_read, NULL },
    { "read_past_eof_is_end_of_file", read_past_eof_is_end_of_file, NULL },
    { "asynchronous_handle_signals_its_event",
      asynchronous_handle_signals_its_event, NULL },
    { "completion_apc_reports_the_whole_transfer",
      completion_apc_reports_the_whole_transfer, NULL },
    { "leaves_no_scratch_file",       leaves_no_scratch_file,       NULL },
};

DEFINE_GROUP(io_scatter, "io/scatter");
