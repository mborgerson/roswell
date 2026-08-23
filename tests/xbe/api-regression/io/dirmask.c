/*
 * NtQueryDirectoryFile with title-controlled wildcard masks.
 *
 * Regression coverage for the FsRtlIsNameInExpressionPrivate trim:
 * the wildcard backtracking buffer scales with
 * the number of '*'s in the MASK, not with FATX name length, and the
 * trim removed both the dynamic buffer and the overflow check -- a
 * heavy-star mask could corrupt the kernel stack.  Also pins basic
 * single-entry enumeration through the ANSI fold.
 */

#include "../harness.h"
#include <string.h>
#include <stdio.h>

static const char DIR_PATH[] =
    "\\Device\\Harddisk0\\Partition1\\nxkrnl-api-dirmask";

#ifndef FILE_DELETE_ON_CLOSE
#define FILE_DELETE_ON_CLOSE 0x00001000
#endif
#ifndef STATUS_NO_SUCH_FILE
#define STATUS_NO_SUCH_FILE ((NTSTATUS)0xC000000FL)
#endif
#ifndef STATUS_NO_MORE_FILES
#define STATUS_NO_MORE_FILES ((NTSTATUS)0x80000006L)
#endif

static NTSTATUS open_dir(HANDLE *h, ULONG disposition, ULONG extra_opts)
{
    ANSI_STRING name = {
        .Length        = (USHORT)(sizeof(DIR_PATH) - 1),
        .MaximumLength = sizeof(DIR_PATH),
        .Buffer        = (PCHAR)DIR_PATH,
    };
    OBJECT_ATTRIBUTES oa = {
        .RootDirectory = NULL,
        .ObjectName    = &name,
        .Attributes    = OBJ_CASE_INSENSITIVE,
    };
    IO_STATUS_BLOCK iosb;
    return NtCreateFile(h, GENERIC_READ | SYNCHRONIZE, &oa, &iosb, NULL,
                        FILE_ATTRIBUTE_NORMAL, 0, disposition,
                        FILE_DIRECTORY_FILE | FILE_SYNCHRONOUS_IO_NONALERT |
                        extra_opts);
}

static NTSTATUS make_file(const char *leaf)
{
    char path[128];
    ANSI_STRING name;
    OBJECT_ATTRIBUTES oa;
    IO_STATUS_BLOCK iosb;
    HANDLE h;
    NTSTATUS s;

    strcpy(path, DIR_PATH);
    strcat(path, "\\");
    strcat(path, leaf);
    name.Buffer = path;
    name.Length = (USHORT)strlen(path);
    name.MaximumLength = name.Length + 1;
    oa.RootDirectory = NULL;
    oa.ObjectName = &name;
    oa.Attributes = OBJ_CASE_INSENSITIVE;

    s = NtCreateFile(&h, GENERIC_WRITE | SYNCHRONIZE, &oa, &iosb, NULL,
                     FILE_ATTRIBUTE_NORMAL, 0, FILE_OVERWRITE_IF,
                     FILE_NON_DIRECTORY_FILE | FILE_SYNCHRONOUS_IO_NONALERT);
    if (NT_SUCCESS(s))
        NtClose(h);
    return s;
}

static char g_matched[160];

/* Count entries matching `mask` via repeated single-entry queries on a
 * fresh handle (NT binds the search pattern to the handle at the first
 * query, so reusing one handle would ignore later masks). */
static int count_matches(const char *mask)
{
    HANDLE dir;
    ANSI_STRING m = {
        .Length        = (USHORT)strlen(mask),
        .MaximumLength = (USHORT)(strlen(mask) + 1),
        .Buffer        = (PCHAR)mask,
    };
    union {
        FILE_DIRECTORY_INFORMATION di;
        /* XAPI sizes this buffer for the FATX max name; retail rejects
         * undersized buffers with STATUS_INVALID_PARAMETER. */
        char raw[sizeof(FILE_DIRECTORY_INFORMATION) + 256];
    } u;
    IO_STATUS_BLOCK iosb;
    BOOLEAN restart = TRUE;
    int n = 0;
    size_t used = 0;

    if (!NT_SUCCESS(open_dir(&dir, FILE_OPEN, 0)))
        return -3;

    g_matched[0] = '\0';
    for (;;) {
        /* XAPI passes the mask only on the first call (FindFirstFile);
         * continuations pass NULL (FindNextFile). */
        NTSTATUS s = NtQueryDirectoryFile(dir, NULL, NULL, NULL, &iosb,
                                          &u.di, sizeof(u),
                                          FileDirectoryInformation,
                                          restart ? &m : NULL, restart);
        restart = FALSE;
        if (s == STATUS_NO_MORE_FILES || s == STATUS_NO_SUCH_FILE)
            break;
        if (!NT_SUCCESS(s)) {
            /* Record the unexpected terminator for diagnosis. */
            snprintf(g_matched + strlen(g_matched),
                     sizeof(g_matched) - strlen(g_matched),
                     "[end=0x%08x] ", (unsigned)s);
            NtClose(dir);
            return -1;
        }
        n++;
        /* Record names so a count mismatch is diagnosable from TAP. */
        {
            size_t len = u.di.FileNameLength;
            if (len > 24) len = 24;
            if (used + len + 2 < sizeof(g_matched)) {
                memcpy(g_matched + used, u.di.FileName, len);
                used += len;
                g_matched[used++] = ' ';
                g_matched[used] = '\0';
            }
        }
        if (n > 64) { NtClose(dir); return -2; }  /* runaway guard */
    }
    NtClose(dir);
    return n;
}

