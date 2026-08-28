/*
 * XcSHA* / XcRC4* / XcHMAC -- the real hashing and stream-cipher exports.
 * These impls ship in xb/crypto.c (SHA-1 + RC4 cores from cryptlib,
 * HMAC-SHA1 built on top) but had no coverage against the retail kernel.
 * Golden vectors below were confirmed on the official kernel, so a green
 * run proves our cores match silicon bit-for-bit.
 */

#include "../harness.h"
#include <string.h>

/* SHA context is a documented 116-byte buffer; over-allocate for safety.
 * The RC4 key struct is undocumented (~258 bytes); 512 covers it. */
#define SHA_CTX_BYTES  128
#define RC4_KEY_BYTES  512

static bool digest_eq(const unsigned char *got, const unsigned char *want,
                      unsigned int n, const char *tag)
{
    for (unsigned int i = 0; i < n; i++) {
        if (got[i] != want[i]) {
            test_record_failure(__FILE__, __LINE__,
                "%s: byte %u got 0x%02x expected 0x%02x",
                tag, i, got[i], want[i]);
            return false;
        }
    }
    return true;
}

static void sha1(const void *data, ULONG len, unsigned char out[20])
{
    unsigned char ctx[SHA_CTX_BYTES];
    XcSHAInit(ctx);
    XcSHAUpdate(ctx, (PUCHAR)data, len);
    XcSHAFinal(ctx, out);
}

static bool t_sha1_abc(void)
{
    static const unsigned char want[20] = {
        0xa9,0x99,0x3e,0x36,0x47,0x06,0x81,0x6a,0xba,0x3e,
        0x25,0x71,0x78,0x50,0xc2,0x6c,0x9c,0xd0,0xd8,0x9d};
    unsigned char d[20];
    sha1("abc", 3, d);
    return digest_eq(d, want, 20, "sha1(abc)");
}

static bool t_sha1_empty(void)
{
    static const unsigned char want[20] = {
        0xda,0x39,0xa3,0xee,0x5e,0x6b,0x4b,0x0d,0x32,0x55,
        0xbf,0xef,0x95,0x60,0x18,0x90,0xaf,0xd8,0x07,0x09};
    unsigned char d[20];
    sha1("", 0, d);
    return digest_eq(d, want, 20, "sha1()");
}

/* Multi-Update must accumulate exactly like a single-shot hash. */
static bool t_sha1_incremental(void)
{
    static const unsigned char want[20] = {
        0x2f,0xd4,0xe1,0xc6,0x7a,0x2d,0x28,0xfc,0xed,0x84,
        0x9e,0xe1,0xbb,0x76,0xe7,0x39,0x1b,0x93,0xeb,0x12};
    unsigned char ctx[SHA_CTX_BYTES], d[20];
    XcSHAInit(ctx);
    XcSHAUpdate(ctx, (PUCHAR)"The quick brown fox ", 20);
    XcSHAUpdate(ctx, (PUCHAR)"jumps over the lazy dog", 23);
    XcSHAFinal(ctx, d);
    return digest_eq(d, want, 20, "sha1(quick-fox,split)");
}

static bool t_rc4_key_plaintext(void)
{
    static const unsigned char want[9] = {
        0xbb,0xf3,0x16,0xe8,0xd9,0x40,0xaf,0x0a,0xd3};
    unsigned char key_struct[RC4_KEY_BYTES];
    unsigned char buf[9];
    memcpy(buf, "Plaintext", 9);
    XcRC4Key(key_struct, 3, (PUCHAR)"Key");
    XcRC4Crypt(key_struct, 9, buf);
    return digest_eq(buf, want, 9, "rc4(Key,Plaintext)");
}

