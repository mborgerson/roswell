/*
 * PROJECT:     nxkrnl -- a free kernel for the original Xbox
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     The keys a title signs and links with.
 *
 * These three are not the console's and not the kernel's: they belong to
 * the title.  Each is an HMAC of a field of the running image's own
 * certificate, under one key the kernel holds, and every title therefore
 * gets a different set.  Two titles' XBEs carry different certificate
 * fields, so nothing here is shared between them; an image whose
 * certificate fields are zero -- which is every image an open toolchain
 * builds -- gets one key repeated, because the same input went in.
 *
 * The key the HMAC runs under is left zero.  Filling it in with a real
 * console's would make save signatures and system-link traffic
 * interchangeable with a retail box, and nothing needs that yet; the
 * derivation is the part that matters, and it is right either way.
 * Point NxkTitleCertificateKey at a real key and these become retail's.
 */

#include <ntdef.h>
#include <ntifs.h>

/* HMAC-SHA1, the kernel's own (xb/crypto.c). */
VOID NxkHmacSha1(_In_reads_bytes_(KeyLength) const VOID *Key,
                 _In_ ULONG KeyLength,
                 _In_reads_bytes_(Length) const VOID *Data, _In_ ULONG Length,
                 _Out_writes_(20) PUCHAR Digest);

#define XBOX_KEY_LENGTH        16
#define XBOX_ALTERNATE_KEYS    16

/* Certificate fields, from its start. */
#define CERT_SIZE_OFFSET       0x00
#define CERT_LAN_KEY           0xB0
#define CERT_SIGNATURE_KEY     0xC0
#define CERT_ALTERNATE_KEYS    0xD0

/* A certificate only carries the alternate keys if it reaches past them. */
#define CERT_SIZE_WITH_ALTERNATES \
    (CERT_ALTERNATE_KEYS + XBOX_ALTERNATE_KEYS * XBOX_KEY_LENGTH)

UCHAR XboxLANKey[XBOX_KEY_LENGTH];
UCHAR XboxSignatureKey[XBOX_KEY_LENGTH];
UCHAR XboxAlternateSignatureKeys[XBOX_ALTERNATE_KEYS][XBOX_KEY_LENGTH];

static const UCHAR NxkTitleCertificateKey[XBOX_KEY_LENGTH] = { 0 };

/* The digest is longer than the key; only its front is kept. */
static VOID
NxkDeriveKey(_In_reads_bytes_(XBOX_KEY_LENGTH) const UCHAR *Field,
             _Out_writes_(XBOX_KEY_LENGTH) PUCHAR Key)
{
    UCHAR Digest[20];

    NxkHmacSha1(NxkTitleCertificateKey, sizeof(NxkTitleCertificateKey),
                Field, XBOX_KEY_LENGTH, Digest);
    RtlCopyMemory(Key, Digest, XBOX_KEY_LENGTH);
}

/*
 * Run once the image's headers are in memory and before its thunks are
 * resolved, so the keys are in place before anything can read them.
 */
VOID
NxkDeriveTitleKeys(_In_ PVOID Certificate)
{
    const UCHAR *Cert = (const UCHAR *)Certificate;
    ULONG Size, i;

    RtlZeroMemory(XboxLANKey, sizeof(XboxLANKey));
    RtlZeroMemory(XboxSignatureKey, sizeof(XboxSignatureKey));
    RtlZeroMemory(XboxAlternateSignatureKeys,
                  sizeof(XboxAlternateSignatureKeys));

    if (Cert == NULL)
        return;

    RtlCopyMemory(&Size, Cert + CERT_SIZE_OFFSET, sizeof(Size));

    NxkDeriveKey(Cert + CERT_LAN_KEY, XboxLANKey);
    NxkDeriveKey(Cert + CERT_SIGNATURE_KEY, XboxSignatureKey);

    if (Size < CERT_SIZE_WITH_ALTERNATES)
        return;

    for (i = 0; i < XBOX_ALTERNATE_KEYS; i++)
        NxkDeriveKey(Cert + CERT_ALTERNATE_KEYS + i * XBOX_KEY_LENGTH,
                     XboxAlternateSignatureKeys[i]);
}

/* EOF */
