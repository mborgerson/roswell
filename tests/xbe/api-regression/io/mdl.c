/*
 * Large single-call transfers through the storage stack, exercising
 * the MDL lock/map paths beneath the IO manager: multi-64 KB
 * unbuffered (sector-direct) and buffered HDD transfers, plus a large
 * DVD read cross-checked against chunked reads of the same extent.
 *
 * Buffers come from NtAllocateVirtualMemory (page-aligned, satisfies
 * the unbuffered-IO alignment requirement) and carry offset-dependent
 * patterns so a page locked, mapped, or copied to the wrong place is
 * caught by content, not just status.
 */

#include "../harness.h"
#include <string.h>

#ifndef FILE_DELETE_ON_CLOSE
#define FILE_DELETE_ON_CLOSE 0x00001000
#endif
#ifndef FILE_NO_INTERMEDIATE_BUFFERING
#define FILE_NO_INTERMEDIATE_BUFFERING 0x00000008
#endif

#define KB(n) ((SIZE_T)(n) * 1024)
#define MB(n) ((SIZE_T)(n) * 1024 * 1024)

static const char MDL_SCRATCH[] =
    "\\Device\\Harddisk0\\Partition1\\nxkrnl-api-mdl.tmp";
static const char DVD_XBE[] = "\\Device\\CdRom0\\default.xbe";

static NTSTATUS open_path(const char *path, HANDLE *h, ACCESS_MASK access,
                          ULONG disposition, ULONG opts,
                          IO_STATUS_BLOCK *iosb)
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

static void unlink_path(const char *path)
{
    HANDLE h;
    IO_STATUS_BLOCK iosb;
    NTSTATUS s = open_path(path, &h, GENERIC_READ, FILE_OPEN,
                           FILE_DELETE_ON_CLOSE, &iosb);
    if (NT_SUCCESS(s)) NtClose(h);
}

static PVOID alloc_buf(SIZE_T size)
{
    PVOID base = NULL;
    SIZE_T sz = size;
    if (NtAllocateVirtualMemory(&base, 0, &sz, MEM_RESERVE | MEM_COMMIT,
                                PAGE_READWRITE) != STATUS_SUCCESS)
        return NULL;
    return base;
}

static void free_buf(PVOID base)
{
    PVOID b = base;
    SIZE_T sz = 0;
    NtFreeVirtualMemory(&b, &sz, MEM_RELEASE);
}

static void fill_pattern(unsigned char *buf, SIZE_T len, ULONG file_off,
                         unsigned char salt)
{
    for (SIZE_T i = 0; i < len; i++) {
        ULONG off = file_off + (ULONG)i;
        buf[i] = (unsigned char)((off * 2654435761u) >> 24) ^ salt;
    }
}

static bool check_pattern(const unsigned char *buf, SIZE_T len, ULONG file_off,
                          unsigned char salt, const char *what)
{
    for (SIZE_T i = 0; i < len; i++) {
        ULONG off = file_off + (ULONG)i;
        unsigned char want = (unsigned char)((off * 2654435761u) >> 24) ^ salt;
        if (buf[i] != want) {
            test_record_failure(__FILE__, __LINE__,
                                "%s: offset %lu: got 0x%02x want 0x%02x",
                                what, (unsigned long)off, buf[i], want);
            return false;
        }
    }
    return true;
}

/* 512 KB sector-direct write and read in single calls: the transfer
 * goes down as one large locked-page run. */
static bool t_unbuffered_512k(void)
{
    HANDLE h = NULL;
    IO_STATUS_BLOCK iosb;
    LARGE_INTEGER zero = { .QuadPart = 0 };
    NTSTATUS s;
    bool ok = true;

    unsigned char *wbuf = alloc_buf(KB(512));
    unsigned char *rbuf = alloc_buf(KB(512));
    if (wbuf == NULL || rbuf == NULL) {
        if (wbuf) free_buf(wbuf);
        if (rbuf) free_buf(rbuf);
        FAIL_AND_RETURN("buffer alloc failed");
    }

    unlink_path(MDL_SCRATCH);
    s = open_path(MDL_SCRATCH, &h, GENERIC_READ | GENERIC_WRITE,
                  FILE_OVERWRITE_IF, FILE_NO_INTERMEDIATE_BUFFERING, &iosb);
    if (s != STATUS_SUCCESS) {
        free_buf(wbuf); free_buf(rbuf);
        FAIL_AND_RETURN("unbuffered create -> 0x%08x", (unsigned)s);
    }

    fill_pattern(wbuf, KB(512), 0, 0x91);
    s = NtWriteFile(h, NULL, NULL, NULL, &iosb, wbuf, KB(512), &zero);
    if (s != STATUS_SUCCESS || iosb.Information != KB(512)) {
        test_record_failure(__FILE__, __LINE__, "write -> 0x%08x len=%u",
                            (unsigned)s, (unsigned)iosb.Information);
        ok = false;
    }

    if (ok) {
        s = NtReadFile(h, NULL, NULL, NULL, &iosb, rbuf, KB(512), &zero);
        if (s != STATUS_SUCCESS || iosb.Information != KB(512)) {
            test_record_failure(__FILE__, __LINE__, "read -> 0x%08x len=%u",
                                (unsigned)s, (unsigned)iosb.Information);
            ok = false;
        }
    }
    if (ok) ok = check_pattern(rbuf, KB(512), 0, 0x91, "unbuffered readback");

    NtClose(h);
    unlink_path(MDL_SCRATCH);
    free_buf(wbuf);
    free_buf(rbuf);
    return ok;
}

