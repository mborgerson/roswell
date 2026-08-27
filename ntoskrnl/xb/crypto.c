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

/* --- modular exponentiation ---------------------------------------------- *
 * Operands are arrays of 32-bit words, least significant word first, all
 * the same length.  Result = Base^Exponent mod Modulus.
 *
 * RSA moduli are odd, so the hot path is Montgomery multiplication.  An
 * even modulus has no Montgomery inverse and reduces each product one bit
 * at a time instead -- correct, and far slower, but no key takes it.
 *
 * The word count is small (64 words for a 2048-bit key), but with several
 * temporaries live at once they are still too big for the stack.
 */

/* Squaring costs a multiplication per exponent bit; refuse operand sizes
 * no key would ever use rather than spin. */
#define BN_MAX_WORDS 512

static ULONG
BnCompare(const ULONG *A, const ULONG *B, ULONG N)
{
    while (N-- > 0)
    {
        if (A[N] != B[N])
            return A[N] > B[N] ? 1 : (ULONG)-1;
    }
    return 0;
}

/* A -= B, returning the borrow out of the top word. */
static ULONG
BnSubtract(PULONG A, const ULONG *B, ULONG N)
{
    ULONGLONG Borrow = 0;
    ULONG i;

    for (i = 0; i < N; i++)
    {
        ULONGLONG D = (ULONGLONG)A[i] - B[i] - Borrow;
        A[i] = (ULONG)D;
        Borrow = (D >> 32) & 1;
    }
    return (ULONG)Borrow;
}

/* A = (2A + Bit) mod M, for A below M. */
static VOID
BnDoubleMod(PULONG A, ULONG Bit, const ULONG *M, ULONG N)
{
    ULONG Carry = Bit;
    ULONG i;

    for (i = 0; i < N; i++)
    {
        ULONG Next = A[i] >> 31;
        A[i] = (A[i] << 1) | Carry;
        Carry = Next;
    }
    if (Carry != 0 || BnCompare(A, M, N) != (ULONG)-1)
        BnSubtract(A, M, N);
}

/* Result = Value mod M, shifting Value in from the top a bit at a time. */
static VOID
BnReduce(PULONG Result, const ULONG *Value, ULONG ValueWords,
         const ULONG *M, ULONG N)
{
    ULONG i;

    RtlZeroMemory(Result, N * sizeof(ULONG));
    for (i = ValueWords * 32; i-- > 0;)
        BnDoubleMod(Result, (Value[i / 32] >> (i % 32)) & 1, M, N);
}

/*
 * Montgomery product: T = A * B * 2^-32N mod M, for A and B below M.
 * T holds N + 2 words.
 */
static VOID
BnMontMul(PULONG T, const ULONG *A, const ULONG *B, const ULONG *M,
          ULONG MInv, ULONG N)
{
    ULONG i, j;

    RtlZeroMemory(T, (N + 2) * sizeof(ULONG));
    for (i = 0; i < N; i++)
    {
        ULONGLONG Carry = 0;
        ULONG Factor;

        for (j = 0; j < N; j++)
        {
            Carry += (ULONGLONG)A[j] * B[i] + T[j];
            T[j] = (ULONG)Carry;
            Carry >>= 32;
        }
        Carry += T[N];
        T[N] = (ULONG)Carry;
        T[N + 1] = (ULONG)(Carry >> 32);

        /* Cancel the low word, shifting the accumulator down one place. */
        Factor = T[0] * MInv;
        Carry = ((ULONGLONG)Factor * M[0] + T[0]) >> 32;
        for (j = 1; j < N; j++)
        {
            Carry += (ULONGLONG)Factor * M[j] + T[j];
            T[j - 1] = (ULONG)Carry;
            Carry >>= 32;
        }
        Carry += T[N];
        T[N - 1] = (ULONG)Carry;
        T[N] = T[N + 1] + (ULONG)(Carry >> 32);
    }
    if (T[N] != 0 || BnCompare(T, M, N) != (ULONG)-1)
        BnSubtract(T, M, N);
}

/* Result = A * B mod M for an even M.  Scratch holds 2N words. */
static VOID
BnMulMod(PULONG Result, const ULONG *A, const ULONG *B, const ULONG *M,
         PULONG Scratch, ULONG N)
{
    ULONG i, j;

    RtlZeroMemory(Scratch, 2 * N * sizeof(ULONG));
    for (i = 0; i < N; i++)
    {
        ULONGLONG Carry = 0;
        for (j = 0; j < N; j++)
        {
            Carry += (ULONGLONG)A[j] * B[i] + Scratch[i + j];
            Scratch[i + j] = (ULONG)Carry;
            Carry >>= 32;
        }
        Scratch[i + N] = (ULONG)Carry;
    }
    BnReduce(Result, Scratch, 2 * N, M, N);
}

