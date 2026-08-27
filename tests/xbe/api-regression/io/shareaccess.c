/*
 * The share-access trio a title's block-device driver uses to arbitrate
 * opens of its own device: IoSetShareAccess, IoCheckShareAccess and
 * IoRemoveShareAccess.
 *
 * The SHARE_ACCESS is allocated by the CALLER, so its width is ABI: the
 * console counts opens in bytes where an NT kernel counts them in dwords.
 * A guard byte after the structure catches a kernel writing the wide
 * form, which would otherwise pass every value check while quietly
 * scribbling 21 bytes past the end of the caller's field.
 *
 * The file object is a zeroed buffer of the title's own rather than a
 * real one, so the calls cannot disturb a file system's live accounting.
 */

#include "../harness.h"
#include <string.h>

#ifndef FILE_READ_DATA
#define FILE_READ_DATA   0x0001
#define FILE_WRITE_DATA  0x0002
#endif

#ifndef STATUS_SHARING_VIOLATION
#define STATUS_SHARING_VIOLATION ((NTSTATUS)0xC0000043L)
#endif

#define SA_GUARD 0xCC

/* Big enough for any kernel's file object; only the head is looked at. */
#define FO_BYTES 256
#define FO_ACCESS_BITS 0x02   /* access bitfield byte */

#define FO_READ_ACCESS    0x02
#define FO_WRITE_ACCESS   0x04
#define FO_SHARED_READ    0x10
#define FO_SHARED_WRITE   0x20

typedef struct {
    SHARE_ACCESS access;
    UCHAR guard[16];
} guarded_sa_t;

static UCHAR g_fo[FO_BYTES];

static PFILE_OBJECT fresh_file_object(void)
{
    memset(g_fo, 0, sizeof(g_fo));
    return (PFILE_OBJECT)g_fo;
}

static void fresh_share_access(guarded_sa_t *g)
{
    memset(g, 0, sizeof(*g));
    memset(g->guard, SA_GUARD, sizeof(g->guard));
}

static bool guard_intact(const guarded_sa_t *g)
{
    unsigned i;
    for (i = 0; i < sizeof(g->guard); i++)
        if (g->guard[i] != SA_GUARD)
            return false;
    return true;
}

static bool counts_are(const SHARE_ACCESS *sa, unsigned open, unsigned rd,
                       unsigned wr, unsigned del, unsigned srd, unsigned swr,
                       unsigned sdel)
{
    return sa->OpenCount == open && sa->Readers == rd && sa->Writers == wr &&
           sa->Deleters == del && sa->SharedRead == srd &&
           sa->SharedWrite == swr && sa->SharedDelete == sdel;
}

#define ASSERT_COUNTS(g, o, r, w, d, sr, sw, sd) do { \
    const SHARE_ACCESS *_s = &(g).access; \
    if (!counts_are(_s, o, r, w, d, sr, sw, sd)) \
        FAIL_AND_RETURN("counts open=%u r=%u w=%u d=%u sr=%u sw=%u sd=%u", \
                        _s->OpenCount, _s->Readers, _s->Writers, \
                        _s->Deleters, _s->SharedRead, _s->SharedWrite, \
                        _s->SharedDelete); \
    if (!guard_intact(&(g))) \
        FAIL_AND_RETURN("kernel wrote past the 7-byte SHARE_ACCESS"); \
} while (0)

static bool t_set_records_the_open(void)
{
    guarded_sa_t g;
    PFILE_OBJECT fo = fresh_file_object();

    fresh_share_access(&g);
    IoSetShareAccess(FILE_READ_DATA, FILE_SHARE_READ, fo, &g.access);
    ASSERT_COUNTS(g, 1, 1, 0, 0, 1, 0, 0);

    /* The requested access lands in the file object's own bitfield byte,
     * which is where a file system reads it back at close. */
    ASSERT_EQ_U32(g_fo[FO_ACCESS_BITS], FO_READ_ACCESS | FO_SHARED_READ);
    return true;
}

static bool t_set_read_write_shared(void)
{
    guarded_sa_t g;
    PFILE_OBJECT fo = fresh_file_object();

    fresh_share_access(&g);
    IoSetShareAccess(FILE_READ_DATA | FILE_WRITE_DATA,
                     FILE_SHARE_READ | FILE_SHARE_WRITE, fo, &g.access);
    ASSERT_COUNTS(g, 1, 1, 1, 0, 1, 1, 0);
    ASSERT_EQ_U32(g_fo[FO_ACCESS_BITS],
                  FO_READ_ACCESS | FO_WRITE_ACCESS |
                  FO_SHARED_READ | FO_SHARED_WRITE);
    return true;
}

/* An open asking for no data access at all resets the field instead of
 * counting: it is the initializer a driver calls on its first open. */
