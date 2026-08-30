/*
 * Cached-write contract through the file cache: flush visibility,
 * cross-handle coherence, sustained write-heavy workloads, and
 * extend-with-zero-fill.
 *
 * Regression coverage for the cache-manager trims (lazy-writer
 * machinery drop, CcCanIWrite -> TRUE, CcDeferWrite synchronous
 * dispatch): with no background lazy writer the only flushers left are
 * NtFlushBuffersFile, file close, and shutdown.  Titles do file IO for
 * their whole run, so the observable contract is: cached writes always
 * complete (no throttle hang), data is coherently visible to any other
 * handle on the same file before any flush, an explicit flush
 * succeeds, and data written + closed survives reopen.
 *
 * Retail-validated semantics: writes never
 * block indefinitely, NtFlushBuffersFile returns STATUS_SUCCESS, a
 * second handle opened before any flush reads the new bytes, and
 * extending EOF with NtSetInformationFile(FileEndOfFileInformation)
 * succeeds and preserves the pre-extend bytes.  Note the gap content
 * after an EOF extension is NOT zeroed by the retail kernel (stale
 * cluster bytes were observed), so no test may assert zero-fill.
 *
 * Timing-insensitive by construction: completion of each test is the
 * no-deadlock assertion (the suite runner's timeout converts a hang
 * into a failure); nothing asserts durations.  The HDD is left clean
 * (explicit delete-on-close unlink of every path touched).
 */

#include "../harness.h"
#include <stdio.h>
#include <string.h>

#ifndef FILE_DELETE_ON_CLOSE
#define FILE_DELETE_ON_CLOSE 0x00001000
#endif
#ifndef STATUS_NO_SUCH_FILE
#define STATUS_NO_SUCH_FILE ((NTSTATUS)0xC000000FL)
#endif

#define CHUNK_SIZE   0x10000             /* 64 KB */
#define HEAVY_TOTAL  (8u * 1024 * 1024)  /* 8 MB, written unflushed */
#define CHURN_FILES  16
#define CHURN_SIZE   0x20000             /* 128 KB per churn file */

static const char CC_SCRATCH[] =
    "\\Device\\Harddisk0\\Partition1\\nxkrnl-api-cc.tmp";

static char g_path_buf[96];
static unsigned char g_chunk[CHUNK_SIZE];

static NTSTATUS open_path_share(const char *path, HANDLE *h,
                                ACCESS_MASK access, ULONG share,
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
                        FILE_ATTRIBUTE_NORMAL, share, disposition,
                        opts | FILE_SYNCHRONOUS_IO_NONALERT
                             | FILE_NON_DIRECTORY_FILE);
}

static NTSTATUS open_path(const char *path, HANDLE *h, ACCESS_MASK access,
                          ULONG disposition, ULONG opts,
                          IO_STATUS_BLOCK *iosb)
{
    return open_path_share(path, h, access, 0, disposition, opts, iosb);
}

static void unlink_path(const char *path)
{
    HANDLE h;
    IO_STATUS_BLOCK iosb;
    NTSTATUS s = open_path(path, &h, GENERIC_READ, FILE_OPEN,
                           FILE_DELETE_ON_CLOSE, &iosb);
    if (NT_SUCCESS(s)) NtClose(h);
}

/* Deterministic, offset-dependent fill so a misplaced or stale sector is
 * caught no matter where it lands. */
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

/* NtFlushBuffersFile succeeds on a written handle, and the bytes are
 * visible to a second handle opened afterwards while the writer stays
 * open. */
static bool t_flush_buffers_visibility(void)
{
    HANDLE hw, hr;
    IO_STATUS_BLOCK iosb;
    NTSTATUS s;
    LARGE_INTEGER zero = { .QuadPart = 0 };

    unlink_path(CC_SCRATCH);
    s = open_path_share(CC_SCRATCH, &hw, GENERIC_READ | GENERIC_WRITE,
                        FILE_SHARE_READ, FILE_OVERWRITE_IF, 0, &iosb);
    ASSERT_NTSTATUS(s, STATUS_SUCCESS);

    fill_pattern(g_chunk, CHUNK_SIZE, 0, 0x11);
    s = NtWriteFile(hw, NULL, NULL, NULL, &iosb, g_chunk, CHUNK_SIZE, NULL);
    ASSERT_NTSTATUS(s, STATUS_SUCCESS);
    ASSERT_EQ_U32(iosb.Information, CHUNK_SIZE);

    s = NtFlushBuffersFile(hw, &iosb);
    if (s != STATUS_SUCCESS) {
        NtClose(hw);
        unlink_path(CC_SCRATCH);
        FAIL_AND_RETURN("NtFlushBuffersFile -> 0x%08x", (unsigned)s);
    }

    s = open_path_share(CC_SCRATCH, &hr, GENERIC_READ,
                        FILE_SHARE_READ | FILE_SHARE_WRITE, FILE_OPEN, 0,
                        &iosb);
    if (s != STATUS_SUCCESS) {
        NtClose(hw);
        unlink_path(CC_SCRATCH);
        FAIL_AND_RETURN("reopen after flush -> 0x%08x", (unsigned)s);
    }
    memset(g_chunk, 0, CHUNK_SIZE);
    s = NtReadFile(hr, NULL, NULL, NULL, &iosb, g_chunk, CHUNK_SIZE, &zero);
    NtClose(hr);
    if (s != STATUS_SUCCESS || iosb.Information != CHUNK_SIZE) {
        NtClose(hw);
        unlink_path(CC_SCRATCH);
        FAIL_AND_RETURN("read-back: s=0x%08x len=%u", (unsigned)s,
                        (unsigned)iosb.Information);
    }
    if (!check_pattern(g_chunk, CHUNK_SIZE, 0, 0x11, "flushed bytes")) {
        NtClose(hw);
        unlink_path(CC_SCRATCH);
        return false;
    }

    NtClose(hw);
    unlink_path(CC_SCRATCH);
    return true;
}

