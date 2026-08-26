/*
 * Xbox Rtl printf-family exports.  The Xbox kernel exports C-style
 * sprintf/snprintf and their va_list forms; forward each onto the same
 * runtime vsnprintf the kernel's own DbgPrint path uses so titles get
 * identical formatting.  Return the character count, like retail.
 */

#include <ntdef.h>
#include <stdarg.h>
#include <stdio.h>

int __cdecl
RtlVsnprintf(char *Buffer, size_t Count, const char *Format, va_list Args)
{
    return _vsnprintf(Buffer, Count, Format, Args);
}

int __cdecl
RtlVsprintf(char *Buffer, const char *Format, va_list Args)
{
    return _vsnprintf(Buffer, (size_t)-1, Format, Args);
}

int __cdecl
RtlSnprintf(char *Buffer, size_t Count, const char *Format, ...)
{
    va_list ap;
    int ret;

    va_start(ap, Format);
    ret = _vsnprintf(Buffer, Count, Format, ap);
    va_end(ap);
    return ret;
}

int __cdecl
RtlSprintf(char *Buffer, const char *Format, ...)
{
    va_list ap;
    int ret;

    va_start(ap, Format);
    ret = _vsnprintf(Buffer, (size_t)-1, Format, ap);
    va_end(ap);
    return ret;
}