/* RC4 is its own inverse: re-keying and re-crypting recovers the input. */
static bool t_rc4_roundtrip(void)
{
    unsigned char key_struct[RC4_KEY_BYTES];
    unsigned char buf[14];
    static const unsigned char plain[14] = "Attack at dawn";
    static const unsigned char cipher[14] = {
        0x45,0xa0,0x1f,0x64,0x5f,0xc3,0x5b,0x38,0x35,0x52,0x54,0x4b,0x9b,0xf5};

    memcpy(buf, plain, 14);
    XcRC4Key(key_struct, 6, (PUCHAR)"Secret");
    XcRC4Crypt(key_struct, 14, buf);
    if (!digest_eq(buf, cipher, 14, "rc4(Secret) enc")) return false;

    XcRC4Key(key_struct, 6, (PUCHAR)"Secret");
    XcRC4Crypt(key_struct, 14, buf);
    return digest_eq(buf, plain, 14, "rc4(Secret) dec");
}

/* RFC 2202 HMAC-SHA1 test 2 (key "Jefe", <=64 bytes so Xbox's truncate
 * path == standard HMAC).  All data in the first segment, second empty. */
static bool t_hmac_jefe(void)
{
    static const unsigned char want[20] = {
        0xef,0xfc,0xdf,0x6a,0xe5,0xeb,0x2f,0xa2,0xd2,0x74,
        0x16,0xd5,0xf1,0x84,0xdf,0x9c,0x25,0x9a,0x7c,0x79};
    unsigned char out[20];
    XcHMAC((PUCHAR)"Jefe", 4,
           (PUCHAR)"what do ya want for nothing?", 28,
           NULL, 0, out);
    return digest_eq(out, want, 20, "hmac(Jefe)");
}

/* The two input segments hash as their concatenation. */
static bool t_hmac_two_segments(void)
{
    static const unsigned char want[20] = {
        0xe0,0xcd,0x99,0x77,0xea,0x2a,0x96,0x6d,0x8c,0xb7,
        0x8d,0xb1,0x97,0x94,0x7c,0x50,0xda,0x8c,0x1c,0x25};
    unsigned char out[20];
    XcHMAC((PUCHAR)"key", 3,
           (PUCHAR)"The quick brown fox", 19,
           (PUCHAR)" jumps over", 11, out);
    return digest_eq(out, want, 20, "hmac(key,2seg)");
}


/*
 * The caller owns the context, so its layout is part of the interface.
 * The console reserves 24 bytes it never touches, then the five state
 * words, the byte count and the 64-byte block buffer: 116 in all.
 */
#define CTX_GUARD    0xCC
#define CTX_STATE    24
#define CTX_COUNT    44
#define CTX_BUFFER   52
#define CTX_BYTES    116

static unsigned char g_ctx[512];

static unsigned long ctx_word(unsigned offset)
{
    return (unsigned long)g_ctx[offset] |
           ((unsigned long)g_ctx[offset + 1] << 8) |
           ((unsigned long)g_ctx[offset + 2] << 16) |
           ((unsigned long)g_ctx[offset + 3] << 24);
}

static bool ctx_clean_outside(unsigned from, unsigned to)
{
    unsigned i;
    for (i = 0; i < sizeof(g_ctx); i++) {
        if (i >= from && i < to) continue;
        if (g_ctx[i] != CTX_GUARD)
            FAIL_AND_RETURN("context byte +%u was written (0x%02x)",
                            i, g_ctx[i]);
    }
    return true;
}

/* Init writes the five state words and the count, and nothing else. */
static bool t_sha_context_layout(void)
{
    memset(g_ctx, CTX_GUARD, sizeof(g_ctx));
    XcSHAInit(g_ctx);

    ASSERT_EQ_U32(ctx_word(CTX_STATE +  0), 0x67452301);
    ASSERT_EQ_U32(ctx_word(CTX_STATE +  4), 0xEFCDAB89);
    ASSERT_EQ_U32(ctx_word(CTX_STATE +  8), 0x98BADCFE);
    ASSERT_EQ_U32(ctx_word(CTX_STATE + 12), 0x10325476);
    ASSERT_EQ_U32(ctx_word(CTX_STATE + 16), 0xC3D2E1F0);
    ASSERT_EQ_U32(ctx_word(CTX_COUNT), 0);
    ASSERT_EQ_U32(ctx_word(CTX_COUNT + 4), 0);
    return ctx_clean_outside(CTX_STATE, CTX_BUFFER);
}