/* A second handle sees cached bytes BEFORE any flush: the cache is
 * coherent per-file, not per-handle. */
static bool t_cross_handle_no_flush(void)
{
    HANDLE hw, hr;
    IO_STATUS_BLOCK iosb;
    NTSTATUS s;
    LARGE_INTEGER zero = { .QuadPart = 0 };

    unlink_path(CC_SCRATCH);
    s = open_path_share(CC_SCRATCH, &hw, GENERIC_READ | GENERIC_WRITE,
                        FILE_SHARE_READ, FILE_OVERWRITE_IF, 0, &iosb);
    ASSERT_NTSTATUS(s, STATUS_SUCCESS);

    fill_pattern(g_chunk, CHUNK_SIZE, 0, 0x22);
    s = NtWriteFile(hw, NULL, NULL, NULL, &iosb, g_chunk, CHUNK_SIZE, NULL);
    ASSERT_NTSTATUS(s, STATUS_SUCCESS);

    /* No flush here: the second handle must still see the new bytes. */
    s = open_path_share(CC_SCRATCH, &hr, GENERIC_READ,
                        FILE_SHARE_READ | FILE_SHARE_WRITE, FILE_OPEN, 0,
                        &iosb);
    if (s != STATUS_SUCCESS) {
        NtClose(hw);
        unlink_path(CC_SCRATCH);
        FAIL_AND_RETURN("open 2nd handle -> 0x%08x", (unsigned)s);
    }
    memset(g_chunk, 0, CHUNK_SIZE);
    s = NtReadFile(hr, NULL, NULL, NULL, &iosb, g_chunk, CHUNK_SIZE, &zero);
    NtClose(hr);
    NtClose(hw);
    if (s != STATUS_SUCCESS || iosb.Information != CHUNK_SIZE) {
        unlink_path(CC_SCRATCH);
        FAIL_AND_RETURN("cross-handle read: s=0x%08x len=%u", (unsigned)s,
                        (unsigned)iosb.Information);
    }
    if (!check_pattern(g_chunk, CHUNK_SIZE, 0, 0x22, "unflushed bytes")) {
        unlink_path(CC_SCRATCH);
        return false;
    }
    unlink_path(CC_SCRATCH);
    return true;
}

/* Sustained cached writes -- 8 MB with no intervening flush -- complete
 * without throttling/hanging, and every byte survives close + reopen.
 * Completing at all is the no-deadlock assertion. */
static bool t_write_heavy_no_deadlock(void)
{
    HANDLE h;
    IO_STATUS_BLOCK iosb;
    NTSTATUS s;
    ULONG off;

    unlink_path(CC_SCRATCH);
    s = open_path(CC_SCRATCH, &h, GENERIC_READ | GENERIC_WRITE,
                  FILE_OVERWRITE_IF, 0, &iosb);
    ASSERT_NTSTATUS(s, STATUS_SUCCESS);

    for (off = 0; off < HEAVY_TOTAL; off += CHUNK_SIZE) {
        fill_pattern(g_chunk, CHUNK_SIZE, off, 0x33);
        s = NtWriteFile(h, NULL, NULL, NULL, &iosb, g_chunk, CHUNK_SIZE,
                        NULL);
        if (s != STATUS_SUCCESS || iosb.Information != CHUNK_SIZE) {
            NtClose(h);
            unlink_path(CC_SCRATCH);
            FAIL_AND_RETURN("write @%lu: s=0x%08x len=%u",
                            (unsigned long)off, (unsigned)s,
                            (unsigned)iosb.Information);
        }
    }
    NtClose(h);

    /* Reopen and verify front, middle, and tail chunks. */
    s = open_path(CC_SCRATCH, &h, GENERIC_READ, FILE_OPEN, 0, &iosb);
    ASSERT_NTSTATUS(s, STATUS_SUCCESS);
    {
        static const ULONG spots[] = {
            0, HEAVY_TOTAL / 2, HEAVY_TOTAL - CHUNK_SIZE
        };
        for (unsigned i = 0; i < sizeof(spots)/sizeof(spots[0]); i++) {
            LARGE_INTEGER pos = { .QuadPart = spots[i] };
            memset(g_chunk, 0, CHUNK_SIZE);
            s = NtReadFile(h, NULL, NULL, NULL, &iosb, g_chunk, CHUNK_SIZE,
                           &pos);
            if (s != STATUS_SUCCESS || iosb.Information != CHUNK_SIZE) {
                NtClose(h);
                unlink_path(CC_SCRATCH);
                FAIL_AND_RETURN("verify read @%lu: s=0x%08x len=%u",
                                (unsigned long)spots[i], (unsigned)s,
                                (unsigned)iosb.Information);
            }
            if (!check_pattern(g_chunk, CHUNK_SIZE, spots[i], 0x33,
                               "heavy data")) {
                NtClose(h);
                unlink_path(CC_SCRATCH);
                return false;
            }
        }
    }
    NtClose(h);
    unlink_path(CC_SCRATCH);
    return true;
}