/* 1 MB buffered write/read in single calls through the cache. */
static bool t_buffered_1m(void)
{
    HANDLE h = NULL;
    IO_STATUS_BLOCK iosb;
    LARGE_INTEGER zero = { .QuadPart = 0 };
    NTSTATUS s;
    bool ok = true;

    unsigned char *buf = alloc_buf(MB(1));
    if (buf == NULL) FAIL_AND_RETURN("buffer alloc failed");

    unlink_path(MDL_SCRATCH);
    s = open_path(MDL_SCRATCH, &h, GENERIC_READ | GENERIC_WRITE,
                  FILE_OVERWRITE_IF, 0, &iosb);
    if (s != STATUS_SUCCESS) {
        free_buf(buf);
        FAIL_AND_RETURN("create -> 0x%08x", (unsigned)s);
    }

    fill_pattern(buf, MB(1), 0, 0x37);
    s = NtWriteFile(h, NULL, NULL, NULL, &iosb, buf, MB(1), &zero);
    if (s != STATUS_SUCCESS || iosb.Information != MB(1)) {
        test_record_failure(__FILE__, __LINE__, "write -> 0x%08x len=%u",
                            (unsigned)s, (unsigned)iosb.Information);
        ok = false;
    }

    if (ok) {
        s = NtFlushBuffersFile(h, &iosb);
        if (s != STATUS_SUCCESS) {
            test_record_failure(__FILE__, __LINE__, "flush -> 0x%08x",
                                (unsigned)s);
            ok = false;
        }
    }

    if (ok) {
        memset(buf, 0xEE, MB(1));
        s = NtReadFile(h, NULL, NULL, NULL, &iosb, buf, MB(1), &zero);
        if (s != STATUS_SUCCESS || iosb.Information != MB(1)) {
            test_record_failure(__FILE__, __LINE__, "read -> 0x%08x len=%u",
                                (unsigned)s, (unsigned)iosb.Information);
            ok = false;
        }
    }
    if (ok) ok = check_pattern(buf, MB(1), 0, 0x37, "buffered readback");

    NtClose(h);
    unlink_path(MDL_SCRATCH);
    free_buf(buf);
    return ok;
}

/* One large DVD read equals the same extent read in 64 KB chunks: the
 * large transfer's scatter is assembled correctly.  The XBE on the
 * test medium may be shorter than the 256 KB request, so the large
 * read's returned length defines the extent (at least two chunks). */
static bool t_dvd_large_read(void)
{
    HANDLE h = NULL;
    IO_STATUS_BLOCK iosb;
    NTSTATUS s;
    bool ok = true;
    SIZE_T total = 0;

    unsigned char *big = alloc_buf(KB(256));
    unsigned char *small = alloc_buf(KB(256));
    if (big == NULL || small == NULL) {
        if (big) free_buf(big);
        if (small) free_buf(small);
        FAIL_AND_RETURN("buffer alloc failed");
    }

    s = open_path(DVD_XBE, &h, GENERIC_READ, FILE_OPEN, 0, &iosb);
    if (s != STATUS_SUCCESS) {
        free_buf(big); free_buf(small);
        FAIL_AND_RETURN("open DVD xbe -> 0x%08x", (unsigned)s);
    }

    {
        LARGE_INTEGER off = { .QuadPart = 0 };
        s = NtReadFile(h, NULL, NULL, NULL, &iosb, big, KB(256), &off);
        total = iosb.Information;
        if (s != STATUS_SUCCESS || total < KB(128)) {
            test_record_failure(__FILE__, __LINE__,
                                "large read -> 0x%08x len=%u",
                                (unsigned)s, (unsigned)total);
            ok = false;
        }
    }

    for (SIZE_T pos = 0; ok && pos < total; pos += KB(64)) {
        SIZE_T want = total - pos > KB(64) ? KB(64) : total - pos;
        LARGE_INTEGER off = { .QuadPart = pos };
        s = NtReadFile(h, NULL, NULL, NULL, &iosb, small + pos, KB(64), &off);
        if (s != STATUS_SUCCESS || iosb.Information != want) {
            test_record_failure(__FILE__, __LINE__,
                                "chunk read @%lu -> 0x%08x len=%u want=%u",
                                (unsigned long)pos, (unsigned)s,
                                (unsigned)iosb.Information, (unsigned)want);
            ok = false;
        }
    }

    if (ok && memcmp(big, small, total) != 0) {
        SIZE_T at = 0;
        while (at < total && big[at] == small[at]) at++;
        test_record_failure(__FILE__, __LINE__,
                            "large/chunked mismatch at offset %lu",
                            (unsigned long)at);
        ok = false;
    }

    NtClose(h);
    free_buf(big);
    free_buf(small);
    return ok;
}

static const test_entry_t io_mdl_entries[] = {
    {"unbuffered_512k", t_unbuffered_512k},
    {"buffered_1m",     t_buffered_1m},
    {"dvd_large_read",  t_dvd_large_read},
};

DEFINE_GROUP(io_mdl, "io/mdl");
