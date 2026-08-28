/*
 * ExQueryNonVolatileSetting: the console's settings, out of the EEPROM.
 *
 * A title can read the same part over SMBus, so nothing here needs
 * hardcoded values: every case reads the setting through the ordinal
 * and the bytes behind it off the bus, and compares.  That checks the
 * kernel's index-to-offset table against the part itself, on whichever
 * kernel is running.
 */

#include "../harness.h"
#include <string.h>

#define EEPROM_ADDRESS 0xA8

#define NVS_BINARY     3
#define NVS_DWORD      4

#ifndef STATUS_BUFFER_TOO_SMALL
#define STATUS_BUFFER_TOO_SMALL ((NTSTATUS)0xC0000023L)
#endif
#ifndef STATUS_OBJECT_NAME_NOT_FOUND
#define STATUS_OBJECT_NAME_NOT_FOUND ((NTSTATUS)0xC0000034L)
#endif

static bool eeprom_bytes(ULONG offset, ULONG length, UCHAR *out)
{
    ULONG i;

    for (i = 0; i < length; i++) {
        ULONG v = 0;
        NTSTATUS s = HalReadSMBusValue(EEPROM_ADDRESS, (UCHAR)(offset + i),
                                       FALSE, &v);
        if (!NT_SUCCESS(s))
            FAIL_AND_RETURN("SMBus read of EEPROM 0x%02x: 0x%08x",
                            (unsigned)(offset + i), (unsigned)s);
        out[i] = (UCHAR)v;
    }
    return true;
}

/* One setting, against the bytes it is made of. */
static bool setting_matches(ULONG index, ULONG offset, ULONG length,
                            ULONG type)
{
    UCHAR got[32], want[32];
    ULONG got_type = 0, got_len = 0;
    NTSTATUS s;

    memset(got, 0xAA, sizeof(got));
    s = ExQueryNonVolatileSetting(index, &got_type, got, sizeof(got),
                                  &got_len);
    if (!NT_SUCCESS(s))
        FAIL_AND_RETURN("index 0x%04x: 0x%08x", (unsigned)index, (unsigned)s);
    if (got_len != length)
        FAIL_AND_RETURN("index 0x%04x: length %u expected %u",
                        (unsigned)index, (unsigned)got_len, (unsigned)length);
    if (got_type != type)
        FAIL_AND_RETURN("index 0x%04x: type %u expected %u",
                        (unsigned)index, (unsigned)got_type, (unsigned)type);
    if (!eeprom_bytes(offset, length, want))
        return false;
    if (memcmp(got, want, length) != 0) {
        /* Say where it really lives rather than only that it moved. */
        ULONG probe;
        for (probe = 0x30; probe + length <= 0x100; probe += 4) {
            UCHAR at[32];
            if (!eeprom_bytes(probe, length, at))
                break;
            if (memcmp(got, at, length) == 0)
                tap_comment("  index 0x%04x matches EEPROM 0x%02x",
                            (unsigned)index, (unsigned)probe);
        }
        FAIL_AND_RETURN("index 0x%04x: does not match EEPROM 0x%02x",
                        (unsigned)index, (unsigned)offset);
    }

    /* The rest of the caller's buffer is zeroed, not left alone. */
    for (ULONG i = length; i < sizeof(got); i++)
        if (got[i] != 0)
            FAIL_AND_RETURN("index 0x%04x left 0x%02x at +%u",
                            (unsigned)index, got[i], (unsigned)i);
    return true;
}

/* The clock settings, which the dashboard writes and every title reads. */
static bool t_the_time_zone_comes_from_the_user_section(void)
{
    return setting_matches(0x0000, 0x64, 4, NVS_DWORD) &&
           setting_matches(0x0001, 0x68, 4, NVS_BINARY) &&
           setting_matches(0x0002, 0x78, 4, NVS_BINARY) &&
           setting_matches(0x0003, 0x88, 4, NVS_DWORD) &&
           setting_matches(0x0004, 0x6C, 4, NVS_BINARY) &&
           setting_matches(0x0005, 0x7C, 4, NVS_BINARY) &&
           setting_matches(0x0006, 0x8C, 4, NVS_DWORD);
}

/* Language, video, audio and the rest of what a title reads to present
 * itself. */