/* Create/write/close churn across many files: every close must flush
 * its dirty data, and the directory/FAT updates must hold up for the
 * reopen + verify pass with no background writer involved. */
static bool t_many_files_flush_on_close(void)
{
    IO_STATUS_BLOCK iosb;
    NTSTATUS s;
    int i;
    bool ok = true;

    for (i = 0; i < CHURN_FILES; i++) {
        HANDLE h;
        ULONG off;
        snprintf(g_path_buf, sizeof(g_path_buf),
                 "\\Device\\Harddisk0\\Partition1\\nxkrnl-api-cc%02d.tmp", i);
        unlink_path(g_path_buf);
        s = open_path(g_path_buf, &h, GENERIC_WRITE, FILE_OVERWRITE_IF, 0,
                      &iosb);
        if (s != STATUS_SUCCESS) {
            test_record_failure(__FILE__, __LINE__,
                                "create #%d -> 0x%08x", i, (unsigned)s);
            ok = false;
            break;
        }
        for (off = 0; off < CHURN_SIZE; off += CHUNK_SIZE) {
            fill_pattern(g_chunk, CHUNK_SIZE, off, (unsigned char)i);
            s = NtWriteFile(h, NULL, NULL, NULL, &iosb, g_chunk, CHUNK_SIZE,
                            NULL);
            if (s != STATUS_SUCCESS) break;
        }
        NtClose(h);
        if (s != STATUS_SUCCESS) {
            test_record_failure(__FILE__, __LINE__,
                                "write #%d @%lu -> 0x%08x", i,
                                (unsigned long)off, (unsigned)s);
            ok = false;
            break;
        }
    }

    /* Verify + clean up every file we managed to create. */
    for (i = 0; i < CHURN_FILES; i++) {
        HANDLE h;
        LARGE_INTEGER tail = { .QuadPart = CHURN_SIZE - CHUNK_SIZE };
        snprintf(g_path_buf, sizeof(g_path_buf),
                 "\\Device\\Harddisk0\\Partition1\\nxkrnl-api-cc%02d.tmp", i);
        s = open_path(g_path_buf, &h, GENERIC_READ, FILE_OPEN,
                      FILE_DELETE_ON_CLOSE, &iosb);
        if (s != STATUS_SUCCESS) {
            if (ok) {
                test_record_failure(__FILE__, __LINE__,
                                    "reopen #%d -> 0x%08x", i, (unsigned)s);
                ok = false;
            }
            continue;
        }
        if (ok) {
            memset(g_chunk, 0, CHUNK_SIZE);
            s = NtReadFile(h, NULL, NULL, NULL, &iosb, g_chunk, CHUNK_SIZE,
                           &tail);
            if (s != STATUS_SUCCESS || iosb.Information != CHUNK_SIZE) {
                test_record_failure(__FILE__, __LINE__,
                                    "verify #%d: s=0x%08x len=%u", i,
                                    (unsigned)s, (unsigned)iosb.Information);
                ok = false;
            } else if (!check_pattern(g_chunk, CHUNK_SIZE,
                                      CHURN_SIZE - CHUNK_SIZE,
                                      (unsigned char)i, "churn tail")) {
                ok = false;
            }
        }
        NtClose(h);  /* delete-on-close reclaims the file */
    }
    return ok;
}

/* Extending EOF with NtSetInformationFile(FileEndOfFileInformation)
 * succeeds, grows the readable range to the new EOF, and preserves the
 * bytes written before the extension.  Retail
 * does NOT zero-fill the gap -- stale cluster bytes were observed --
 * so the gap content is left unasserted. */
static bool t_extend_preserves_data(void)
{
    HANDLE h;
    IO_STATUS_BLOCK iosb;
    NTSTATUS s;
    LARGE_INTEGER zero = { .QuadPart = 0 };
    FILE_END_OF_FILE_INFORMATION eof;

    unlink_path(CC_SCRATCH);
    s = open_path(CC_SCRATCH, &h, GENERIC_READ | GENERIC_WRITE,
                  FILE_OVERWRITE_IF, 0, &iosb);
    ASSERT_NTSTATUS(s, STATUS_SUCCESS);

    fill_pattern(g_chunk, 4096, 0, 0x44);
    s = NtWriteFile(h, NULL, NULL, NULL, &iosb, g_chunk, 4096, NULL);
    ASSERT_NTSTATUS(s, STATUS_SUCCESS);

    eof.EndOfFile.QuadPart = CHUNK_SIZE;
    s = NtSetInformationFile(h, &iosb, &eof, sizeof(eof),
                             FileEndOfFileInformation);
    if (s != STATUS_SUCCESS) {
        NtClose(h);
        unlink_path(CC_SCRATCH);
        FAIL_AND_RETURN("set EOF -> 0x%08x", (unsigned)s);
    }

    memset(g_chunk, 0xee, CHUNK_SIZE);
    s = NtReadFile(h, NULL, NULL, NULL, &iosb, g_chunk, CHUNK_SIZE, &zero);
    NtClose(h);
    if (s != STATUS_SUCCESS || iosb.Information != CHUNK_SIZE) {
        unlink_path(CC_SCRATCH);
        FAIL_AND_RETURN("read extended: s=0x%08x len=%u", (unsigned)s,
                        (unsigned)iosb.Information);
    }
    if (!check_pattern(g_chunk, 4096, 0, 0x44, "pre-extend bytes")) {
        unlink_path(CC_SCRATCH);
        return false;
    }
    /* Gap content (4096..CHUNK_SIZE) is intentionally unasserted:
     * retail exposes stale cluster bytes there. */
    unlink_path(CC_SCRATCH);
    return true;
}