static bool t_heavy_star_mask(void)
{
    HANDLE dir;
    NTSTATUS s;

    /* Create (or open) the scratch directory and populate it. */
    s = open_dir(&dir, FILE_OPEN_IF, 0);
    ASSERT_NTSTATUS(s, STATUS_SUCCESS);
    NtClose(dir);

    ASSERT_NTSTATUS(make_file("aaaaaaaaaaaa.txt"), STATUS_SUCCESS);
    ASSERT_NTSTATUS(make_file("ababababab.txt"),   STATUS_SUCCESS);
    ASSERT_NTSTATUS(make_file("zzz.txt"),          STATUS_SUCCESS);

    /* Retail-validated truths:
     *   "*"        -> exactly the 3 files (NO fabricated . / .. entries)
     *   "zzz.txt"  -> 1   (masks are honored)
     *   "a*"       -> 2
     *   "*.txt"    -> 3
     *   12-star    -> first call fails STATUS_INVALID_PARAMETER: retail
     *                 bounds pattern complexity and REJECTS rather than
     *                 risking its fixed backtracking buffer.  Any
     *                 non-crash outcome (clean rejection or a correct
     *                 match) is acceptable here; silent overflow is not. */
    {
        int n = count_matches("*");
        if (n != 3) FAIL_AND_RETURN("'*' matched %d, expected 3: %s",
                                    n, g_matched);
    }
    {
        int n = count_matches("zzz.txt");
        if (n != 1) FAIL_AND_RETURN("exact matched %d, expected 1: %s",
                                    n, g_matched);
    }
    {
        int n = count_matches("a*");
        if (n != 2) FAIL_AND_RETURN("'a*' matched %d, expected 2: %s",
                                    n, g_matched);
    }
    {
        int n = count_matches("*.txt");
        if (n != 3) FAIL_AND_RETURN("'*.txt' matched %d, expected 3: %s",
                                    n, g_matched);
    }
    {
        /* 12 '*a*' groups need >=12 interspersed 'a's: only the 12-a
         * file qualifies (ababababab has 5).  nxkrnl's dynamic-buffer
         * matcher returns exactly that 1; retail bounds pattern
         * complexity and rejects with INVALID_PARAMETER (-1 here).
         * Both are fine -- a silent overflow/crash or a wrong match
         * set is not. */
        int n = count_matches("*a*a*a*a*a*a*a*a*a*a*a*a*");
        if (n != -1 && n != 1)
            FAIL_AND_RETURN("heavy mask matched %d (expected 1 or a "
                            "clean reject): %s", n, g_matched);
    }

    return true;
}

/* DOS-style wildcards (DOS_STAR '<', DOS_QM '>', DOS_DOT '"') are NT
 * short-name machinery; FATX has no short names and retail treats them
 * as ordinary (unmatchable) literals.  Retail probe against
 * {aaaaaaaaaaaa.txt, ababababab.txt, zzz.txt}-shaped
 * directories: every DOS-wildcard mask matches ZERO entries, while the
 * NT matcher's DOS arms would match 1-3.  Pins the retail behavior the
 * FsRtlIsNameInExpressionPrivate DOS-arm trim restores. */
static bool t_dos_wildcards_literal(void)
{
    HANDLE dir;
    NTSTATUS s;

    /* Same scratch population as heavy_star_mask; idempotent. */
    s = open_dir(&dir, FILE_OPEN_IF, 0);
    ASSERT_NTSTATUS(s, STATUS_SUCCESS);
    NtClose(dir);
    ASSERT_NTSTATUS(make_file("aaaaaaaaaaaa.txt"), STATUS_SUCCESS);
    ASSERT_NTSTATUS(make_file("ababababab.txt"),   STATUS_SUCCESS);
    ASSERT_NTSTATUS(make_file("zzz.txt"),          STATUS_SUCCESS);

    {
        int n = count_matches("*.txt");  /* sanity: plain stars still work */
        if (n != 3) FAIL_AND_RETURN("'*.txt' matched %d, expected 3: %s",
                                    n, g_matched);
    }
    {
        int n = count_matches("<.txt");  /* DOS_STAR */
        if (n != 0) FAIL_AND_RETURN("'<.txt' matched %d, expected 0 "
                                    "(retail): %s", n, g_matched);
    }
    {
        int n = count_matches(">>>.txt");  /* DOS_QM x3 */
        if (n != 0) FAIL_AND_RETURN("'>>>.txt' matched %d, expected 0 "
                                    "(retail): %s", n, g_matched);
    }
    {
        int n = count_matches("zzz\"txt");  /* DOS_DOT */
        if (n != 0) FAIL_AND_RETURN("'zzz\"txt' matched %d, expected 0 "
                                    "(retail): %s", n, g_matched);
    }
    {
        int n = count_matches("<\"txt");  /* DOS_STAR + DOS_DOT */
        if (n != 0) FAIL_AND_RETURN("'<\"txt' matched %d, expected 0 "
                                    "(retail): %s", n, g_matched);
    }
    return true;
}

static const test_entry_t io_dirmask_entries[] = {
    {"heavy_star_mask", t_heavy_star_mask},
    {"dos_wildcards_literal", t_dos_wildcards_literal},
};

DEFINE_GROUP(io_dirmask, "io/dirmask");
