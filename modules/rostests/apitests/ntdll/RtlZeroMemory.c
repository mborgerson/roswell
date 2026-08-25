/*
 * PROJECT:         ReactOS api tests
 * LICENSE:         LGPLv2.1+ - See COPYING.LIB in the top level directory
 * PURPOSE:         Test for RtlZeroMemory
 * PROGRAMMERS:     ReactOS Team
 */

#include "precomp.h"

START_TEST(RtlZeroMemory)
{
    UCHAR Buffer[32];
    SIZE_T i;

    /* Fill buffer with non-zero pattern, then zero it */
    RtlFillMemory(Buffer, sizeof(Buffer), 0xAA);
    RtlZeroMemory(Buffer, sizeof(Buffer));
    for (i = 0; i < sizeof(Buffer); i++)
    {
        ok(Buffer[i] == 0, "Buffer[%lu] = 0x%x, expected 0\n", (ULONG)i, Buffer[i]);
    }

    /* Zero a partial region at the beginning */
    RtlFillMemory(Buffer, sizeof(Buffer), 0xBB);
    RtlZeroMemory(Buffer, 16);
    for (i = 0; i < 16; i++)
    {
        ok(Buffer[i] == 0, "Buffer[%lu] = 0x%x, expected 0\n", (ULONG)i, Buffer[i]);
    }
    for (i = 16; i < sizeof(Buffer); i++)
    {
        ok(Buffer[i] == 0xBB, "Buffer[%lu] = 0x%x, expected 0xBB\n", (ULONG)i, Buffer[i]);
    }

    /* Zero a partial region in the middle */
    RtlFillMemory(Buffer, sizeof(Buffer), 0xCC);
    RtlZeroMemory(Buffer + 8, 16);
    for (i = 0; i < 8; i++)
    {
        ok(Buffer[i] == 0xCC, "Buffer[%lu] = 0x%x, expected 0xCC\n", (ULONG)i, Buffer[i]);
    }
    for (i = 8; i < 24; i++)
    {
        ok(Buffer[i] == 0, "Buffer[%lu] = 0x%x, expected 0\n", (ULONG)i, Buffer[i]);
    }
    for (i = 24; i < sizeof(Buffer); i++)
    {
        ok(Buffer[i] == 0xCC, "Buffer[%lu] = 0x%x, expected 0xCC\n", (ULONG)i, Buffer[i]);
    }

    /* Zero with length 0 should not modify the buffer */
    RtlFillMemory(Buffer, sizeof(Buffer), 0xDD);
    RtlZeroMemory(Buffer, 0);
    for (i = 0; i < sizeof(Buffer); i++)
    {
        ok(Buffer[i] == 0xDD, "Buffer[%lu] = 0x%x, expected 0xDD\n", (ULONG)i, Buffer[i]);
    }

    /* Zero exactly 1 byte */
    RtlFillMemory(Buffer, sizeof(Buffer), 0xEE);
    RtlZeroMemory(Buffer + 15, 1);
    ok(Buffer[14] == 0xEE, "Buffer[14] = 0x%x, expected 0xEE\n", Buffer[14]);
    ok(Buffer[15] == 0, "Buffer[15] = 0x%x, expected 0\n", Buffer[15]);
    ok(Buffer[16] == 0xEE, "Buffer[16] = 0x%x, expected 0xEE\n", Buffer[16]);
}