/* Small whole-file write -> close -> reopen -> read, across a range of
 * sub-block / non-sector-aligned sizes.  The other cc-write tests write
 * in 64 KB / 4 KB chunks, so the small whole-file case was unexercised.
 * Contract: the reopened read returns exactly the bytes written. */
static bool t_small_file_reopen(void)
{
    static const ULONG sizes[] = {
        1, 3, 64, 137, 256, 511, 512, 513, 1000, 4095, 4096, 4097,
    };
    bool ok = true;

    for (unsigned i = 0; i < sizeof(sizes)/sizeof(sizes[0]); i++) {
        HANDLE h;
        IO_STATUS_BLOCK iosb;
        NTSTATUS s;
        LARGE_INTEGER zero = { .QuadPart = 0 };
        ULONG n = sizes[i];

        unlink_path(CC_SCRATCH);

        s = open_path(CC_SCRATCH, &h, GENERIC_READ | GENERIC_WRITE,
                      FILE_OVERWRITE_IF, 0, &iosb);
        if (s != STATUS_SUCCESS) {
            test_record_failure(__FILE__, __LINE__,
                                "size %lu: create -> 0x%08x", n, (unsigned)s);
            ok = false; continue;
        }

        fill_pattern(g_chunk, n, 0, 0x5a);
        s = NtWriteFile(h, NULL, NULL, NULL, &iosb, g_chunk, n, NULL);
        if (s != STATUS_SUCCESS || iosb.Information != n) {
            test_record_failure(__FILE__, __LINE__,
                                "size %lu: write s=0x%08x len=%u", n,
                                (unsigned)s, (unsigned)iosb.Information);
            NtClose(h); ok = false; continue;
        }

        /* Close flushes (no lazy writer); the read-back is a fresh handle,
         * exactly like the title's separate write/read handles. */
        NtClose(h);

        s = open_path(CC_SCRATCH, &h, GENERIC_READ, FILE_OPEN, 0, &iosb);
        if (s != STATUS_SUCCESS) {
            test_record_failure(__FILE__, __LINE__,
                                "size %lu: reopen -> 0x%08x", n, (unsigned)s);
            ok = false; continue;
        }

        memset(g_chunk, 0, n);
        s = NtReadFile(h, NULL, NULL, NULL, &iosb, g_chunk, n, &zero);
        if (s != STATUS_SUCCESS || iosb.Information != n) {
            test_record_failure(__FILE__, __LINE__,
                                "size %lu: read-back s=0x%08x len=%u (want %lu)",
                                n, (unsigned)s, (unsigned)iosb.Information, n);
            NtClose(h); ok = false; continue;
        }
        if (!check_pattern(g_chunk, n, 0, 0x5a, "small reopen")) {
            NtClose(h); ok = false; continue;
        }
        NtClose(h);
    }

    unlink_path(CC_SCRATCH);
    return ok;
}

#ifndef FILE_DIRECTORY_FILE
#define FILE_DIRECTORY_FILE 0x00000001
#endif
#ifndef FILE_OPEN_IF
#define FILE_OPEN_IF 3
#endif

static const char CC_SUBDIR[] =
    "\\Device\\Harddisk0\\Partition1\\nxkrnl-api-ccdir";
static const char CC_SUBFILE[] =
    "\\Device\\Harddisk0\\Partition1\\nxkrnl-api-ccdir\\small.bin";

/* Small file written into a freshly-created subdirectory survives reopen.
 * Unlike t_small_file_reopen the file is in a just-made subdir, so its
 * size update lands in that new directory's dir block; sibling files then
 * churn the same directory before the reopen.  Contract: the reopened
 * read returns exactly the bytes written (not a zero-length / EOF read,
 * which a mismatched dir-entry slot index would produce). */
