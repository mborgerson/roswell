/*
 * File-cache behavior contract beyond io/cc-write's flush/coherence
 * coverage: eviction correctness when the working set exceeds the
 * cache, and truncate/re-extend purge semantics.
 *
 * The boot-time file cache is small and fixed; writing or reading a
 * few MB guarantees eviction.  Content is always pattern-verified so a
 * buffer recycled to the wrong file/offset is caught.  Gap bytes after
 * an EOF extension are never asserted (retail exposes stale cluster
 * content there -- see io/cc_write.c).
 *
 * CachePagesCommitted is recorded around the heavy test as the
 * baseline observation for sizing a replacement cache.
 */

#include "../harness.h"
#include <stdio.h>
#include <string.h>

#ifndef FILE_DELETE_ON_CLOSE
#define FILE_DELETE_ON_CLOSE 0x00001000
#endif
#ifndef STATUS_END_OF_FILE
#define STATUS_END_OF_FILE ((NTSTATUS)0xC0000011L)
#endif

#define KB(n) ((SIZE_T)(n) * 1024)
#define MB(n) ((SIZE_T)(n) * 1024 * 1024)

#define CHUNK 0x10000u /* 64 KB */

static unsigned char g_buf[CHUNK];

static const char CACHE_A[] =
    "\\Device\\Harddisk0\\Partition1\\nxkrnl-api-cache-a.tmp";
static const char CACHE_B[] =
    "\\Device\\Harddisk0\\Partition1\\nxkrnl-api-cache-b.tmp";

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

static void fill_pattern(unsigned char *buf, ULONG len, ULONG file_off,
                         unsigned char salt)
{
    for (ULONG i = 0; i < len; i++) {
        ULONG off = file_off + i;
        buf[i] = (unsigned char)((off * 2654435761u) >> 24) ^ salt;
    }
}

