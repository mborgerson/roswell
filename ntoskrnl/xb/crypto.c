/*
 * PROJECT:     nxkrnl -- a free kernel for the original Xbox
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Xc* crypto ordinals -- SHA-1, RC4, HMAC-SHA1, modular
 *              exponentiation and the DES cipher framework, published
 *              through the vector a title can replace.
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

static VOID NTAPI XcpSHAInit(_In_ PVOID Ctx)
{ A_SHAInit(XC_SHA_CTX(Ctx)); }
static VOID NTAPI XcpSHAUpdate(_In_ PVOID Ctx, _In_ PVOID Data, _In_ ULONG Len)
{ A_SHAUpdate(XC_SHA_CTX(Ctx), (const unsigned char *)Data, Len); }
static VOID NTAPI XcpSHAFinal(_In_ PVOID Ctx, _Out_writes_(20) PVOID Digest)
{ A_SHAFinal(XC_SHA_CTX(Ctx), (PULONG)Digest); }

/* RC4 key schedule.  RC4_CONTEXT (258 B) fits inside the key-struct slot. */
static VOID NTAPI
XcpRC4Key(_Out_ PVOID KeyStruct, _In_ ULONG KeyLen, _In_ PVOID KeyData)
{
    rc4_init((RC4_CONTEXT *)KeyStruct,
             (const unsigned char *)KeyData, KeyLen);
}

/* RC4 transform, in place. */
static VOID NTAPI
XcpRC4Crypt(_In_ PVOID KeyStruct, _In_ ULONG Len, _Inout_ PVOID Data)
{
    rc4_crypt((RC4_CONTEXT *)KeyStruct, (unsigned char *)Data, Len);
}

/* HMAC-SHA1 over two data segments:
 *   inner = SHA1(ipad || I1 || I2);  Out = SHA1(opad || inner). */