static bool t_subdir_small_reopen(void)
{
    HANDLE h;
    IO_STATUS_BLOCK iosb;
    NTSTATUS s;
    LARGE_INTEGER zero = { .QuadPart = 0 };
    const ULONG FT = 137;
    bool ok = true;

    /* fresh subdir (raw NtCreateFile: open_path forces FILE_NON_DIRECTORY_FILE
     * which conflicts with FILE_DIRECTORY_FILE) */
    {
        ANSI_STRING name;
        OBJECT_ATTRIBUTES oa;
        name.Length        = (USHORT)strlen(CC_SUBDIR);
        name.MaximumLength = name.Length + 1;
        name.Buffer        = (PCHAR)CC_SUBDIR;
        oa.RootDirectory   = NULL;
        oa.ObjectName      = &name;
        oa.Attributes      = OBJ_CASE_INSENSITIVE;
        s = NtCreateFile(&h, GENERIC_WRITE | SYNCHRONIZE, &oa, &iosb, NULL,
                         FILE_ATTRIBUTE_DIRECTORY, 0, FILE_OPEN_IF,
                         FILE_DIRECTORY_FILE | FILE_SYNCHRONOUS_IO_NONALERT);
        if (s != STATUS_SUCCESS)
            FAIL_AND_RETURN("mkdir -> 0x%08x", (unsigned)s);
        NtClose(h);
    }

    /* write the small file */
    s = open_path(CC_SUBFILE, &h, GENERIC_READ | GENERIC_WRITE,
                  FILE_OVERWRITE_IF, 0, &iosb);
    if (s != STATUS_SUCCESS) { ok=false; goto out; }
    fill_pattern(g_chunk, FT, 0, 0x5a);
    s = NtWriteFile(h, NULL, NULL, NULL, &iosb, g_chunk, FT, NULL);
    if (s != STATUS_SUCCESS || iosb.Information != FT) {
        test_record_failure(__FILE__, __LINE__, "subfile write s=0x%08x len=%u",
                            (unsigned)s, (unsigned)iosb.Information);
        NtClose(h); ok=false; goto out;
    }
    NtClose(h);

    /* churn the directory: write several larger sibling files into it,
     * forcing the dir block's pending size update through the cache */
    for (int i = 0; i < 6; i++) {
        HANDLE hc;
        snprintf(g_path_buf, sizeof(g_path_buf),
                 "\\Device\\Harddisk0\\Partition1\\nxkrnl-api-ccdir\\sib%d.dat", i);
        if (open_path(g_path_buf, &hc, GENERIC_WRITE, FILE_OVERWRITE_IF, 0,
                      &iosb) != STATUS_SUCCESS) continue;
        for (ULONG off = 0; off < 0x30000; off += CHUNK_SIZE) {
            fill_pattern(g_chunk, CHUNK_SIZE, off, (unsigned char)(0x80+i));
            NtWriteFile(hc, NULL, NULL, NULL, &iosb, g_chunk, CHUNK_SIZE, NULL);
        }
        NtClose(hc);
    }

    /* reopen the small file and read it back */
    s = open_path(CC_SUBFILE, &h, GENERIC_READ, FILE_OPEN, 0, &iosb);
    if (s != STATUS_SUCCESS) {
        test_record_failure(__FILE__, __LINE__, "subfile reopen -> 0x%08x",
                            (unsigned)s);
        ok=false; goto out;
    }
    memset(g_chunk, 0, FT);
    s = NtReadFile(h, NULL, NULL, NULL, &iosb, g_chunk, FT, &zero);
    if (s != STATUS_SUCCESS || iosb.Information != FT) {
        test_record_failure(__FILE__, __LINE__,
                            "subfile read-back s=0x%08x len=%u (want %lu)",
                            (unsigned)s, (unsigned)iosb.Information, FT);
        NtClose(h); ok=false; goto out;
    }
    if (!check_pattern(g_chunk, FT, 0, 0x5a, "subdir small reopen")) {
        NtClose(h); ok=false; goto out;
    }
    NtClose(h);

out:
    /* cleanup: delete files + subdir */
    unlink_path(CC_SUBFILE);
    for (int i = 0; i < 6; i++) {
        snprintf(g_path_buf, sizeof(g_path_buf),
                 "\\Device\\Harddisk0\\Partition1\\nxkrnl-api-ccdir\\sib%d.dat", i);
        unlink_path(g_path_buf);
    }
    unlink_path(CC_SUBDIR);
    return ok;
}


/* A write starting beyond EOF must extend the file through the gap
 * (the kernel zeroes cache pages over the hole on the way -- but per
 * the header note, the on-disk gap content is NOT asserted). */
static bool t_gap_write_extends(void)
{
    HANDLE h;
    IO_STATUS_BLOCK iosb;
    NTSTATUS s = open_path(CC_SCRATCH, &h, GENERIC_READ | GENERIC_WRITE,
                           FILE_OVERWRITE_IF, 0, &iosb);
    ASSERT_NTSTATUS(s, STATUS_SUCCESS);

    unsigned char lo[0x1000], hi[0x1000], back[0x1000];
    fill_pattern(lo, sizeof(lo), 0, 0x5A);
    fill_pattern(hi, sizeof(hi), 0x20000, 0x5A);

    LARGE_INTEGER off0 = { .QuadPart = 0 };
    LARGE_INTEGER off128k = { .QuadPart = 0x20000 };
    s = NtWriteFile(h, NULL, NULL, NULL, &iosb, lo, sizeof(lo), &off0);
    if (NT_SUCCESS(s))
        s = NtWriteFile(h, NULL, NULL, NULL, &iosb, hi, sizeof(hi),
                        &off128k);
    if (!NT_SUCCESS(s)) {
        NtClose(h);
        unlink_path(CC_SCRATCH);
        FAIL_AND_RETURN("gap write -> 0x%08x", (unsigned)s);
    }

    FILE_NETWORK_OPEN_INFORMATION ni;
    s = NtQueryInformationFile(h, &iosb, &ni, sizeof(ni),
                               FileNetworkOpenInformation);
    bool size_ok = NT_SUCCESS(s) &&
                   ni.EndOfFile.QuadPart == 0x20000 + 0x1000;

    s = NtReadFile(h, NULL, NULL, NULL, &iosb, back, sizeof(back),
                   &off128k);
    NtClose(h);
    unlink_path(CC_SCRATCH);
    ASSERT_TRUE(size_ok);
    ASSERT_NTSTATUS(s, STATUS_SUCCESS);
    ASSERT_TRUE(memcmp(hi, back, sizeof(back)) == 0);
    return true;
}

