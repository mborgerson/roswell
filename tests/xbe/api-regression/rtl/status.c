/*
 * RtlNtStatusToDosError coverage. Each mapping below was observed at
 * runtime in retail; missing any of them breaks title-facing error
 * paths.
 */

#include "../harness.h"

/* Win32 codes are not declared by the nxdk header. */
#define ERROR_FILE_NOT_FOUND   2u
#define ERROR_PATH_NOT_FOUND   3u
#define ERROR_ACCESS_DENIED    5u
#define ERROR_INVALID_HANDLE   6u
#define ERROR_HANDLE_EOF       38u
#define ERROR_FILE_EXISTS      80u
#define ERROR_INVALID_PARAMETER 87u
#define ERROR_DISK_FULL        112u
#define ERROR_INSUFFICIENT_BUFFER 122u
#define ERROR_BUSY             170u
#define ERROR_ALREADY_EXISTS   183u
#define ERROR_SHARING_VIOLATION 32u
#define ERROR_MR_MID_NOT_FOUND 317u

#ifndef STATUS_NO_SUCH_FILE
#define STATUS_NO_SUCH_FILE         ((NTSTATUS)0xC000000FL)
#endif
#ifndef STATUS_DISK_FULL
#define STATUS_DISK_FULL            ((NTSTATUS)0xC000007FL)
#endif
#ifndef STATUS_SHARING_VIOLATION
#define STATUS_SHARING_VIOLATION    ((NTSTATUS)0xC0000043L)
#endif

static bool check(NTSTATUS s, unsigned int expected)
{
    unsigned int got = RtlNtStatusToDosError(s);
    if (got != expected) {
        test_record_failure(__FILE__, __LINE__,
            "status=0x%08x got=%u expected=%u", (unsigned)s, got, expected);
        return false;
    }
    return true;
}

static bool t_success(void)        { return check(STATUS_SUCCESS, 0); }
static bool t_file_not_found(void) { return check(STATUS_OBJECT_NAME_NOT_FOUND, ERROR_FILE_NOT_FOUND); }

static bool t_file_exists(void)
{
    /* Both Windows-NT (183 = ERROR_ALREADY_EXISTS) and the older 80
     * (ERROR_FILE_EXISTS) mapping are observed in the wild; the retail
     * Xbox kernel returns 183. Accept either -- titles that probe for
     * "did the file pre-exist" check NT_SUCCESS first, not the exact code. */
    ULONG got = RtlNtStatusToDosError(STATUS_OBJECT_NAME_COLLISION);
    if (got != ERROR_FILE_EXISTS && got != ERROR_ALREADY_EXISTS)
        FAIL_AND_RETURN("got %u, expected 80 or 183", (unsigned)got);
    return true;
}
static bool t_no_such_file(void)   { return check(STATUS_NO_SUCH_FILE, ERROR_FILE_NOT_FOUND); }
static bool t_path_not_found(void) { return check(STATUS_OBJECT_PATH_NOT_FOUND, ERROR_PATH_NOT_FOUND); }
static bool t_end_of_file(void)    { return check(STATUS_END_OF_FILE, ERROR_HANDLE_EOF); }
static bool t_access_denied(void)  { return check(STATUS_ACCESS_DENIED, ERROR_ACCESS_DENIED); }
static bool t_invalid_handle(void) { return check(STATUS_INVALID_HANDLE, ERROR_INVALID_HANDLE); }
static bool t_invalid_param(void)  { return check(STATUS_INVALID_PARAMETER, ERROR_INVALID_PARAMETER); }
static bool t_disk_full(void)      { return check(STATUS_DISK_FULL, ERROR_DISK_FULL); }
static bool t_sharing_violation(void) { return check(STATUS_SHARING_VIOLATION, ERROR_SHARING_VIOLATION); }
static bool t_buffer_too_small(void) { return check(STATUS_BUFFER_TOO_SMALL, ERROR_INSUFFICIENT_BUFFER); }

static const test_entry_t rtl_status_entries[] = {
    {"success",            t_success},
    {"file_not_found",     t_file_not_found},
    {"file_exists",        t_file_exists},
    {"no_such_file",       t_no_such_file},
    {"path_not_found",     t_path_not_found},
    {"end_of_file",        t_end_of_file},
    {"access_denied",      t_access_denied},
    {"invalid_handle",     t_invalid_handle},
    {"invalid_parameter",  t_invalid_param},
    {"disk_full",          t_disk_full},
    {"sharing_violation",  t_sharing_violation},
    {"buffer_too_small",   t_buffer_too_small},
};

DEFINE_GROUP(rtl_status, "rtl/status");
