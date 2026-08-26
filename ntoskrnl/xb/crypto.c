/*
 * PROJECT:     nxkrnl -- a free kernel for the original Xbox
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Xc* crypto ordinals -- SHA-1, RC4, HMAC-SHA1, plus stubs for
 *              the cipher framework (DES key parity / key table /
 *              block-crypt) and modular exponentiation.
 *
 * Titles sign and verify HDD cache content with these; a no-op leaves
 * them consuming a zeroed digest / unencrypted keystream and crashes
 * inside the title's signature path.  RC4 + HMAC-SHA1 construction per
 * cxbx-reloaded EmuKrnlXc.cpp; SHA-1 / RC4 cores come from sdk/lib/cryptlib.
 */

#include <ntdef.h>
#include <ntifs.h>

#include <sha1.h>
#include <rc4.h>

/*
 * XcSHA*: caller allocates the SHA context.  The Xbox layout reserves a
 * 24-byte header the algorithm doesn't use and keeps the SHA-1 state at
 * offset +24.  SHA_CTX is 92 bytes, fitting the slot.
 */
#define XC_SHA_CTX(c)  ((PSHA_CTX)((PUCHAR)(c) + 24))

VOID NTAPI XcSHAInit(_In_ PVOID Ctx)
{ A_SHAInit(XC_SHA_CTX(Ctx)); }
VOID NTAPI XcSHAUpdate(_In_ PVOID Ctx, _In_ PVOID Data, _In_ ULONG Len)
{ A_SHAUpdate(XC_SHA_CTX(Ctx), (const unsigned char *)Data, Len); }
VOID NTAPI XcSHAFinal(_In_ PVOID Ctx, _Out_writes_(20) PVOID Digest)
{ A_SHAFinal(XC_SHA_CTX(Ctx), (PULONG)Digest); }

/* RC4 key schedule.  RC4_CONTEXT (258 B) fits inside the key-struct slot. */
VOID NTAPI
XcRC4Key(_Out_ PVOID KeyStruct, _In_ ULONG KeyLen, _In_ PVOID KeyData)
{
    rc4_init((RC4_CONTEXT *)KeyStruct,
             (const unsigned char *)KeyData, KeyLen);
}

/* RC4 transform, in place. */
VOID NTAPI
XcRC4Crypt(_In_ PVOID KeyStruct, _In_ ULONG Len, _Inout_ PVOID Data)
{
    rc4_crypt((RC4_CONTEXT *)KeyStruct, (unsigned char *)Data, Len);
}

/* HMAC-SHA1 over two data segments:
 *   inner = SHA1(ipad || I1 || I2);  Out = SHA1(opad || inner). */
VOID NTAPI
XcHMAC(_In_ PVOID K, _In_ ULONG Kl, _In_ PVOID I1, _In_ ULONG L1,
       _In_ PVOID I2, _In_ ULONG L2, _Out_writes_(20) PVOID Out)
{
    UCHAR pad1[64], pad2[64], temp[64 + 20];
    SHA_CTX c;
    ULONG i;

    if (Kl > 64) Kl = 64;
    RtlZeroMemory(pad1, sizeof(pad1));
    RtlZeroMemory(pad2, sizeof(pad2));
    RtlCopyMemory(pad1, K, Kl);
    RtlCopyMemory(pad2, K, Kl);
    for (i = 0; i < 64 / sizeof(ULONG); i++)
    {
        ((PULONG)pad1)[i] ^= 0x36363636;
        ((PULONG)pad2)[i] ^= 0x5C5C5C5C;
    }

    A_SHAInit(&c);
    A_SHAUpdate(&c, pad1, 64);
    if (L1) A_SHAUpdate(&c, (const unsigned char *)I1, L1);
    if (L2) A_SHAUpdate(&c, (const unsigned char *)I2, L2);
    A_SHAFinal(&c, (PULONG)(temp + 64));
    RtlCopyMemory(temp, pad2, 64);

    A_SHAInit(&c);
    A_SHAUpdate(&c, temp, sizeof(temp));
    A_SHAFinal(&c, (PULONG)Out);
}

/*
 * Cipher framework + modular exponentiation -- ABI-correct stubs.  Real
 * implementations belong on top of the same primitives once a title
 * exercises them; this lets the thunk-table call land on a balanced
 * function and the title continue.
 */
ULONG NTAPI
XcModExp(_Out_ PVOID Out, _In_ PVOID In, _In_ PVOID Mod,
         _In_ PVOID Exp, _In_ ULONG Len)
{
    UNREFERENCED_PARAMETER(In);
    UNREFERENCED_PARAMETER(Mod);
    UNREFERENCED_PARAMETER(Exp);
    RtlZeroMemory(Out, Len);
    return 0;
}

/*
 * Force odd parity on every key byte: the low bit is the parity bit, set
 * so each byte carries an odd number of 1s.  The upper seven bits are the
 * key material and are left untouched.
 */
VOID NTAPI
XcDESKeyParity(_Inout_ PVOID Key, _In_ ULONG KeyLen)
{
    PUCHAR k = (PUCHAR)Key;
    ULONG i;

    for (i = 0; i < KeyLen; i++)
    {
        UCHAR b = k[i] & 0xFE;
        UCHAR p = b;
        p ^= p >> 4;
        p ^= p >> 2;
        p ^= p >> 1;
        k[i] = b | ((p & 1) ^ 1);
    }
}

VOID NTAPI
XcKeyTable(_In_ ULONG Cipher, _Out_ PVOID KeyTable, _In_ PVOID Key)
{
    UNREFERENCED_PARAMETER(Cipher);
    UNREFERENCED_PARAMETER(Key);
    RtlZeroMemory(KeyTable, 256);
}

VOID NTAPI
XcBlockCryptCBC(_In_ ULONG Cipher, _In_ ULONG Len, _Out_ PVOID Out,
                _In_ PVOID In, _In_ PVOID KeyTable, _In_ ULONG Op,
                _Inout_ PVOID Feedback)
{
    UNREFERENCED_PARAMETER(Cipher);
    UNREFERENCED_PARAMETER(KeyTable);
    UNREFERENCED_PARAMETER(Op);
    UNREFERENCED_PARAMETER(Feedback);
    if (Out && In && Len)
        RtlCopyMemory(Out, In, Len);
}

/* EOF */
