/*
 * libc mem* shims for the freestanding loader.  gcc emits implicit memcpy/
 * memset calls when lowering struct copies and {0} zero-inits, and the XZ
 * decoder calls memmove/memcmp; the no-libc build has to provide all four.
 * With builtins enabled, gcc inlines small fixed-size calls into mov sequences.
 */

#include "nxldr.h"

void *
memcpy(void *Dest, const void *Src, ULONG Count)
{
    UCHAR *d = (UCHAR *)Dest;
    const UCHAR *s = (const UCHAR *)Src;

    /* Word-at-a-time when aligned, byte-at-a-time on tails. */
    while (Count >= 4 && (((ULONG_PTR)d & 3) == 0) && (((ULONG_PTR)s & 3) == 0))
    {
        *(ULONG *)d = *(const ULONG *)s;
        d += 4;
        s += 4;
        Count -= 4;
    }
    while (Count-- > 0)
        *d++ = *s++;
    return Dest;
}

void *
memset(void *Dest, int Value, ULONG Count)
{
    UCHAR *d = (UCHAR *)Dest;
    UCHAR b = (UCHAR)Value;

    while (Count-- > 0)
        *d++ = b;
    return Dest;
}

void *
memmove(void *Dest, const void *Src, ULONG Count)
{
    UCHAR *d = (UCHAR *)Dest;
    const UCHAR *s = (const UCHAR *)Src;

    if (d < s)
        return memcpy(Dest, Src, Count);
    while (Count-- > 0)
        d[Count] = s[Count];
    return Dest;
}

int
memcmp(const void *Buffer1, const void *Buffer2, ULONG Count)
{
    const UCHAR *x = (const UCHAR *)Buffer1;
    const UCHAR *y = (const UCHAR *)Buffer2;
    ULONG i;

    for (i = 0; i < Count; i++)
    {
        if (x[i] != y[i])
            return x[i] < y[i] ? -1 : 1;
    }
    return 0;
}