/* --- roswell#26 repro: preallocated create + full-coverage write ------------
 * NtCreateFile with a non-NULL AllocationSize followed by a single WriteFile
 * that covers the whole allocation.  It deadlocked the file cache when both
 * the allocation and the write were >= 64 KB (every cache block filled with
 * the file's dirty data, leaving none for the FAT the write-back re-reads); a
 * shorter write, or a smaller allocation, left a free block and passed.
 * Mirrors abaire/roswell PR#2 (FILE_SUPERSEDE, GENERIC_WRITE, SYNCHRONOUS |
 * NON_DIRECTORY | SEQUENTIAL_ONLY). */
#ifndef FILE_SUPERSEDE
#define FILE_SUPERSEDE 0
#endif
#ifndef FILE_SEQUENTIAL_ONLY
#define FILE_SEQUENTIAL_ONLY 0x00000004
#endif

static bool repro_prealloc_write(ULONG alloc_bytes, ULONG write_bytes)
{
    HANDLE h;
    IO_STATUS_BLOCK iosb;
    NTSTATUS s;
    ANSI_STRING name;
    OBJECT_ATTRIBUTES oa;
    LARGE_INTEGER alloc;

    unlink_path(CC_SCRATCH);

    name.Length        = (USHORT)strlen(CC_SCRATCH);
    name.MaximumLength = name.Length + 1;
    name.Buffer        = (PCHAR)CC_SCRATCH;
    oa.RootDirectory   = NULL;
    oa.ObjectName      = &name;
    oa.Attributes      = OBJ_CASE_INSENSITIVE;

    alloc.QuadPart = alloc_bytes;

    s = NtCreateFile(&h, FILE_GENERIC_WRITE | SYNCHRONIZE, &oa, &iosb, &alloc,
                     FILE_ATTRIBUTE_NORMAL, 0, FILE_SUPERSEDE,
                     FILE_SYNCHRONOUS_IO_NONALERT | FILE_NON_DIRECTORY_FILE
                         | FILE_SEQUENTIAL_ONLY);
    if (s != STATUS_SUCCESS)
        FAIL_AND_RETURN("create alloc=%lu -> 0x%08x", alloc_bytes, (unsigned)s);

    fill_pattern(g_chunk, write_bytes, 0, 0x77);
    s = NtWriteFile(h, NULL, NULL, NULL, &iosb, g_chunk, write_bytes, NULL);
    NtClose(h);
    unlink_path(CC_SCRATCH);
    if (s != STATUS_SUCCESS || iosb.Information != write_bytes)
        FAIL_AND_RETURN("write alloc=%lu len=%lu -> s=0x%08x got=%u",
                        alloc_bytes, write_bytes, (unsigned)s,
                        (unsigned)iosb.Information);
    return true;
}

/* Retail FATX ignores the create-time NtCreateFile AllocationSize: a new
 * file is empty (EndOfFile 0) until written, and a write moves EOF to the
 * high-water mark.  Retail-captured (--official): eof=0 after create(64 K),
 * eof=10 after a 10-byte write.  vfatfs used to set EOF to the allocation,
 * so a preallocated file reported its full size with no data written.
 * (AllocationSize is left unasserted: retail reports it == EOF, vfatfs
 * reports the cluster-rounded size -- a separate, pre-existing difference.) */
static LONGLONG query_eof(HANDLE h)
{
    IO_STATUS_BLOCK iosb;
    FILE_NETWORK_OPEN_INFORMATION ni;
    if (NtQueryInformationFile(h, &iosb, &ni, sizeof(ni),
                               FileNetworkOpenInformation) != STATUS_SUCCESS)
        return -1;
    return ni.EndOfFile.QuadPart;
}

static bool t_prealloc_create_eof(void)
{
    HANDLE h;
    IO_STATUS_BLOCK iosb;
    NTSTATUS s;
    ANSI_STRING name;
    OBJECT_ATTRIBUTES oa;
    LARGE_INTEGER alloc;
    LONGLONG eof;

    unlink_path(CC_SCRATCH);
    name.Length        = (USHORT)strlen(CC_SCRATCH);
    name.MaximumLength = name.Length + 1;
    name.Buffer        = (PCHAR)CC_SCRATCH;
    oa.RootDirectory   = NULL;
    oa.ObjectName      = &name;
    oa.Attributes      = OBJ_CASE_INSENSITIVE;
    alloc.QuadPart = 0x10000;

    s = NtCreateFile(&h, FILE_GENERIC_READ | FILE_GENERIC_WRITE | SYNCHRONIZE,
                     &oa, &iosb, &alloc, FILE_ATTRIBUTE_NORMAL, 0,
                     FILE_SUPERSEDE,
                     FILE_SYNCHRONOUS_IO_NONALERT | FILE_NON_DIRECTORY_FILE);
    ASSERT_NTSTATUS(s, STATUS_SUCCESS);

    eof = query_eof(h);
    if (eof != 0) {
        NtClose(h);
        unlink_path(CC_SCRATCH);
        FAIL_AND_RETURN("eof after create(alloc=64K): got %ld want 0", (long)eof);
    }

    fill_pattern(g_chunk, 10, 0, 0x77);
    s = NtWriteFile(h, NULL, NULL, NULL, &iosb, g_chunk, 10, NULL);
    eof = query_eof(h);
    NtClose(h);
    if (s != STATUS_SUCCESS || iosb.Information != 10) {
        unlink_path(CC_SCRATCH);
        FAIL_AND_RETURN("write(10): s=0x%08x len=%u", (unsigned)s,
                        (unsigned)iosb.Information);
    }
    if (eof != 10) {
        unlink_path(CC_SCRATCH);
        FAIL_AND_RETURN("eof after write(10): got %ld want 10", (long)eof);
    }

    s = open_path(CC_SCRATCH, &h, FILE_GENERIC_READ, FILE_OPEN, 0, &iosb);
    if (s == STATUS_SUCCESS) {
        eof = query_eof(h);
        NtClose(h);
        if (eof != 10) {
            unlink_path(CC_SCRATCH);
            FAIL_AND_RETURN("eof after reopen: got %ld want 10", (long)eof);
        }
    }
    unlink_path(CC_SCRATCH);
    return true;
}

