/*
 * What the block cipher does with an input that is not eight-byte
 * aligned.
 *
 * Found the hard way: a source file added ahead of xc/cipher.c in the
 * link order moved that group's read-only plaintext four bytes off an
 * eight-byte boundary, and the console bugchecked writing to exactly
 * that address -- a write into the caller's INPUT, on a read-only page.
 * Put the same off-boundary input in writable storage, as here, and the
 * call answers correctly and hands the bytes back unchanged, so
 * whatever it writes there it puts back.  Invisible in RAM, fatal in
 * read-only memory.
 *
 * The rule that falls out: keep a cipher input either eight-byte
 * aligned or writable.  This group pins the aligned case and puts the
 * off-boundary observation on the record; it is last in the file and
 * last in the suite because the console can be made to fault here.
 */

#include "../harness.h"
#include <string.h>

/* Writable, so a write the console aims at the input cannot fault. */
static UCHAR g_table[384];
static UCHAR g_in[64];
static UCHAR g_out[64];
static UCHAR g_feedback[8];

static const UCHAR KEY[8] = {
    0x01, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef};

static const UCHAR PLAIN[16] = {
    0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
    0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f};

/* PLAIN through single-DES CBC with a zero feedback -- the retail
 * kernel's own answer, shared with the xc/cipher group. */
static const UCHAR DES_CBC[16] = {
    0x32, 0x60, 0x26, 0x6c, 0x2c, 0xf2, 0x02, 0xe2,
    0x79, 0xcf, 0x70, 0xcd, 0x1d, 0xac, 0x09, 0xa5};

/* Encrypt PLAIN out of g_in + skew into g_out + skew.  Reports whether
 * the call left the input alone and whether the ciphertext is right. */
static void run_at(unsigned skew, bool *input_intact, bool *cipher_ok)
{
    UCHAR *in = g_in + skew;
    UCHAR *out = g_out + skew;

    memset(g_table, 0, sizeof(g_table));
    XcKeyTable(0, g_table, (PUCHAR)KEY);

    memset(g_in, 0, sizeof(g_in));
    memset(g_out, 0xEE, sizeof(g_out));
    memset(g_feedback, 0, sizeof(g_feedback));
    memcpy(in, PLAIN, sizeof(PLAIN));

    XcBlockCryptCBC(0, sizeof(PLAIN), out, in, g_table, 1, g_feedback);

    *input_intact = memcmp(in, PLAIN, sizeof(PLAIN)) == 0;
    *cipher_ok = memcmp(out, DES_CBC, sizeof(DES_CBC)) == 0;
}

/* The baseline: on the boundary the call reads the input and writes
 * only the output. */
static bool t_aligned_input_is_read_only(void)
{
    bool intact, ok;

    run_at(0, &intact, &ok);
    if (!ok)
        FAIL_AND_RETURN("aligned ciphertext wrong: %02x%02x%02x%02x...",
                        g_out[0], g_out[1], g_out[2], g_out[3]);
    if (!intact)
        FAIL_AND_RETURN("aligned call wrote into its input: "
                        "%02x%02x%02x%02x...",
                        g_in[0], g_in[1], g_in[2], g_in[3]);
    return true;
}

/* Off the boundary, in storage that can absorb the write.  Reported
 * rather than asserted: nothing here is a rule the two kernels have to
 * agree on, and only one of them can be made to fault. */
static bool t_misaligned_input(void)
{
    bool intact, ok;

    run_at(4, &intact, &ok);
    /* The address goes in the report: the finding is about where the
     * buffer sits, so a run that did not actually land off the boundary
     * proves nothing. */
    tap_comment("misaligned: in=%p (mod8=%u) intact=%d cipher_ok=%d "
                "bytes=%02x%02x%02x%02x",
                (void *)(g_in + 4), (unsigned)(((ULONG_PTR)(g_in + 4)) & 7u),
                (int)intact, (int)ok,
                g_in[4], g_in[5], g_in[6], g_in[7]);
    return true;
}

static const test_entry_t xc_align_entries[] = {
    { "aligned_input_is_read_only", t_aligned_input_is_read_only, NULL },
    { "misaligned_input", t_misaligned_input, NULL },
};

DEFINE_GROUP(xc_align, "xc/align");