static bool t_set_no_access_resets(void)
{
    guarded_sa_t g;
    PFILE_OBJECT fo = fresh_file_object();

    fresh_share_access(&g);
    g.access.OpenCount = 3;
    g.access.Readers = 3;
    g.access.SharedRead = 3;

    IoSetShareAccess(0, 0, fo, &g.access);
    ASSERT_COUNTS(g, 0, 0, 0, 0, 0, 0, 0);
    return true;
}

static bool t_check_grants_compatible(void)
{
    guarded_sa_t g;
    PFILE_OBJECT first = fresh_file_object();
    static UCHAR second_fo[FO_BYTES];
    NTSTATUS s;

    fresh_share_access(&g);
    IoSetShareAccess(FILE_READ_DATA, FILE_SHARE_READ, first, &g.access);

    /* A second reader that also permits sharing is compatible, and
     * Update folds it into the counts. */
    memset(second_fo, 0, sizeof(second_fo));
    s = IoCheckShareAccess(FILE_READ_DATA, FILE_SHARE_READ,
                           (PFILE_OBJECT)second_fo, &g.access, TRUE);
    ASSERT_NTSTATUS(s, STATUS_SUCCESS);
    ASSERT_COUNTS(g, 2, 2, 0, 0, 2, 0, 0);
    return true;
}

/* Update == FALSE answers the question without recording the open. */
static bool t_check_without_update_leaves_counts(void)
{
    guarded_sa_t g;
    PFILE_OBJECT first = fresh_file_object();
    static UCHAR second_fo[FO_BYTES];
    NTSTATUS s;

    fresh_share_access(&g);
    IoSetShareAccess(FILE_READ_DATA, FILE_SHARE_READ, first, &g.access);

    memset(second_fo, 0, sizeof(second_fo));
    s = IoCheckShareAccess(FILE_READ_DATA, FILE_SHARE_READ,
                           (PFILE_OBJECT)second_fo, &g.access, FALSE);
    ASSERT_NTSTATUS(s, STATUS_SUCCESS);
    ASSERT_COUNTS(g, 1, 1, 0, 0, 1, 0, 0);
    return true;
}

static bool t_check_denies_conflicting(void)
{
    guarded_sa_t g;
    PFILE_OBJECT first = fresh_file_object();
    static UCHAR second_fo[FO_BYTES];
    NTSTATUS s;

    fresh_share_access(&g);
    /* An exclusive reader: no sharing permitted. */
    IoSetShareAccess(FILE_READ_DATA, 0, first, &g.access);

    memset(second_fo, 0, sizeof(second_fo));
    s = IoCheckShareAccess(FILE_READ_DATA, FILE_SHARE_READ,
                           (PFILE_OBJECT)second_fo, &g.access, TRUE);
    ASSERT_NTSTATUS(s, STATUS_SHARING_VIOLATION);
    /* A refused open must not be counted, Update or not. */
    ASSERT_COUNTS(g, 1, 1, 0, 0, 0, 0, 0);
    return true;
}

static bool t_remove_unwinds_the_open(void)
{
    guarded_sa_t g;
    PFILE_OBJECT fo = fresh_file_object();

    fresh_share_access(&g);
    IoSetShareAccess(FILE_READ_DATA | FILE_WRITE_DATA,
                     FILE_SHARE_READ | FILE_SHARE_WRITE, fo, &g.access);
    ASSERT_COUNTS(g, 1, 1, 1, 0, 1, 1, 0);

    IoRemoveShareAccess(fo, &g.access);
    ASSERT_COUNTS(g, 0, 0, 0, 0, 0, 0, 0);
    return true;
}

/* Removing one of two opens leaves the other's grant standing. */
static bool t_remove_one_of_two(void)
{
    guarded_sa_t g;
    PFILE_OBJECT first = fresh_file_object();
    static UCHAR second_fo[FO_BYTES];
    NTSTATUS s;

    fresh_share_access(&g);
    IoSetShareAccess(FILE_READ_DATA, FILE_SHARE_READ, first, &g.access);

    memset(second_fo, 0, sizeof(second_fo));
    s = IoCheckShareAccess(FILE_READ_DATA, FILE_SHARE_READ,
                           (PFILE_OBJECT)second_fo, &g.access, TRUE);
    ASSERT_NTSTATUS(s, STATUS_SUCCESS);

    IoRemoveShareAccess((PFILE_OBJECT)second_fo, &g.access);
    ASSERT_COUNTS(g, 1, 1, 0, 0, 1, 0, 0);
    return true;
}

static const test_entry_t io_shareaccess_entries[] = {
    {"set_records_the_open",       t_set_records_the_open},
    {"set_read_write_shared",      t_set_read_write_shared},
    {"set_no_access_resets",       t_set_no_access_resets},
    {"check_grants_compatible",    t_check_grants_compatible},
    {"check_without_update",       t_check_without_update_leaves_counts},
    {"check_denies_conflicting",   t_check_denies_conflicting},
    {"remove_unwinds_the_open",    t_remove_unwinds_the_open},
    {"remove_one_of_two",          t_remove_one_of_two},
};

DEFINE_GROUP(io_shareaccess, "io/shareaccess");