/* Retail FATX has no separate allocation field: it reports AllocationSize
 * equal to EndOfFile -- in both the by-handle query and directory
 * enumeration -- where vfatfs reported the cluster-rounded size.  Retail-
 * captured (--official): alloc == eof for every write size, and for the
 * same file listed via NtQueryDirectoryFile. */
typedef struct {
    ULONG NextEntryOffset;
    ULONG FileIndex;
    LARGE_INTEGER CreationTime, LastAccessTime, LastWriteTime, ChangeTime;
    LARGE_INTEGER EndOfFile, AllocationSize;
    ULONG FileAttributes;
    ULONG FileNameLength;
    WCHAR FileName[1];
} DIR_INFO;

static bool t_allocsize_matches_eof(void)
{
    static const ULONG sizes[] = { 1, 512, 4096, 16385, 40000 };
    IO_STATUS_BLOCK iosb;
    NTSTATUS s;
    HANDLE h;
    ANSI_STRING name;
    OBJECT_ATTRIBUTES oa;
    static unsigned char dbuf[1024];

    /* by-handle query: FileNetworkOpenInformation */
    for (unsigned i = 0; i < sizeof(sizes)/sizeof(sizes[0]); i++) {
        FILE_NETWORK_OPEN_INFORMATION ni;
        ULONG n = sizes[i];
        unlink_path(CC_SCRATCH);
        s = open_path(CC_SCRATCH, &h, GENERIC_READ | GENERIC_WRITE,
                      FILE_OVERWRITE_IF, 0, &iosb);
        if (s != STATUS_SUCCESS) {
            unlink_path(CC_SCRATCH);
            FAIL_AND_RETURN("size %lu: create 0x%08x", n, (unsigned)s);
        }
        fill_pattern(g_chunk, n, 0, 0x33);
        s = NtWriteFile(h, NULL, NULL, NULL, &iosb, g_chunk, n, NULL);
        if (s == STATUS_SUCCESS)
            s = NtQueryInformationFile(h, &iosb, &ni, sizeof(ni),
                                       FileNetworkOpenInformation);
        NtClose(h);
        unlink_path(CC_SCRATCH);
        if (s != STATUS_SUCCESS)
            FAIL_AND_RETURN("size %lu: write/query 0x%08x", n, (unsigned)s);
        if (ni.EndOfFile.QuadPart != n || ni.AllocationSize.QuadPart != n)
            FAIL_AND_RETURN("size %lu: eof=%ld alloc=%ld (want both %lu)", n,
                            (long)ni.EndOfFile.QuadPart,
                            (long)ni.AllocationSize.QuadPart, n);
    }

    /* directory enumeration: FILE_DIRECTORY_INFORMATION */
    name.Length = (USHORT)strlen(CC_SUBDIR); name.MaximumLength = name.Length + 1;
    name.Buffer = (PCHAR)CC_SUBDIR; oa.RootDirectory = NULL; oa.ObjectName = &name;
    oa.Attributes = OBJ_CASE_INSENSITIVE;
    s = NtCreateFile(&h, GENERIC_WRITE | SYNCHRONIZE, &oa, &iosb, NULL,
                     FILE_ATTRIBUTE_DIRECTORY, 0, FILE_OPEN_IF,
                     FILE_DIRECTORY_FILE | FILE_SYNCHRONOUS_IO_NONALERT);
    if (s != STATUS_SUCCESS) FAIL_AND_RETURN("mkdir 0x%08x", (unsigned)s);
    NtClose(h);

    s = open_path(CC_SUBFILE, &h, GENERIC_WRITE, FILE_OVERWRITE_IF, 0, &iosb);
    if (s == STATUS_SUCCESS) {
        fill_pattern(g_chunk, 4096, 0, 0x33);
        NtWriteFile(h, NULL, NULL, NULL, &iosb, g_chunk, 4096, NULL);
        NtClose(h);
    }

    name.Length = (USHORT)strlen(CC_SUBDIR); name.MaximumLength = name.Length + 1;
    name.Buffer = (PCHAR)CC_SUBDIR; oa.RootDirectory = NULL; oa.ObjectName = &name;
    oa.Attributes = OBJ_CASE_INSENSITIVE;
    s = NtCreateFile(&h, GENERIC_READ | SYNCHRONIZE, &oa, &iosb, NULL,
                     FILE_ATTRIBUTE_DIRECTORY,
                     FILE_SHARE_READ | FILE_SHARE_WRITE, FILE_OPEN,
                     FILE_DIRECTORY_FILE | FILE_SYNCHRONOUS_IO_NONALERT);
    if (s != STATUS_SUCCESS) {
        unlink_path(CC_SUBFILE); unlink_path(CC_SUBDIR);
        FAIL_AND_RETURN("diropen 0x%08x", (unsigned)s);
    }
    memset(dbuf, 0, sizeof(dbuf));
    s = NtQueryDirectoryFile(h, NULL, NULL, NULL, &iosb, dbuf, sizeof(dbuf),
                             FileDirectoryInformation, NULL, TRUE);
    NtClose(h);
    if (!NT_SUCCESS(s)) {
        unlink_path(CC_SUBFILE); unlink_path(CC_SUBDIR);
        FAIL_AND_RETURN("dir-enum query 0x%08x", (unsigned)s);
    }
    {
        ULONG off = 0;
        bool seen = false;
        for (;;) {
            DIR_INFO *di = (DIR_INFO *)(dbuf + off);
            if (!(di->FileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
                seen = true;
                if (di->AllocationSize.QuadPart != di->EndOfFile.QuadPart) {
                    unlink_path(CC_SUBFILE); unlink_path(CC_SUBDIR);
                    FAIL_AND_RETURN("dir-enum: eof=%ld alloc=%ld (want equal)",
                                    (long)di->EndOfFile.QuadPart,
                                    (long)di->AllocationSize.QuadPart);
                }
            }
            if (di->NextEntryOffset == 0) break;
            off += di->NextEntryOffset;
            if (off >= sizeof(dbuf)) break;
        }
        if (!seen) {
            unlink_path(CC_SUBFILE); unlink_path(CC_SUBDIR);
            FAIL_AND_RETURN("dir-enum: no file entry found");
        }
    }
    unlink_path(CC_SUBFILE);
    unlink_path(CC_SUBDIR);
    return true;
}

/* Passing control: 64 KB preallocation, 48 KB write (issue test 6b). */
static bool t_prealloc_write_partial(void)
{
    return repro_prealloc_write(0x10000, 0xC000);
}

/* The repro: 64 KB preallocation, 64 KB write (issue test 7). */
static bool t_prealloc_write_full(void)
{
    return repro_prealloc_write(0x10000, 0x10000);
}

/* Guards the cache write-back reserve directly, independent of the create-
 * time AllocationSize handling: preallocate 64 KB via
 * NtSetInformationFile(FileAllocationInformation) -- which allocates the
 * clusters but leaves EOF at 0 -- then write 64 KB into them.  The clusters
 * already exist, so the write only reads the FAT to map them; filling all
 * four cache blocks with the file's data evicts that FAT block, and the
 * write-back must re-read it with no free slot.  Deadlocked before the
 * reserve fix; completing is the assertion (roswell#26). */
static bool t_prealloc_setinfo_write_full(void)
{
    HANDLE h;
    IO_STATUS_BLOCK iosb;
    NTSTATUS s;
    FILE_ALLOCATION_INFORMATION ai;

    unlink_path(CC_SCRATCH);
    s = open_path(CC_SCRATCH, &h, GENERIC_READ | GENERIC_WRITE,
                  FILE_OVERWRITE_IF, 0, &iosb);
    ASSERT_NTSTATUS(s, STATUS_SUCCESS);

    ai.AllocationSize.QuadPart = 0x10000;
    s = NtSetInformationFile(h, &iosb, &ai, sizeof(ai),
                             FileAllocationInformation);
    if (s != STATUS_SUCCESS) {
        NtClose(h);
        unlink_path(CC_SCRATCH);
        FAIL_AND_RETURN("set-alloc 64K -> 0x%08x", (unsigned)s);
    }

    fill_pattern(g_chunk, CHUNK_SIZE, 0, 0x66);
    s = NtWriteFile(h, NULL, NULL, NULL, &iosb, g_chunk, CHUNK_SIZE, NULL);
    NtClose(h);
    unlink_path(CC_SCRATCH);
    if (s != STATUS_SUCCESS || iosb.Information != CHUNK_SIZE)
        FAIL_AND_RETURN("write 64K -> s=0x%08x len=%u", (unsigned)s,
                        (unsigned)iosb.Information);
    return true;
}

static const test_entry_t io_cc_write_entries[] = {
    {"flush_buffers_visibility",  t_flush_buffers_visibility},
    {"cross_handle_no_flush",     t_cross_handle_no_flush},
    {"write_heavy_no_deadlock",   t_write_heavy_no_deadlock},
    {"many_files_flush_on_close", t_many_files_flush_on_close},
    {"extend_preserves_data",     t_extend_preserves_data},
    {"small_file_reopen",         t_small_file_reopen},
    {"subdir_small_reopen",       t_subdir_small_reopen},
    {"gap_write_extends",         t_gap_write_extends},
    {"allocsize_matches_eof",     t_allocsize_matches_eof},
    {"prealloc_create_eof",       t_prealloc_create_eof},
    {"prealloc_write_partial",    t_prealloc_write_partial},
    {"prealloc_write_full",       t_prealloc_write_full},
    {"prealloc_setinfo_write_full", t_prealloc_setinfo_write_full},
};

DEFINE_GROUP(io_cc_write, "io/cc-write");