static bool t_the_user_settings_come_from_the_user_section(void)
{
    return setting_matches(0x0007, 0x90, 4, NVS_DWORD) &&
           setting_matches(0x0008, 0x94, 4, NVS_DWORD) &&
           setting_matches(0x0009, 0x98, 4, NVS_DWORD) &&
           setting_matches(0x000A, 0x9C, 4, NVS_DWORD) &&
           setting_matches(0x000B, 0xA0, 4, NVS_DWORD) &&
           setting_matches(0x000C, 0xA4, 4, NVS_DWORD) &&
           setting_matches(0x0011, 0xB8, 4, NVS_DWORD) &&
           setting_matches(0x0012, 0xBC, 4, NVS_DWORD);
}

/* The network addresses, which are four more of the same. */
static bool t_the_network_settings_come_from_the_user_section(void)
{
    return setting_matches(0x000D, 0xA8, 4, NVS_DWORD) &&
           setting_matches(0x000E, 0xAC, 4, NVS_DWORD) &&
           setting_matches(0x000F, 0xB0, 4, NVS_DWORD) &&
           setting_matches(0x0010, 0xB4, 4, NVS_DWORD);
}

/* The factory section: written before the console shipped, and the only
 * settings that are not four bytes wide. */
static bool t_the_factory_settings_are_where_they_were_written(void)
{
    return setting_matches(0x0100, 0x34, 12, NVS_BINARY) &&
           setting_matches(0x0101, 0x40,  6, NVS_BINARY) &&
           setting_matches(0x0102, 0x48, 16, NVS_BINARY) &&
           setting_matches(0x0103, 0x58,  4, NVS_DWORD);
}

/* --- opening the EEPROM's sealed section, title-side ---------------------
 *
 * The game region matches no stored byte of the part: it lives in the
 * 0x1C sealed bytes at 0x14.  To place it the way every other setting
 * here is placed, the test opens that section itself out of the bytes it
 * reads over the bus, and compares.  That needs the console's doubled
 * SHA-1 -- ordinary SHA-1 driven from a version-specific start state
 * with the length counter preset to one block -- which no exported
 * ordinal offers, so it is written out here.
 */

static ULONG sha1_rol(int n, ULONG x) { return (x << n) | (x >> (32 - n)); }

static void sha1_block(ULONG *st, const UCHAR *blk)
{
    static const ULONG k[] = { 0x5A827999, 0x6ED9EBA1, 0x8F1BBCDC, 0xCA62C1D6 };
    ULONG w[80], a = st[0], b = st[1], c = st[2], d = st[3], e = st[4], t;
    int i;

    for (i = 0; i < 16; i++)
        w[i] = ((ULONG)blk[i * 4] << 24) | ((ULONG)blk[i * 4 + 1] << 16) |
               ((ULONG)blk[i * 4 + 2] << 8) | blk[i * 4 + 3];
    for (i = 16; i < 80; i++)
        w[i] = sha1_rol(1, w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16]);

    for (i = 0; i < 80; i++) {
        ULONG f = i < 20 ? ((b & c) | (~b & d))
                : i < 40 ? (b ^ c ^ d)
                : i < 60 ? ((b & c) | (b & d) | (c & d))
                : (b ^ c ^ d);
        t = sha1_rol(5, a) + f + e + w[i] + k[i / 20];
        e = d; d = c; c = sha1_rol(30, b); b = a; a = t;
    }
    st[0] += a; st[1] += b; st[2] += c; st[3] += d; st[4] += e;
}

/* One pass: start state `iv`, length counter preset to 512 bits. */
static void sha1_pass(const ULONG *iv, const UCHAR *data, ULONG len,
                      UCHAR *out)
{
    UCHAR blk[64];
    ULONG st[5], bits = 512 + (len << 3), i, n = 0;

    memcpy(st, iv, sizeof(st));
    while (len - n >= 64) { sha1_block(st, data + n); n += 64; }

    memset(blk, 0, sizeof(blk));
    memcpy(blk, data + n, len - n);
    blk[len - n] = 0x80;
    if (len - n >= 56) { sha1_block(st, blk); memset(blk, 0, sizeof(blk)); }
    blk[60] = (UCHAR)(bits >> 24); blk[61] = (UCHAR)(bits >> 16);
    blk[62] = (UCHAR)(bits >> 8);  blk[63] = (UCHAR)bits;
    sha1_block(st, blk);

    for (i = 0; i < 20; i++)
        out[i] = (UCHAR)(st[i >> 2] >> (8 * (3 - (i & 3))));
}