ULONG NTAPI
XcModExp(_Out_ PULONG Result, _In_ PULONG Base, _In_ PULONG Exponent,
         _In_ PULONG Modulus, _In_ ULONG Words)
{
    PULONG Pool, Acc, Val, Tmp, Scratch;
    ULONG MInv = 0;
    ULONG ExpBits;
    BOOLEAN Odd;
    ULONG i;

    if (Words == 0 || Words > BN_MAX_WORDS)
        return 0;

    /* A zero modulus has nothing to reduce against. */
    for (i = 0; i < Words; i++)
        if (Modulus[i] != 0)
            break;
    if (i == Words)
        return 0;

    Pool = ExAllocatePoolWithTag(NonPagedPool,
                                 (5 * Words + 2) * sizeof(ULONG), 'pXcX');
    if (Pool == NULL)
        return 0;
    Acc = Pool;
    Val = Acc + Words;
    Tmp = Val + Words;                  /* N + 2 words */
    Scratch = Tmp + Words + 2;          /* 2N words */

    Odd = (BOOLEAN)(Modulus[0] & 1);
    BnReduce(Val, Base, Words, Modulus, Words);

    if (Odd)
    {
        /* -Modulus^-1 mod 2^32 by Newton iteration: each step doubles the
         * number of correct low bits, so five cover a word. */
        ULONG Inv = Modulus[0];
        for (i = 0; i < 5; i++)
            Inv *= 2 - Modulus[0] * Inv;
        MInv = (ULONG)(0 - Inv);

        /* Acc = 2^32N mod Modulus is the Montgomery form of one, reached
         * by doubling up from one; doubling it out again gives the
         * conversion factor that puts the base in the same form. */
        RtlZeroMemory(Acc, Words * sizeof(ULONG));
        Acc[0] = 1;
        for (i = 0; i < Words * 32; i++)
            BnDoubleMod(Acc, 0, Modulus, Words);
        RtlCopyMemory(Scratch, Acc, Words * sizeof(ULONG));
        for (i = 0; i < Words * 32; i++)
            BnDoubleMod(Scratch, 0, Modulus, Words);

        BnMontMul(Tmp, Val, Scratch, Modulus, MInv, Words);
        RtlCopyMemory(Val, Tmp, Words * sizeof(ULONG));
    }
    else
    {
        RtlZeroMemory(Acc, Words * sizeof(ULONG));
        Acc[0] = 1;
    }

    /* Squaring past the exponent's top set bit is wasted work, and a
     * public exponent leaves almost every word of its operand empty. */
    ExpBits = Words * 32;
    while (ExpBits > 0 &&
           ((Exponent[(ExpBits - 1) / 32] >> ((ExpBits - 1) % 32)) & 1) == 0)
        ExpBits--;

    /* Square and multiply, low exponent bit first. */
    for (i = 0; i < ExpBits; i++)
    {
        if ((Exponent[i / 32] >> (i % 32)) & 1)
        {
            if (Odd)
                BnMontMul(Tmp, Acc, Val, Modulus, MInv, Words);
            else
                BnMulMod(Tmp, Acc, Val, Modulus, Scratch, Words);
            RtlCopyMemory(Acc, Tmp, Words * sizeof(ULONG));
        }
        if (Odd)
            BnMontMul(Tmp, Val, Val, Modulus, MInv, Words);
        else
            BnMulMod(Tmp, Val, Val, Modulus, Scratch, Words);
        RtlCopyMemory(Val, Tmp, Words * sizeof(ULONG));
    }

    if (Odd)
    {
        /* Leave Montgomery form by multiplying by one. */
        RtlZeroMemory(Val, Words * sizeof(ULONG));
        Val[0] = 1;
        BnMontMul(Tmp, Acc, Val, Modulus, MInv, Words);
        RtlCopyMemory(Result, Tmp, Words * sizeof(ULONG));
    }
    else
    {
        RtlCopyMemory(Result, Acc, Words * sizeof(ULONG));
    }

    ExFreePool(Pool);
    return 1;
}

/* --- RSA public-key operations -------------------------------------------- *
 * The console's public-key blob is a small header followed by the modulus,
 * everything little-endian:
 *
 *   +0x00  "RSA1"
 *   +0x04  blob length
 *   +0x08  modulus bits
 *   +0x0c  index of the modulus' top byte (its length less one)
 *   +0x10  public exponent
 *   +0x14  modulus
 *
 * Layout and semantics per xbedump's xboxlib.c, confirmed against the
 * retail kernel: the length ordinal hands back the header's own length
 * field untouched -- it neither validates the magic nor derives anything.
 */

#define PK_BLOB_LENGTH(k)   (((const ULONG *)(k))[1])
ULONG NTAPI
XcPKGetKeyLen(_In_ PVOID PubKey)
{
    return PK_BLOB_LENGTH(PubKey);
}

/*
 * Cipher framework -- ABI-correct stubs.  Real implementations belong on
 * top of the same primitives once a title exercises them; this lets the
 * thunk-table call land on a balanced function and the title continue.
 */
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
