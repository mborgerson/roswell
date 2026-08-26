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

static const test_entry_t xc_crypto_entries[] = {
    {"sha1_abc", t_sha1_abc},
    {"sha1_empty", t_sha1_empty},
    {"sha1_incremental", t_sha1_incremental},
    {"rc4_key_plaintext", t_rc4_key_plaintext},
    {"rc4_roundtrip", t_rc4_roundtrip},
    {"hmac_jefe", t_hmac_jefe},
    {"hmac_two_segments", t_hmac_two_segments},
};

DEFINE_GROUP(xc_crypto, "xc/crypto");
