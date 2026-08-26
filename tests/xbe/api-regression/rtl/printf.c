/*
 * Rtl printf-family exports (RtlSprintf / RtlSnprintf / RtlVsprintf /
 * RtlVsnprintf) whose ordinals were unmapped bugcheck stubs.  Each
 * forwards to the kernel's runtime vsnprintf; the tests pin the common
 * conversion set and the snprintf byte bound.
 *
 * nxdk declares the va_list forms as variadic (...), so we reach them
 * through a correctly-typed function pointer to avoid passing a va_list
 * where the header expects inline varargs.
 */

#include "../harness.h"
#include <stdarg.h>
#include <string.h>

typedef void(__cdecl *vspr_t)(char *, const char *, va_list);
typedef void(__cdecl *vsnpr_t)(char *, size_t, const char *, va_list);

static void call_vsprintf(char *buf, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    ((vspr_t)(void *)&RtlVsprintf)(buf, fmt, ap);
    va_end(ap);
}

static void call_vsnprintf(char *buf, size_t count, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    ((vsnpr_t)(void *)&RtlVsnprintf)(buf, count, fmt, ap);
    va_end(ap);
}

static bool t_sprintf(void)
{
    char buf[64];

    memset(buf, 0x7f, sizeof(buf));
    RtlSprintf(buf, "d=%d x=%08x s=%s c=%c u=%u", -5, 0xabcd, "hi", 'Z',
               3000000000u);
    if (strcmp(buf, "d=-5 x=0000abcd s=hi c=Z u=3000000000") != 0)
        FAIL_AND_RETURN("got \"%s\"", buf);
    return true;
}

static bool t_snprintf_fits(void)
{
    char buf[64];

    RtlSnprintf(buf, sizeof(buf), "%d/%d", 22, 7);
    if (strcmp(buf, "22/7") != 0)
        FAIL_AND_RETURN("got \"%s\"", buf);
    return true;
}

/* When the output is longer than the bound, exactly Count chars are
 * written and the tail byte past the bound is left untouched (no NUL). */
static bool t_snprintf_truncates(void)
{
    char buf[16];

    memset(buf, '#', sizeof(buf));
    RtlSnprintf(buf, 5, "%s", "abcdefgh");
    if (memcmp(buf, "abcde", 5) != 0)
        FAIL_AND_RETURN("prefix \"%.5s\"", buf);
    ASSERT_EQ_U32((uint8_t)buf[5], (uint8_t)'#');
    return true;
}

static bool t_vsprintf(void)
{
    char buf[64];

    call_vsprintf(buf, "%s=%ld", "n", -12345L);
    if (strcmp(buf, "n=-12345") != 0)
        FAIL_AND_RETURN("got \"%s\"", buf);
    return true;
}

static bool t_vsnprintf(void)
{
    char buf[64];

    call_vsnprintf(buf, sizeof(buf), "%c%c%c-%x", 'A', 'B', 'C', 255);
    if (strcmp(buf, "ABC-ff") != 0)
        FAIL_AND_RETURN("got \"%s\"", buf);
    return true;
}

static const test_entry_t rtl_printf_entries[] = {
    {"sprintf", t_sprintf},
    {"snprintf_fits", t_snprintf_fits},
    {"snprintf_truncates", t_snprintf_truncates},
    {"vsprintf", t_vsprintf},
    {"vsnprintf", t_vsnprintf},
};

DEFINE_GROUP(rtl_printf, "rtl/printf");