static bool check_pattern(const unsigned char *buf, ULONG len, ULONG file_off,
                          unsigned char salt, const char *what)
{
    for (ULONG i = 0; i < len; i++) {
        ULONG off = file_off + i;
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

static bool write_at(HANDLE h, ULONG pos, ULONG len, unsigned char salt)
{
    IO_STATUS_BLOCK iosb;
    LARGE_INTEGER off = { .QuadPart = pos };
    fill_pattern(g_buf, len, pos, salt);
    NTSTATUS s = NtWriteFile(h, NULL, NULL, NULL, &iosb, g_buf, len, &off);
    if (s != STATUS_SUCCESS || iosb.Information != len) {
        test_record_failure(__FILE__, __LINE__, "write @%lu -> 0x%08x len=%u",
                            (unsigned long)pos, (unsigned)s,
                            (unsigned)iosb.Information);
        return false;
    }
    return true;
}

static bool read_check_at(HANDLE h, ULONG pos, ULONG len, unsigned char salt,
                          const char *what)
{
    IO_STATUS_BLOCK iosb;
    LARGE_INTEGER off = { .QuadPart = pos };
    memset(g_buf, 0xEE, len);
    NTSTATUS s = NtReadFile(h, NULL, NULL, NULL, &iosb, g_buf, len, &off);
    if (s != STATUS_SUCCESS || iosb.Information != len) {
        test_record_failure(__FILE__, __LINE__, "%s: read @%lu -> 0x%08x len=%u",
                            what, (unsigned long)pos, (unsigned)s,
                            (unsigned)iosb.Information);
        return false;
    }
    return check_pattern(g_buf, len, pos, salt, what);
}

/* Two files written in alternating 64 KB chunks, 2 MB each: with a
 * fixed small cache every buffer is evicted and recycled across files
 * mid-test, and every byte must still come back right after reopen. */
static bool t_eviction_two_files(void)
{
    HANDLE ha = NULL, hb = NULL;
    IO_STATUS_BLOCK iosb;
    NTSTATUS s;
    bool ok = true;
    MM_STATISTICS st = { .Length = sizeof(MM_STATISTICS) };

    if (MmQueryStatistics(&st) == STATUS_SUCCESS)
        tap_comment("cache: CachePagesCommitted before heavy IO = %lu",
                    (unsigned long)st.CachePagesCommitted);

    unlink_path(CACHE_A);
    unlink_path(CACHE_B);
    s = open_path(CACHE_A, &ha, GENERIC_READ | GENERIC_WRITE,
                  FILE_OVERWRITE_IF, 0, &iosb);
    ASSERT_NTSTATUS(s, STATUS_SUCCESS);
    s = open_path(CACHE_B, &hb, GENERIC_READ | GENERIC_WRITE,
                  FILE_OVERWRITE_IF, 0, &iosb);
    if (s != STATUS_SUCCESS) {
        NtClose(ha);
        unlink_path(CACHE_A);
        FAIL_AND_RETURN("create B -> 0x%08x", (unsigned)s);
    }

    for (ULONG pos = 0; pos < MB(2) && ok; pos += CHUNK) {
        ok = write_at(ha, pos, CHUNK, 0xA1) && write_at(hb, pos, CHUNK, 0xB2);
    }

    NtClose(ha);
    NtClose(hb);

    if (MmQueryStatistics(&st) == STATUS_SUCCESS)
        tap_comment("cache: CachePagesCommitted after heavy IO = %lu",
                    (unsigned long)st.CachePagesCommitted);

    if (ok) {
        s = open_path(CACHE_A, &ha, GENERIC_READ, FILE_OPEN, 0, &iosb);
        if (s != STATUS_SUCCESS) {
            unlink_path(CACHE_A); unlink_path(CACHE_B);
            FAIL_AND_RETURN("reopen A -> 0x%08x", (unsigned)s);
        }
        s = open_path(CACHE_B, &hb, GENERIC_READ, FILE_OPEN, 0, &iosb);
        if (s != STATUS_SUCCESS) {
            NtClose(ha);
            unlink_path(CACHE_A); unlink_path(CACHE_B);
            FAIL_AND_RETURN("reopen B -> 0x%08x", (unsigned)s);
        }
        for (ULONG pos = 0; pos < MB(2) && ok; pos += CHUNK) {
            ok = read_check_at(ha, pos, CHUNK, 0xA1, "file A") &&
                 read_check_at(hb, pos, CHUNK, 0xB2, "file B");
        }
        NtClose(ha);
        NtClose(hb);
    }

    unlink_path(CACHE_A);
    unlink_path(CACHE_B);
    return ok;
}

/* Clean-buffer eviction on the read path: a 4 MB flushed file read
 * far-offset-first twice over; every chunk re-fetched correctly. */
static bool t_read_churn(void)
{
    HANDLE h = NULL;
    IO_STATUS_BLOCK iosb;
    NTSTATUS s;
    bool ok = true;

    unlink_path(CACHE_A);
    s = open_path(CACHE_A, &h, GENERIC_READ | GENERIC_WRITE,
                  FILE_OVERWRITE_IF, 0, &iosb);
    ASSERT_NTSTATUS(s, STATUS_SUCCESS);

    for (ULONG pos = 0; pos < MB(4) && ok; pos += CHUNK)
        ok = write_at(h, pos, CHUNK, 0xC3);
    if (ok) {
        s = NtFlushBuffersFile(h, &iosb);
        if (s != STATUS_SUCCESS) {
            test_record_failure(__FILE__, __LINE__, "flush -> 0x%08x",
                                (unsigned)s);
            ok = false;
        }
    }

    /* Pass 1: front/back alternation; pass 2: sequential re-read. */
    for (ULONG i = 0; ok && i < MB(4) / CHUNK / 2; i++) {
        ULONG lo = i * CHUNK;
        ULONG hi = (ULONG)MB(4) - (i + 1) * CHUNK;
        ok = read_check_at(h, lo, CHUNK, 0xC3, "churn lo") &&
             read_check_at(h, hi, CHUNK, 0xC3, "churn hi");
    }
    for (ULONG pos = 0; ok && pos < MB(4); pos += CHUNK)
        ok = read_check_at(h, pos, CHUNK, 0xC3, "churn seq");

    NtClose(h);
    unlink_path(CACHE_A);
    return ok;
}

/* Shrinking EOF takes immediately: size reports, reads past EOF fail,
 * the surviving prefix is intact. */
static bool t_truncate_shrinks(void)
{
    HANDLE h = NULL;
    IO_STATUS_BLOCK iosb;
    FILE_END_OF_FILE_INFORMATION eof;
    FILE_NETWORK_OPEN_INFORMATION info;
    NTSTATUS s;
    bool ok = true;

    unlink_path(CACHE_A);
    s = open_path(CACHE_A, &h, GENERIC_READ | GENERIC_WRITE,
                  FILE_OVERWRITE_IF, 0, &iosb);
    ASSERT_NTSTATUS(s, STATUS_SUCCESS);

    for (ULONG pos = 0; pos < KB(256) && ok; pos += CHUNK)
        ok = write_at(h, pos, CHUNK, 0xD4);

    if (ok) {
        eof.EndOfFile.QuadPart = KB(64);
        s = NtSetInformationFile(h, &iosb, &eof, sizeof(eof),
                                 FileEndOfFileInformation);
        if (s != STATUS_SUCCESS) {
            test_record_failure(__FILE__, __LINE__, "shrink EOF -> 0x%08x",
                                (unsigned)s);
            ok = false;
        }
    }

    /* Retail-validated: FileStandardInformation is rejected here;
     * FileNetworkOpenInformation is the size-query class that works. */
    if (ok) {
        memset(&info, 0, sizeof(info));
        s = NtQueryInformationFile(h, &iosb, &info, sizeof(info),
                                   FileNetworkOpenInformation);
        if (s != STATUS_SUCCESS || info.EndOfFile.QuadPart != (LONGLONG)KB(64)) {
            test_record_failure(__FILE__, __LINE__,
                                "post-shrink size: s=0x%08x eof=%ld",
                                (unsigned)s, (long)info.EndOfFile.QuadPart);
            ok = false;
        }
    }

    /* A read entirely past the new EOF fails with END_OF_FILE. */
    if (ok) {
        LARGE_INTEGER off = { .QuadPart = KB(64) };
        s = NtReadFile(h, NULL, NULL, NULL, &iosb, g_buf, CHUNK, &off);
        if (s != STATUS_END_OF_FILE) {
            test_record_failure(__FILE__, __LINE__,
                                "read past EOF -> 0x%08x, want END_OF_FILE",
                                (unsigned)s);
            ok = false;
        }
    }

    if (ok) ok = read_check_at(h, 0, KB(64), 0xD4, "surviving prefix");

    NtClose(h);
    unlink_path(CACHE_A);
    return ok;
}

/* Truncating dirty cached data then re-extending and rewriting: the
 * purged tail must not resurface as the old bytes after the rewrite,
 * and everything persists across reopen. */
static bool t_truncate_purges_dirty(void)
{
    HANDLE h = NULL;
    IO_STATUS_BLOCK iosb;
    FILE_END_OF_FILE_INFORMATION eof;
    NTSTATUS s;
    bool ok = true;

    unlink_path(CACHE_A);
    s = open_path(CACHE_A, &h, GENERIC_READ | GENERIC_WRITE,
                  FILE_OVERWRITE_IF, 0, &iosb);
    ASSERT_NTSTATUS(s, STATUS_SUCCESS);

    /* Dirty 256 KB (salt 1), truncate to 64 KB, re-extend, rewrite the
     * tail with salt 2. */
    for (ULONG pos = 0; pos < KB(256) && ok; pos += CHUNK)
        ok = write_at(h, pos, CHUNK, 0x11);

    if (ok) {
        eof.EndOfFile.QuadPart = KB(64);
        s = NtSetInformationFile(h, &iosb, &eof, sizeof(eof),
                                 FileEndOfFileInformation);
        if (s != STATUS_SUCCESS) {
            test_record_failure(__FILE__, __LINE__, "shrink -> 0x%08x",
                                (unsigned)s);
            ok = false;
        }
    }
    for (ULONG pos = KB(64); pos < KB(256) && ok; pos += CHUNK)
        ok = write_at(h, pos, CHUNK, 0x22);

    NtClose(h);

    if (ok) {
        s = open_path(CACHE_A, &h, GENERIC_READ, FILE_OPEN, 0, &iosb);
        if (s != STATUS_SUCCESS) {
            unlink_path(CACHE_A);
            FAIL_AND_RETURN("reopen -> 0x%08x", (unsigned)s);
        }
        ok = read_check_at(h, 0, KB(64), 0x11, "prefix") &&
             read_check_at(h, KB(64), KB(64), 0x22, "rewritten tail") &&
             read_check_at(h, KB(192), KB(64), 0x22, "rewritten end");
        NtClose(h);
    }

    unlink_path(CACHE_A);
    return ok;
}

/* Fsc* cache-size ordinals: get/set round-trips with working IO at the
 * adjusted size.  Defaults differ by design (retail: 16 pages; nxkrnl:
 * block-granular with a larger boot default), so the default is
 * recorded, not asserted. */
static bool t_fsc_cache_size(void)
{
    PFN_COUNT Original = FscGetCacheSize();
    HANDLE h = NULL;
    IO_STATUS_BLOCK iosb;
    NTSTATUS s;
    bool ok = true;

    tap_comment("fsc: default cache pages = %lu", (unsigned long)Original);
    ASSERT_TRUE(Original > 0);

    s = FscSetCacheSize(64);
    tap_comment("fsc: Set(64) -> 0x%08x, Get = %lu", (unsigned)s,
                (unsigned long)FscGetCacheSize());
    ASSERT_NTSTATUS(s, STATUS_SUCCESS);
    ASSERT_TRUE(FscGetCacheSize() >= 64);

    /* Cached IO still round-trips at the new size. */
    unlink_path(CACHE_A);
    s = open_path(CACHE_A, &h, GENERIC_READ | GENERIC_WRITE,
                  FILE_OVERWRITE_IF, 0, &iosb);
    ASSERT_NTSTATUS(s, STATUS_SUCCESS);
    for (ULONG pos = 0; pos < KB(256) && ok; pos += CHUNK)
        ok = write_at(h, pos, CHUNK, 0xF5);
    for (ULONG pos = 0; pos < KB(256) && ok; pos += CHUNK)
        ok = read_check_at(h, pos, CHUNK, 0xF5, "fsc resized IO");
    NtClose(h);
    unlink_path(CACHE_A);
    if (!ok) return false;

    FscInvalidateIdleBlocks();

    s = FscSetCacheSize(Original);
    tap_comment("fsc: restore(%lu) -> 0x%08x, Get = %lu",
                (unsigned long)Original, (unsigned)s,
                (unsigned long)FscGetCacheSize());
    ASSERT_NTSTATUS(s, STATUS_SUCCESS);
    return true;
}

static const test_entry_t mm_cache_entries[] = {
    {"eviction_two_files",    t_eviction_two_files},
    {"read_churn",            t_read_churn},
    {"truncate_shrinks",      t_truncate_shrinks},
    {"truncate_purges_dirty", t_truncate_purges_dirty},
    {"fsc_cache_size",        t_fsc_cache_size},
};

DEFINE_GROUP(mm_cache, "mm/cache");