static VOID NTAPI
XcpHMAC(_In_ PVOID K, _In_ ULONG Kl, _In_ PVOID I1, _In_ ULONG L1,
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

static ULONG NTAPI
XcpModExp(_Out_ PULONG Result, _In_ PULONG Base, _In_ PULONG Exponent,
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
#define PK_MODULUS_TOP(k)   (((const ULONG *)(k))[3])
#define PK_EXPONENT(k)      ((const ULONG *)(k) + 4)
#define PK_MODULUS(k)       ((const ULONG *)(k) + 5)

static ULONG NTAPI
XcpPKGetKeyLen(_In_ PVOID PubKey)
{
    return PK_BLOB_LENGTH(PubKey);
}

/*
 * The DigestInfo that prefixes a SHA-1 hash inside a PKCS#1 v1.5 block,
 * stored the way the decrypted block reads out: least significant byte
 * first, so these run backwards compared to the ASN.1 encoding.  Both the
 * form carrying explicit NULL algorithm parameters and the form omitting
 * them are accepted.
 */
static const struct { UCHAR Length; UCHAR Bytes[15]; } XcPkcs1Prefix[] = {
    { 15, { 0x14, 0x04, 0x00, 0x05, 0x1a, 0x02, 0x03, 0x0e,
            0x2b, 0x05, 0x06, 0x09, 0x30, 0x21, 0x30 } },
    { 13, { 0x14, 0x04, 0x1a, 0x02, 0x03, 0x0e, 0x2b, 0x05,
            0x06, 0x07, 0x30, 0x1f, 0x30 } },
};

static BOOLEAN NTAPI
XcpVerifyPKCS1Signature(_In_ PVOID Signature, _In_ PVOID PubKey,
                       _In_ PVOID Digest)
{
    const UCHAR *Hash = (const UCHAR *)Digest;
    ULONG Top = PK_MODULUS_TOP(PubKey);
    ULONG Bytes = Top + 1;
    ULONG Words = Bytes / 4;
    PULONG Pool, Sig, Exp, Mod, Block;
    const UCHAR *Plain;
    ULONG Zero, i;
    BOOLEAN Ok = FALSE;

    /* The block has to hold the hash, a prefix and at least some padding. */
    if ((Bytes & 3) != 0 || Words == 0 || Words > BN_MAX_WORDS || Bytes < 64)
        return FALSE;

    Pool = ExAllocatePoolWithTag(NonPagedPool, 4 * Bytes, 'sXcX');
    if (Pool == NULL)
        return FALSE;
    Sig = Pool;
    Exp = Sig + Words;
    Mod = Exp + Words;
    Block = Mod + Words;

    RtlCopyMemory(Sig, Signature, Bytes);
    RtlZeroMemory(Exp, Bytes);
    Exp[0] = *PK_EXPONENT(PubKey);
    RtlCopyMemory(Mod, PK_MODULUS(PubKey), Bytes);

    if (XcpModExp(Block, Sig, Exp, Mod, Words) == 0)
        goto Done;

    /* The block reads out least significant byte first, so the hash sits
     * at the bottom, reversed, and the padding runs up to the top. */
    Plain = (const UCHAR *)Block;
    for (i = 0; i < 20; i++)
        if (Plain[i] != Hash[19 - i])
            goto Done;

    Zero = 0;
    for (i = 0; i < RTL_NUMBER_OF(XcPkcs1Prefix); i++)
    {
        if (RtlCompareMemory(Plain + 20, XcPkcs1Prefix[i].Bytes,
                             XcPkcs1Prefix[i].Length) ==
            XcPkcs1Prefix[i].Length)
        {
            Zero = 20 + XcPkcs1Prefix[i].Length;
            break;
        }
    }
    if (Zero == 0)
        goto Done;

    /* A separating zero below the padding, and 00 01 capping the top. */
    if (Plain[Zero] != 0x00 || Plain[Top] != 0x00 || Plain[Top - 1] != 0x01)
        goto Done;
    for (i = Zero + 1; i < Top - 1; i++)
        if (Plain[i] != 0xff)
            goto Done;

    Ok = TRUE;

Done:
    ExFreePool(Pool);
    return Ok;
}

/*
 * Cipher framework -- DES and triple-DES, the two the console offers.
 *
 * The key table is opaque to the caller but its layout is not free: a
 * title allocates it, so the sizes are fixed at 128 bytes for DES and
 * 384 (one schedule per key) for triple-DES, and the bytes themselves
 * are laid out the way the console lays them out.  Confirmed against
 * the retail kernel, which builds the schedules the same way for every
 * cipher number it is given: zero selects DES, anything else triple.
 */

#define DES_TABLE_BYTES     128
#define DES3_TABLE_BYTES    (3 * DES_TABLE_BYTES)
#define DES_BLOCK_BYTES     8

/* FIPS 46-3, one-based bit numbers, most significant bit first. */
static const UCHAR DesIp[64] = {
    58,50,42,34,26,18,10, 2, 60,52,44,36,28,20,12, 4,
    62,54,46,38,30,22,14, 6, 64,56,48,40,32,24,16, 8,
    57,49,41,33,25,17, 9, 1, 59,51,43,35,27,19,11, 3,
    61,53,45,37,29,21,13, 5, 63,55,47,39,31,23,15, 7,
};

static const UCHAR DesFp[64] = {
    40, 8,48,16,56,24,64,32, 39, 7,47,15,55,23,63,31,
    38, 6,46,14,54,22,62,30, 37, 5,45,13,53,21,61,29,
    36, 4,44,12,52,20,60,28, 35, 3,43,11,51,19,59,27,
    34, 2,42,10,50,18,58,26, 33, 1,41, 9,49,17,57,25,
};

static const UCHAR DesE[48] = {
    32, 1, 2, 3, 4, 5,  4, 5, 6, 7, 8, 9,
     8, 9,10,11,12,13, 12,13,14,15,16,17,
    16,17,18,19,20,21, 20,21,22,23,24,25,
    24,25,26,27,28,29, 28,29,30,31,32, 1,
};

static const UCHAR DesP[32] = {
    16, 7,20,21,29,12,28,17,  1,15,23,26, 5,18,31,10,
     2, 8,24,14,32,27, 3, 9, 19,13,30, 6,22,11, 4,25,
};

static const UCHAR DesPc1[56] = {
    57,49,41,33,25,17, 9,  1,58,50,42,34,26,18,
    10, 2,59,51,43,35,27, 19,11, 3,60,52,44,36,
    63,55,47,39,31,23,15,  7,62,54,46,38,30,22,
    14, 6,61,53,45,37,29, 21,13, 5,28,20,12, 4,
};

static const UCHAR DesPc2[48] = {
    14,17,11,24, 1, 5,  3,28,15, 6,21,10,
    23,19,12, 4,26, 8, 16, 7,27,20,13, 2,
    41,52,31,37,47,55, 30,40,51,45,33,48,
    44,49,39,56,34,53, 46,42,50,36,29,32,
};

static const UCHAR DesShifts[16] = {
    1,1,2,2,2,2,2,2,1,2,2,2,2,2,2,1
};

static const UCHAR DesSBox[8][64] = {
    {14, 4,13, 1, 2,15,11, 8, 3,10, 6,12, 5, 9, 0, 7,
      0,15, 7, 4,14, 2,13, 1,10, 6,12,11, 9, 5, 3, 8,
      4, 1,14, 8,13, 6, 2,11,15,12, 9, 7, 3,10, 5, 0,
     15,12, 8, 2, 4, 9, 1, 7, 5,11, 3,14,10, 0, 6,13},
    {15, 1, 8,14, 6,11, 3, 4, 9, 7, 2,13,12, 0, 5,10,
      3,13, 4, 7,15, 2, 8,14,12, 0, 1,10, 6, 9,11, 5,
      0,14, 7,11,10, 4,13, 1, 5, 8,12, 6, 9, 3, 2,15,
     13, 8,10, 1, 3,15, 4, 2,11, 6, 7,12, 0, 5,14, 9},
    {10, 0, 9,14, 6, 3,15, 5, 1,13,12, 7,11, 4, 2, 8,
     13, 7, 0, 9, 3, 4, 6,10, 2, 8, 5,14,12,11,15, 1,
     13, 6, 4, 9, 8,15, 3, 0,11, 1, 2,12, 5,10,14, 7,
      1,10,13, 0, 6, 9, 8, 7, 4,15,14, 3,11, 5, 2,12},
    { 7,13,14, 3, 0, 6, 9,10, 1, 2, 8, 5,11,12, 4,15,
     13, 8,11, 5, 6,15, 0, 3, 4, 7, 2,12, 1,10,14, 9,
     10, 6, 9, 0,12,11, 7,13,15, 1, 3,14, 5, 2, 8, 4,
      3,15, 0, 6,10, 1,13, 8, 9, 4, 5,11,12, 7, 2,14},
    { 2,12, 4, 1, 7,10,11, 6, 8, 5, 3,15,13, 0,14, 9,
     14,11, 2,12, 4, 7,13, 1, 5, 0,15,10, 3, 9, 8, 6,
      4, 2, 1,11,10,13, 7, 8,15, 9,12, 5, 6, 3, 0,14,
     11, 8,12, 7, 1,14, 2,13, 6,15, 0, 9,10, 4, 5, 3},
    {12, 1,10,15, 9, 2, 6, 8, 0,13, 3, 4,14, 7, 5,11,
     10,15, 4, 2, 7,12, 9, 5, 6, 1,13,14, 0,11, 3, 8,
      9,14,15, 5, 2, 8,12, 3, 7, 0, 4,10, 1,13,11, 6,
      4, 3, 2,12, 9, 5,15,10,11,14, 1, 7, 6, 0, 8,13},
    { 4,11, 2,14,15, 0, 8,13, 3,12, 9, 7, 5,10, 6, 1,
     13, 0,11, 7, 4, 9, 1,10,14, 3, 5,12, 2,15, 8, 6,
      1, 4,11,13,12, 3, 7,14,10,15, 6, 8, 0, 5, 9, 2,
      6,11,13, 8, 1, 4,10, 7, 9, 5, 0,15,14, 2, 3,12},
    {13, 2, 8, 4, 6,15,11, 1,10, 9, 3,14, 5, 0,12, 7,
      1,15,13, 8,10, 3, 7, 4,12, 5, 6,11, 0,14, 9, 2,
      7,11, 4, 1, 9,12,14, 2, 0, 6,10,13,15, 3, 5, 8,
      2, 1,14, 7, 4,10, 8,13,15,12, 9, 0, 3, 5, 6,11},
};

/* The sixteen round keys, six bits per S-box, ready to XOR. */
typedef struct _DES_KEY
{
    UCHAR Round[16][8];
} DES_KEY;

static ULONG
DesBit(const UCHAR *Bytes, ULONG Position)
{
    return (Bytes[(Position - 1) >> 3] >> (7 - ((Position - 1) & 7))) & 1;
}

static VOID
DesPermute(PUCHAR Out, const UCHAR *In, const UCHAR *Table, ULONG Bits)
{
    ULONG i;

    RtlZeroMemory(Out, (Bits + 7) / 8);
    for (i = 0; i < Bits; i++)
        Out[i >> 3] |= (UCHAR)(DesBit(In, Table[i]) << (7 - (i & 7)));
}

/*
 * A round key occupies two little-endian words: the first carries the
 * inputs to S1, S3, S5 and S7, the second those to S2, S4, S6 and S8,
 * six bits apiece at an eight-bit stride, least significant bit first,
 * starting two bits into the first word and six into the second -- where
 * the eighth wraps around the top.
 */
#define DES_KEY_SHIFT(k)    ((((k) >> 1) * 8 + (((k) & 1) ? 6 : 2)) & 31)

static VOID
DesStoreRound(PUCHAR Table, const UCHAR *Subkey)
{
    ULONG Word[2] = {0, 0};
    ULONG k, b, Shift, Six;

    for (k = 0; k < 8; k++)
    {
        Six = 0;
        for (b = 0; b < 6; b++)
            Six |= (ULONG)Subkey[k * 6 + b] << b;
        Shift = DES_KEY_SHIFT(k);
        Word[k & 1] |= (Six << Shift) | (Shift > 26 ? (Six >> (32 - Shift)) : 0);
    }

    for (k = 0; k < 2; k++)
    {
        Table[k * 4 + 0] = (UCHAR)Word[k];
        Table[k * 4 + 1] = (UCHAR)(Word[k] >> 8);
        Table[k * 4 + 2] = (UCHAR)(Word[k] >> 16);
        Table[k * 4 + 3] = (UCHAR)(Word[k] >> 24);
    }
}

static VOID
DesLoadKey(DES_KEY *Key, const UCHAR *Table)
{
    ULONG r, k, b, Shift, Six;
    ULONG Word[2];

    for (r = 0; r < 16; r++)
    {
        for (k = 0; k < 2; k++)
            Word[k] = (ULONG)Table[r * 8 + k * 4] |
                      ((ULONG)Table[r * 8 + k * 4 + 1] << 8) |
                      ((ULONG)Table[r * 8 + k * 4 + 2] << 16) |
                      ((ULONG)Table[r * 8 + k * 4 + 3] << 24);

        for (k = 0; k < 8; k++)
        {
            UCHAR Value = 0;

            Shift = DES_KEY_SHIFT(k);
            Six = (Word[k & 1] >> Shift) |
                  (Shift > 26 ? (Word[k & 1] << (32 - Shift)) : 0);
            for (b = 0; b < 6; b++)
                Value |= (UCHAR)(((Six >> b) & 1) << (5 - b));
            Key->Round[r][k] = Value;
        }
    }
}

/* Key -> the console's key table: PC-1, the round rotations, PC-2. */
static VOID
DesSchedule(PUCHAR Table, const UCHAR *KeyBytes)
{
    ULONG Left = 0, Right = 0;
    UCHAR Subkey[48];
    ULONG i, j;

    for (i = 0; i < 28; i++)
    {
        Left |= DesBit(KeyBytes, DesPc1[i]) << (27 - i);
        Right |= DesBit(KeyBytes, DesPc1[i + 28]) << (27 - i);
    }

    for (i = 0; i < 16; i++)
    {
        Left = ((Left << DesShifts[i]) |
                (Left >> (28 - DesShifts[i]))) & 0x0FFFFFFF;
        Right = ((Right << DesShifts[i]) |
                 (Right >> (28 - DesShifts[i]))) & 0x0FFFFFFF;

        for (j = 0; j < 48; j++)
        {
            ULONG Bit = DesPc2[j];

            Subkey[j] = (UCHAR)(Bit <= 28 ? (Left >> (28 - Bit)) & 1
                                          : (Right >> (56 - Bit)) & 1);
        }
        DesStoreRound(Table + i * 8, Subkey);
    }
}

static VOID
DesBlock(PUCHAR Out, const UCHAR *In, const DES_KEY *Key, BOOLEAN Decrypt)
{
    UCHAR State[8], Left[4], Right[4], Feistel[4], SOut[4];
    ULONG Round, k, i;

    DesPermute(State, In, DesIp, 64);
    RtlCopyMemory(Left, State, 4);
    RtlCopyMemory(Right, State + 4, 4);

    for (Round = 0; Round < 16; Round++)
    {
        const UCHAR *Subkey = Key->Round[Decrypt ? 15 - Round : Round];

        RtlZeroMemory(SOut, sizeof(SOut));
        for (k = 0; k < 8; k++)
        {
            UCHAR Six = 0, Value;
            ULONG Row, Column;

            for (i = 0; i < 6; i++)
                Six |= (UCHAR)(DesBit(Right, DesE[k * 6 + i]) << (5 - i));
            Six ^= Subkey[k];

            Row = ((Six >> 4) & 2) | (Six & 1);
            Column = (Six >> 1) & 0x0F;
            Value = DesSBox[k][Row * 16 + Column];
            SOut[k >> 1] |= (UCHAR)((k & 1) ? Value : (Value << 4));
        }

        DesPermute(Feistel, SOut, DesP, 32);
        for (i = 0; i < 4; i++)
            Feistel[i] ^= Left[i];
        RtlCopyMemory(Left, Right, 4);
        RtlCopyMemory(Right, Feistel, 4);
    }

    RtlCopyMemory(State, Right, 4);
    RtlCopyMemory(State + 4, Left, 4);
    DesPermute(Out, State, DesFp, 64);
}

/*
 * One block through the cipher the table was built for.  Triple-DES
 * runs the three schedules encrypt-decrypt-encrypt, and in the other
 * order the other way round; every operation code but zero encrypts.
 */
static VOID
DesCryptBlock(PUCHAR Out, const UCHAR *In, const DES_KEY *Keys,
              ULONG KeyCount, BOOLEAN Decrypt)
{
    if (KeyCount == 1)
    {
        DesBlock(Out, In, &Keys[0], Decrypt);
        return;
    }

    if (!Decrypt)
    {
        DesBlock(Out, In, &Keys[0], FALSE);
        DesBlock(Out, Out, &Keys[1], TRUE);
        DesBlock(Out, Out, &Keys[2], FALSE);
    }
    else
    {
        DesBlock(Out, In, &Keys[2], TRUE);
        DesBlock(Out, Out, &Keys[1], FALSE);
        DesBlock(Out, Out, &Keys[0], TRUE);
    }
}

static ULONG
DesKeyCount(ULONG Cipher)
{
    return Cipher == 0 ? 1 : 3;
}

static VOID
DesLoadTable(DES_KEY *Keys, ULONG KeyCount, const UCHAR *Table)
{
    ULONG i;

    for (i = 0; i < KeyCount; i++)
        DesLoadKey(&Keys[i], Table + i * DES_TABLE_BYTES);
}

/*
 * Force odd parity on every key byte: the low bit is the parity bit, set
 * so each byte carries an odd number of 1s.  The upper seven bits are the
 * key material and are left untouched.
 */
static VOID NTAPI
XcpDESKeyParity(_Inout_ PVOID Key, _In_ ULONG KeyLen)
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

static VOID NTAPI
XcpKeyTable(_In_ ULONG Cipher, _Out_ PVOID KeyTable, _In_ PVOID Key)
{
    ULONG Keys = DesKeyCount(Cipher);
    ULONG i;

    for (i = 0; i < Keys; i++)
        DesSchedule((PUCHAR)KeyTable + i * DES_TABLE_BYTES,
                    (const UCHAR *)Key + i * 8);
}

/*
 * The service entry the console answers with zero, whatever operation
 * number it is handed: probed across the first sixteen, none of which
 * touches the argument block.
 */
static ULONG NTAPI
XcpCryptService(_In_ ULONG Op, _In_ PVOID Args)
{
    UNREFERENCED_PARAMETER(Op);
    UNREFERENCED_PARAMETER(Args);
    return 0;
}

/* One block, no chaining: the operation code decrypts only when zero. */
static VOID NTAPI
XcpBlockCrypt(_In_ ULONG Cipher, _Out_ PVOID Out, _In_ PVOID In,
             _In_ PVOID KeyTable, _In_ ULONG Op)
{
    DES_KEY Keys[3];
    ULONG Count = DesKeyCount(Cipher);

    DesLoadTable(Keys, Count, (const UCHAR *)KeyTable);
    DesCryptBlock((PUCHAR)Out, (const UCHAR *)In, Keys, Count,
                  (BOOLEAN)(Op == 0));
}

static VOID NTAPI
XcpBlockCryptCBC(_In_ ULONG Cipher, _In_ ULONG Len, _Out_ PVOID Out,
                _In_ PVOID In, _In_ PVOID KeyTable, _In_ ULONG Op,
                _Inout_ PVOID Feedback)
{
    DES_KEY Keys[3];
    ULONG Count = DesKeyCount(Cipher);
    PUCHAR Chain = (PUCHAR)Feedback;
    const UCHAR *Source = (const UCHAR *)In;
    PUCHAR Target = (PUCHAR)Out;
    UCHAR Block[DES_BLOCK_BYTES];
    ULONG i;

    if (Len < DES_BLOCK_BYTES)
        return;

    DesLoadTable(Keys, Count, (const UCHAR *)KeyTable);

    /* The chaining value is the caller's, and it leaves holding the last
     * ciphertext block whichever direction the run went. */
    for (; Len >= DES_BLOCK_BYTES; Len -= DES_BLOCK_BYTES)
    {
        if (Op != 0)
        {
            for (i = 0; i < DES_BLOCK_BYTES; i++)
                Block[i] = (UCHAR)(Source[i] ^ Chain[i]);
            DesCryptBlock(Target, Block, Keys, Count, FALSE);
            RtlCopyMemory(Chain, Target, DES_BLOCK_BYTES);
        }
        else
        {
            RtlCopyMemory(Block, Source, DES_BLOCK_BYTES);
            DesCryptBlock(Target, Source, Keys, Count, TRUE);
            for (i = 0; i < DES_BLOCK_BYTES; i++)
                Target[i] ^= Chain[i];
            RtlCopyMemory(Chain, Block, DES_BLOCK_BYTES);
        }
        Source += DES_BLOCK_BYTES;
        Target += DES_BLOCK_BYTES;
    }
}

/* --- the crypto vector ---------------------------------------------------- *
 * Every Xc entry the console publishes is a thunk through a vector a title
 * can replace wholesale, which is what the update ordinal exists for.
 * Probed on the retail kernel: the address of an export is not the routine
 * the ROM vector names, installing a vector diverts the export -- including
 * its return value -- and the out-parameter always hands back the ROM's own
 * table, never whatever was installed last.
 */

typedef struct _XC_VECTOR
{
    VOID (NTAPI *SHAInit)(PVOID Ctx);
    VOID (NTAPI *SHAUpdate)(PVOID Ctx, PVOID Data, ULONG Len);
    VOID (NTAPI *SHAFinal)(PVOID Ctx, PVOID Digest);
    VOID (NTAPI *RC4Key)(PVOID KeyStruct, ULONG KeyLen, PVOID KeyData);
    VOID (NTAPI *RC4Crypt)(PVOID KeyStruct, ULONG Len, PVOID Data);
    VOID (NTAPI *HMAC)(PVOID K, ULONG Kl, PVOID I1, ULONG L1,
                       PVOID I2, ULONG L2, PVOID Digest);
    ULONG (NTAPI *PKEncPublic)(PVOID PubKey, PVOID In, PVOID Out);
    ULONG (NTAPI *PKDecPrivate)(PVOID PrivKey, PVOID In, PVOID Out);
    ULONG (NTAPI *PKGetKeyLen)(PVOID PubKey);
    BOOLEAN (NTAPI *VerifyPKCS1Signature)(PVOID Signature, PVOID PubKey,
                                          PVOID Digest);
    ULONG (NTAPI *ModExp)(PULONG Result, PULONG Base, PULONG Exponent,
                          PULONG Modulus, ULONG Words);
    VOID (NTAPI *DESKeyParity)(PVOID Key, ULONG KeyLen);
    VOID (NTAPI *KeyTable)(ULONG Cipher, PVOID KeyTable, PVOID Key);
    VOID (NTAPI *BlockCrypt)(ULONG Cipher, PVOID Out, PVOID In,
                             PVOID KeyTable, ULONG Op);
    VOID (NTAPI *BlockCryptCBC)(ULONG Cipher, ULONG Len, PVOID Out, PVOID In,
                                PVOID KeyTable, ULONG Op, PVOID Feedback);
    ULONG (NTAPI *CryptService)(ULONG Op, PVOID Args);
} XC_VECTOR, *PXC_VECTOR;

/*
 * Two slots have no routine behind them: the raw public and private key
 * operations are unmapped ordinals still, so the slot carries the export
 * scaffold's own stub.  Reaching them through the vector then bugchecks
 * naming the ordinal, exactly as calling the export does.
 */
ULONG __stdcall XbExpStub_341(ULONG, ULONG, ULONG);
ULONG __stdcall XbExpStub_342(ULONG, ULONG, ULONG);

#define XC_ROM_VECTOR                                       \
{                                                           \
    XcpSHAInit, XcpSHAUpdate, XcpSHAFinal,                  \
    XcpRC4Key, XcpRC4Crypt, XcpHMAC,                        \
    (ULONG (NTAPI *)(PVOID, PVOID, PVOID))XbExpStub_341,    \
    (ULONG (NTAPI *)(PVOID, PVOID, PVOID))XbExpStub_342,    \
    XcpPKGetKeyLen, XcpVerifyPKCS1Signature, XcpModExp,     \
    XcpDESKeyParity, XcpKeyTable, XcpBlockCrypt,            \
    XcpBlockCryptCBC, XcpCryptService,                      \
}

static const XC_VECTOR XcRomVector = XC_ROM_VECTOR;
static XC_VECTOR XcVector = XC_ROM_VECTOR;

VOID NTAPI
XcUpdateCrypto(_In_ PVOID NewVector, _Out_opt_ PVOID RomVector)
{
    PVOID *Live = (PVOID *)&XcVector;
    const PVOID *New = (const PVOID *)NewVector;
    ULONG i;

    if (RomVector != NULL)
        RtlCopyMemory(RomVector, &XcRomVector, sizeof(XcRomVector));

    /* An empty slot keeps whatever is installed, so a title replacing one
     * routine hands over a vector with the rest left NULL.  The whole
     * structure is function pointers, so walk it as such. */
    for (i = 0; i < sizeof(XcVector) / sizeof(PVOID); i++)
    {
        if (New[i] != NULL)
            Live[i] = New[i];
    }
}

VOID NTAPI XcSHAInit(_In_ PVOID Ctx)
{ XcVector.SHAInit(Ctx); }

VOID NTAPI XcSHAUpdate(_In_ PVOID Ctx, _In_ PVOID Data, _In_ ULONG Len)
{ XcVector.SHAUpdate(Ctx, Data, Len); }

VOID NTAPI XcSHAFinal(_In_ PVOID Ctx, _Out_writes_(20) PVOID Digest)
{ XcVector.SHAFinal(Ctx, Digest); }

VOID NTAPI XcRC4Key(_Out_ PVOID KeyStruct, _In_ ULONG KeyLen, _In_ PVOID KeyData)
{ XcVector.RC4Key(KeyStruct, KeyLen, KeyData); }

VOID NTAPI XcRC4Crypt(_In_ PVOID KeyStruct, _In_ ULONG Len, _Inout_ PVOID Data)
{ XcVector.RC4Crypt(KeyStruct, Len, Data); }

VOID NTAPI XcHMAC(_In_ PVOID K, _In_ ULONG Kl, _In_ PVOID I1, _In_ ULONG L1,
                  _In_ PVOID I2, _In_ ULONG L2, _Out_writes_(20) PVOID Out)
{ XcVector.HMAC(K, Kl, I1, L1, I2, L2, Out); }

ULONG NTAPI XcPKGetKeyLen(_In_ PVOID PubKey)
{ return XcVector.PKGetKeyLen(PubKey); }

BOOLEAN NTAPI XcVerifyPKCS1Signature(_In_ PVOID Signature, _In_ PVOID PubKey,
                                     _In_ PVOID Digest)
{ return XcVector.VerifyPKCS1Signature(Signature, PubKey, Digest); }

ULONG NTAPI XcModExp(_Out_ PULONG Result, _In_ PULONG Base,
                     _In_ PULONG Exponent, _In_ PULONG Modulus,
                     _In_ ULONG Words)
{ return XcVector.ModExp(Result, Base, Exponent, Modulus, Words); }

VOID NTAPI XcDESKeyParity(_Inout_ PVOID Key, _In_ ULONG KeyLen)
{ XcVector.DESKeyParity(Key, KeyLen); }

VOID NTAPI XcKeyTable(_In_ ULONG Cipher, _Out_ PVOID KeyTable, _In_ PVOID Key)
{ XcVector.KeyTable(Cipher, KeyTable, Key); }

VOID NTAPI XcBlockCrypt(_In_ ULONG Cipher, _Out_ PVOID Out, _In_ PVOID In,
                        _In_ PVOID KeyTable, _In_ ULONG Op)
{ XcVector.BlockCrypt(Cipher, Out, In, KeyTable, Op); }

VOID NTAPI XcBlockCryptCBC(_In_ ULONG Cipher, _In_ ULONG Len, _Out_ PVOID Out,
                           _In_ PVOID In, _In_ PVOID KeyTable, _In_ ULONG Op,
                           _Inout_ PVOID Feedback)
{ XcVector.BlockCryptCBC(Cipher, Len, Out, In, KeyTable, Op, Feedback); }

ULONG NTAPI XcCryptService(_In_ ULONG Op, _In_ PVOID Args)
{ return XcVector.CryptService(Op, Args); }

/* EOF */
