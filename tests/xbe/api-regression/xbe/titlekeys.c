/*
 * The keys a title signs and links with: XboxLANKey, XboxSignatureKey
 * and the sixteen XboxAlternateSignatureKeys.
 *
 * Each is derived from a field of the running image's own certificate,
 * under a key the kernel holds and no title can see.  The value is
 * therefore not checkable from here -- the two kernels hold different
 * keys and produce different answers, both correct.
 *
 * What is checkable is that each key is a function of its certificate
 * field: two fields that are equal give equal keys, two that differ
 * give different ones.  That holds on whichever kernel is running, and
 * it is what tells a derived key apart from a scaffold.
 */

#include "../harness.h"
#include <string.h>

#define KEY_LENGTH  16
#define ALT_KEYS    16

/* The image is at its own base and names its certificate at +0x118. */
static const UCHAR *certificate(void)
{
    const UCHAR *xbe = (const UCHAR *)0x00010000;
    ULONG address;

    memcpy(&address, xbe + 0x118, sizeof(address));
    return (const UCHAR *)address;
}

#define CERT_LAN_KEY        0xB0
#define CERT_SIGNATURE_KEY  0xC0
#define CERT_ALTERNATE      0xD0
#define CERT_SIZE_WITH_ALTERNATES (CERT_ALTERNATE + ALT_KEYS * KEY_LENGTH)

static bool has_alternates(void)
{
    ULONG size;
    memcpy(&size, certificate(), sizeof(size));
    return size >= CERT_SIZE_WITH_ALTERNATES;
}

/* One key, and the certificate field it comes from. */
typedef struct { const char *name; const UCHAR *field; const UCHAR *key; } pair_t;

static bool all_pairs_follow_their_fields(const pair_t *p, ULONG n)
{
    ULONG i, j;

    for (i = 0; i < n; i++) {
        UCHAR zero[KEY_LENGTH];
        memset(zero, 0, sizeof(zero));
        /* A derived key is a digest; a scaffold is four zeroed bytes
         * followed by whatever the kernel's linker put next. */
        if (memcmp(p[i].key, zero, KEY_LENGTH) == 0)
            FAIL_AND_RETURN("%s is all zeros", p[i].name);
    }

    for (i = 0; i < n; i++) {
        for (j = i + 1; j < n; j++) {
            bool same_field = memcmp(p[i].field, p[j].field, KEY_LENGTH) == 0;
            bool same_key = memcmp(p[i].key, p[j].key, KEY_LENGTH) == 0;
            if (same_field && !same_key)
                FAIL_AND_RETURN("%s and %s come from the same certificate "
                                "field and differ", p[i].name, p[j].name);
            if (!same_field && same_key)
                FAIL_AND_RETURN("%s and %s come from different certificate "
                                "fields and match", p[i].name, p[j].name);
        }
    }
    return true;
}

/* The LAN key and the signature key against each other's fields. */
static bool t_the_lan_and_signature_keys_follow_the_certificate(void)
{
    const UCHAR *cert = certificate();
    pair_t pairs[2];

    pairs[0].name = "XboxLANKey";
    pairs[0].field = cert + CERT_LAN_KEY;
    pairs[0].key = XboxLANKey;
    pairs[1].name = "XboxSignatureKey";
    pairs[1].field = cert + CERT_SIGNATURE_KEY;
    pairs[1].key = XboxSignatureKey;

    return all_pairs_follow_their_fields(pairs, 2);
}

/* The sixteen alternates against the signature key and each other. */
static bool t_the_alternate_keys_follow_the_certificate(void)
{
    static char names[ALT_KEYS][16];
    const UCHAR *cert = certificate();
    pair_t pairs[ALT_KEYS + 1];
    ULONG i;

    if (!has_alternates())
        FAIL_AND_RETURN("this image's certificate has no alternate keys");

    pairs[0].name = "XboxSignatureKey";
    pairs[0].field = cert + CERT_SIGNATURE_KEY;
    pairs[0].key = XboxSignatureKey;
    for (i = 0; i < ALT_KEYS; i++) {
        names[i][0] = 'a'; names[i][1] = 'l'; names[i][2] = 't';
        names[i][3] = '0' + (char)(i / 10);
        names[i][4] = '0' + (char)(i % 10);
        names[i][5] = 0;
        pairs[i + 1].name = names[i];
        pairs[i + 1].field = cert + CERT_ALTERNATE + i * KEY_LENGTH;
        pairs[i + 1].key = XboxAlternateSignatureKeys[i];
    }

    return all_pairs_follow_their_fields(pairs, ALT_KEYS + 1);
}

/* The keys are data, not a computation: reading twice gives the same. */
static bool t_the_keys_do_not_move(void)
{
    UCHAR lan[KEY_LENGTH], sig[KEY_LENGTH];

    memcpy(lan, XboxLANKey, sizeof(lan));
    memcpy(sig, XboxSignatureKey, sizeof(sig));
    ASSERT_TRUE(memcmp(lan, XboxLANKey, sizeof(lan)) == 0);
    ASSERT_TRUE(memcmp(sig, XboxSignatureKey, sizeof(sig)) == 0);
    return true;
}

static const test_entry_t xbe_titlekeys_entries[] = {
    { "the_lan_and_signature_keys_follow_the_certificate",
      t_the_lan_and_signature_keys_follow_the_certificate, NULL },
    { "the_alternate_keys_follow_the_certificate",
      t_the_alternate_keys_follow_the_certificate, NULL },
    { "the_keys_do_not_move", t_the_keys_do_not_move, NULL },
};

DEFINE_GROUP(xbe_titlekeys, "xbe/titlekeys");