static void xbox_sha1(const ULONG *iv1, const ULONG *iv2, const UCHAR *data,
                      ULONG len, UCHAR *out)
{
    UCHAR inner[20];
    sha1_pass(iv1, data, len, inner);
    sha1_pass(iv2, inner, 20, out);
}

static void eeprom_rc4(const UCHAR *key, ULONG keylen, UCHAR *data, ULONG len)
{
    UCHAR s[256];
    ULONG i, j = 0, n;

    for (i = 0; i < 256; i++) s[i] = (UCHAR)i;
    for (i = 0; i < 256; i++) {
        UCHAR t;
        j = (j + s[i] + key[i % keylen]) & 0xFF;
        t = s[i]; s[i] = s[j]; s[j] = t;
    }
    i = j = 0;
    for (n = 0; n < len; n++) {
        UCHAR t;
        i = (i + 1) & 0xFF;
        j = (j + s[i]) & 0xFF;
        t = s[i]; s[i] = s[j]; s[j] = t;
        data[n] ^= s[(s[i] + s[j]) & 0xFF];
    }
}

/* Which set sealed a part is not recorded in it: try all four and keep
 * the one whose recomputed hash matches the stored one. */
static const ULONG eeprom_keys[][2][5] = {
    { { 0x85F9E51A, 0xE04613D2, 0x6D86A50C, 0x77C32E3C, 0x4BD717A4 },
      { 0x5D7A9C6B, 0xE1922BEB, 0xB82CCDBC, 0x3137AB34, 0x486B52B3 } },
    { { 0x72127625, 0x336472B9, 0xBE609BEA, 0xF55E226B, 0x99958DAC },
      { 0x76441D41, 0x4DE82659, 0x2E8EF85E, 0xB256FACA, 0xC4FE2DE8 } },
    { { 0x39B06E79, 0xC9BD25E8, 0xDBC6B498, 0x40B4389D, 0x86BBD7ED },
      { 0x9B49BED3, 0x84B430FC, 0x6B8749CD, 0xEBFE5FE5, 0xD96E7393 } },
    { { 0x8058763A, 0xF97D4E0E, 0x865A9762, 0x8A3D920D, 0x08995B2C },
      { 0x01075307, 0xA2F1E037, 0x1186EEEA, 0x88DA9992, 0x168A5609 } },
};

/* Reads the part over the bus and opens its sealed 0x1C bytes into
 * `plain`.  Returns false having already failed the case if the part
 * cannot be read or no key set matches it. */
static bool eeprom_open_sealed(UCHAR *plain)
{
    UCHAR hash[20], key[20], check[20];
    ULONG i;

    if (!eeprom_bytes(0x00, 20, hash))
        return false;

    for (i = 0; i < sizeof(eeprom_keys) / sizeof(eeprom_keys[0]); i++) {
        xbox_sha1(eeprom_keys[i][0], eeprom_keys[i][1], hash, 20, key);
        if (!eeprom_bytes(0x14, 0x1C, plain))
            return false;
        eeprom_rc4(key, 20, plain, 0x1C);
        xbox_sha1(eeprom_keys[i][0], eeprom_keys[i][1], plain, 0x1C, check);
        if (memcmp(check, hash, 20) == 0)
            return true;
    }

    FAIL_AND_RETURN("no EEPROM key set opens this part");
}

/* The game region is the one setting that is sealed rather than stored,
 * so it is placed against the opened section instead of the raw part. */
