/*
 * PROJECT:     nxkrnl -- a free kernel for the original Xbox
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Rtl*ByteSwap ordinals -- endianness reversal of a 16/32-bit
 *              value.  Exported FASTCALL (the value arrives in ECX and the
 *              swapped value returns in EAX); no NT/ReactOS source ships
 *              these as real functions (they are compiler intrinsics there),
 *              so implement them directly.
 */

#include <ntdef.h>

USHORT FASTCALL
RtlUshortByteSwap(_In_ USHORT Source)
{
    return (USHORT)((Source >> 8) | (Source << 8));
}

ULONG FASTCALL
RtlUlongByteSwap(_In_ ULONG Source)
{
    return (Source >> 24)
         | ((Source >> 8) & 0x0000FF00UL)
         | ((Source << 8) & 0x00FF0000UL)
         | (Source << 24);
}