/* The count is a byte count, and short data waits in the block buffer. */
static bool t_sha_context_counts_bytes(void)
{
    unsigned char data[70];

    memset(data, 'A', sizeof(data));
    memset(g_ctx, CTX_GUARD, sizeof(g_ctx));
    XcSHAInit(g_ctx);

    XcSHAUpdate(g_ctx, (PUCHAR)"abc", 3);
    ASSERT_EQ_U32(ctx_word(CTX_COUNT + 4), 3);
    ASSERT_EQ_U32(ctx_word(CTX_COUNT), 0);
    ASSERT_EQ_U32(g_ctx[CTX_BUFFER + 0], 'a');
    ASSERT_EQ_U32(g_ctx[CTX_BUFFER + 1], 'b');
    ASSERT_EQ_U32(g_ctx[CTX_BUFFER + 2], 'c');
    if (!ctx_clean_outside(CTX_STATE, CTX_BUFFER + 3))
        return false;

    /* Past a block the state has moved on and the remainder is buffered. */
    XcSHAUpdate(g_ctx, data, sizeof(data));
    ASSERT_EQ_U32(ctx_word(CTX_COUNT + 4), 73);
    ASSERT_TRUE(ctx_word(CTX_STATE) != 0x67452301);
    ASSERT_EQ_U32(g_ctx[CTX_BUFFER], 'A');
    return ctx_clean_outside(CTX_STATE, CTX_BYTES);
}

/* The digest is twenty bytes, and the context is left ready to reuse. */
static bool t_sha_final_reinitialises(void)
{
    unsigned char digest[64];
    unsigned i;

    memset(g_ctx, CTX_GUARD, sizeof(g_ctx));
    XcSHAInit(g_ctx);
    XcSHAUpdate(g_ctx, (PUCHAR)"abc", 3);

    memset(digest, CTX_GUARD, sizeof(digest));
    XcSHAFinal(g_ctx, digest);
    for (i = 20; i < sizeof(digest); i++)
        ASSERT_EQ_U32(digest[i], CTX_GUARD);

    ASSERT_EQ_U32(ctx_word(CTX_STATE), 0x67452301);
    ASSERT_EQ_U32(ctx_word(CTX_COUNT + 4), 0);
    if (!ctx_clean_outside(CTX_STATE, CTX_BYTES))
        return false;

    /* And a second run through the same buffer hashes correctly. */
    {
        static const unsigned char want[20] = {
            0xa9,0x99,0x3e,0x36,0x47,0x06,0x81,0x6a,0xba,0x3e,
            0x25,0x71,0x78,0x50,0xc2,0x6c,0x9c,0xd0,0xd8,0x9d};
        XcSHAUpdate(g_ctx, (PUCHAR)"abc", 3);
        XcSHAFinal(g_ctx, digest);
        return digest_eq(digest, want, 20, "sha1(abc) reusing the context");
    }
}

static const test_entry_t xc_crypto_entries[] = {
    {"sha1_abc", t_sha1_abc},
    {"sha1_empty", t_sha1_empty},
    {"sha1_incremental", t_sha1_incremental},
    {"rc4_key_plaintext", t_rc4_key_plaintext},
    {"rc4_roundtrip", t_rc4_roundtrip},
    {"hmac_jefe", t_hmac_jefe},
    {"hmac_two_segments", t_hmac_two_segments},
    {"sha_context_layout", t_sha_context_layout},
    {"sha_context_counts_bytes", t_sha_context_counts_bytes},
    {"sha_final_reinitialises", t_sha_final_reinitialises},
};

DEFINE_GROUP(xc_crypto, "xc/crypto");