static bool t_the_game_region_answers(void)
{
    UCHAR buf[8], plain[0x1C];
    ULONG type = 0, len = 0;

    memset(buf, 0xAA, sizeof(buf));
    ASSERT_NTSTATUS(ExQueryNonVolatileSetting(0x0104, &type, buf, sizeof(buf),
                                              &len),
                    STATUS_SUCCESS);
    ASSERT_EQ_U32(type, NVS_DWORD);
    ASSERT_EQ_U32(len, 4);

    if (!eeprom_open_sealed(plain))
        return false;

    /* The section is confounder[8], HDD key[16], then the region. */
    if (memcmp(buf, plain + 0x18, 4) != 0)
        FAIL_AND_RETURN("game region 0x%08x, sealed section holds 0x%08x",
                        (unsigned)*(ULONG *)buf,
                        (unsigned)*(ULONG *)(plain + 0x18));

    /* Unlike every stored setting, this one does not zero what it does
     * not fill: the rest of the caller's buffer comes back untouched. */
    for (ULONG i = 4; i < sizeof(buf); i++)
        if (buf[i] != 0xAA)
            FAIL_AND_RETURN("byte %u past the region is 0x%02x, not left alone",
                            (unsigned)i, buf[i]);
    return true;
}

/* A buffer too small for the setting is refused, and the caller is not
 * told how much to bring: the length comes back zero. */
static bool t_a_short_buffer_is_refused_without_a_length(void)
{
    UCHAR small[2];
    ULONG type = 0x1234, len = 0x5678;

    ASSERT_NTSTATUS(ExQueryNonVolatileSetting(0x0007, &type, small,
                                              sizeof(small), &len),
                    STATUS_BUFFER_TOO_SMALL);
    ASSERT_EQ_U32(type, 0x1234);
    ASSERT_EQ_U32(len, 0x5678);

    ASSERT_NTSTATUS(ExQueryNonVolatileSetting(0x0100, &type, small, 0, &len),
                    STATUS_BUFFER_TOO_SMALL);
    ASSERT_EQ_U32(type, 0x1234);
    ASSERT_EQ_U32(len, 0x5678);
    return true;
}

/* An index that is not a setting is refused as a name, and the whole
 * gap between the two sections is such a gap. */
static bool t_an_index_that_is_not_a_setting_is_refused(void)
{
    static const ULONG absent[] = { 0x0013, 0x0014, 0x0080, 0x0105, 0x0200 };
    UCHAR buf[32];
    ULONG i;

    for (i = 0; i < sizeof(absent) / sizeof(absent[0]); i++) {
        ULONG type = 0, len = 0;
        NTSTATUS s = ExQueryNonVolatileSetting(absent[i], &type, buf,
                                               sizeof(buf), &len);
        if (s != STATUS_OBJECT_NAME_NOT_FOUND)
            FAIL_AND_RETURN("index 0x%04x: 0x%08x expected name-not-found",
                            (unsigned)absent[i], (unsigned)s);
    }
    return true;
}

/* The section markers are indices the console knows but will not hand
 * over piecemeal. */
/* Four indices name a run of the part rather than one setting: the two
 * sections, the sealed one, and the whole thing.  Each comes back as
 * the bytes themselves, matching what a title reads over SMBus. */
static bool t_the_sections_are_runs_of_the_part(void)
{
    static const struct {
        ULONG index, offset, length;
        bool whole_part;          /* copies the run and no more */
    } runs[] = {
        { 0x00FF, 0x60, 0x60,  false },   /* user section */
        { 0x01FF, 0x30, 0x30,  false },   /* factory section */
        { 0xFFFE, 0x00, 0x30,  true  },   /* encrypted section */
        { 0xFFFF, 0x00, 0x100, true  },   /* the whole part */
    };
    static UCHAR got[300], want[300];
    ULONG i;

    for (i = 0; i < sizeof(runs) / sizeof(runs[0]); i++) {
        ULONG type = 0, len = 0, j;
        NTSTATUS s;

        memset(got, 0xAA, sizeof(got));
        s = ExQueryNonVolatileSetting(runs[i].index, &type, got, 256, &len);
        if (!NT_SUCCESS(s))
            FAIL_AND_RETURN("section 0x%04x: 0x%08x", (unsigned)runs[i].index,
                            (unsigned)s);
        if (len != runs[i].length)
            FAIL_AND_RETURN("section 0x%04x: length %u expected %u",
                            (unsigned)runs[i].index, (unsigned)len,
                            (unsigned)runs[i].length);
        if (type != NVS_BINARY)
            FAIL_AND_RETURN("section 0x%04x: type %u", (unsigned)runs[i].index,
                            (unsigned)type);
        if (!eeprom_bytes(runs[i].offset, runs[i].length, want))
            return false;
        if (memcmp(got, want, runs[i].length) != 0)
            FAIL_AND_RETURN("section 0x%04x: does not match EEPROM 0x%02x",
                            (unsigned)runs[i].index, (unsigned)runs[i].offset);
        /* A section marker zeroes the rest of the buffer the way a
         * single setting does; the two that name the part itself copy
         * the run and leave the rest alone. */
        for (j = runs[i].length; j < 256; j++) {
            UCHAR expect = runs[i].whole_part ? 0xAA : 0x00;
            if (got[j] != expect)
                FAIL_AND_RETURN("section 0x%04x: 0x%02x at +%u, expected 0x%02x",
                                (unsigned)runs[i].index, got[j], (unsigned)j,
                                expect);
        }
    }
    return true;
}

/* A buffer too small for a run is refused -- and only the two that name
 * the part itself say how much to bring. */
static bool t_only_the_whole_part_reports_its_length(void)
{
    UCHAR small[32];
    ULONG type, len;

    type = 0x1234; len = 0x5678;
    ASSERT_NTSTATUS(ExQueryNonVolatileSetting(0x00FF, &type, small,
                                              sizeof(small), &len),
                    STATUS_BUFFER_TOO_SMALL);
    ASSERT_EQ_U32(len, 0x5678);

    type = 0x1234; len = 0x5678;
    ASSERT_NTSTATUS(ExQueryNonVolatileSetting(0x01FF, &type, small,
                                              sizeof(small), &len),
                    STATUS_BUFFER_TOO_SMALL);
    ASSERT_EQ_U32(len, 0x5678);

    type = 0x1234; len = 0x5678;
    ASSERT_NTSTATUS(ExQueryNonVolatileSetting(0xFFFE, &type, small,
                                              sizeof(small), &len),
                    STATUS_BUFFER_TOO_SMALL);
    ASSERT_EQ_U32(len, 0x30);

    type = 0x1234; len = 0x5678;
    ASSERT_NTSTATUS(ExQueryNonVolatileSetting(0xFFFF, &type, small,
                                              sizeof(small), &len),
                    STATUS_BUFFER_TOO_SMALL);
    ASSERT_EQ_U32(len, 0x100);
    return true;
}

/* Reading twice gives the same answer: the part is not consumed. */
static bool t_reading_twice_gives_the_same_answer(void)
{
    UCHAR first[8], second[8];
    ULONG type = 0, len = 0;

    ASSERT_NTSTATUS(ExQueryNonVolatileSetting(0x0007, &type, first,
                                              sizeof(first), &len),
                    STATUS_SUCCESS);
    ASSERT_NTSTATUS(ExQueryNonVolatileSetting(0x0007, &type, second,
                                              sizeof(second), &len),
                    STATUS_SUCCESS);
    ASSERT_TRUE(memcmp(first, second, 4) == 0);
    return true;
}

static const test_entry_t ex_eeprom_entries[] = {
    { "the_time_zone_comes_from_the_user_section",
      t_the_time_zone_comes_from_the_user_section, NULL },
    { "the_user_settings_come_from_the_user_section",
      t_the_user_settings_come_from_the_user_section, NULL },
    { "the_network_settings_come_from_the_user_section",
      t_the_network_settings_come_from_the_user_section, NULL },
    { "the_factory_settings_are_where_they_were_written",
      t_the_factory_settings_are_where_they_were_written, NULL },
    { "the_game_region_answers", t_the_game_region_answers, NULL },
    { "a_short_buffer_is_refused_without_a_length",
      t_a_short_buffer_is_refused_without_a_length, NULL },
    { "an_index_that_is_not_a_setting_is_refused",
      t_an_index_that_is_not_a_setting_is_refused, NULL },
    { "the_sections_are_runs_of_the_part",
      t_the_sections_are_runs_of_the_part, NULL },
    { "only_the_whole_part_reports_its_length",
      t_only_the_whole_part_reports_its_length, NULL },
    { "reading_twice_gives_the_same_answer",
      t_reading_twice_gives_the_same_answer, NULL },
};

DEFINE_GROUP(ex_eeprom, "ex/eeprom");
