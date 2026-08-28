/*
 * XcUpdateCrypto -- every Xc entry the console publishes dispatches
 * through a vector a title can replace, and this is how a title replaces
 * it.  The cases below pin what the retail kernel does: the export is a
 * thunk, an installed vector diverts it, and the out-parameter always
 * reports the ROM's own table rather than whatever was installed last.
 */

#include "../harness.h"
#include <string.h>

static unsigned g_sha_hits, g_key_hits, g_service_hits;

static VOID NTAPI hook_sha_init(PUCHAR ctx)
{
    (void)ctx;
    g_sha_hits++;
}

static VOID NTAPI hook_key_table(ULONG cipher, PUCHAR table, PUCHAR key)
{
    (void)cipher; (void)table; (void)key;
    g_key_hits++;
}

static ULONG NTAPI hook_service(ULONG op, PVOID args)
{
    (void)op; (void)args;
    g_service_hits++;
    return 0x5AA5;
}

/* A vector that keeps every entry the caller already has. */
static void unchanged(CRYPTO_VECTOR *v)
{
    v->pXcSHAInit = XcSHAInit;
    v->pXcSHAUpdate = XcSHAUpdate;
    v->pXcSHAFinal = XcSHAFinal;
    v->pXcRC4Key = XcRC4Key;
    v->pXcRC4Crypt = XcRC4Crypt;
    v->pXcHMAC = XcHMAC;
    v->pXcPKEncPublic = XcPKEncPublic;
    v->pXcPKDecPrivate = XcPKDecPrivate;
    v->pXcPKGetKeyLen = XcPKGetKeyLen;
    v->pXcVerifyPKCS1Signature = XcVerifyPKCS1Signature;
    v->pXcModExp = XcModExp;
    v->pXcDESKeyParity = XcDESKeyParity;
    v->pXcKeyTable = XcKeyTable;
    v->pXcBlockCrypt = XcBlockCrypt;
    v->pXcBlockCryptCBC = XcBlockCryptCBC;
    v->pXcCryptService = XcCryptService;
}

/*
 * Read the ROM's table without leaving anything else installed.  The
 * vector held between the two calls names the exported thunks, so
 * nothing may call an Xc entry in that window -- it would come straight
 * back through the thunk.
 */
static void read_rom(CRYPTO_VECTOR *rom)
{
    CRYPTO_VECTOR keep;

    unchanged(&keep);
    XcUpdateCrypto(&keep, rom);
    XcUpdateCrypto(rom, NULL);
}

/* The exported entry is a thunk, not the routine the vector names. */
static bool t_export_is_a_thunk(void)
{
    CRYPTO_VECTOR rom;

    memset(&rom, 0, sizeof(rom));
    read_rom(&rom);
    ASSERT_NOT_NULL(rom.pXcSHAInit);
    ASSERT_NOT_NULL(rom.pXcKeyTable);
    ASSERT_NOT_NULL(rom.pXcCryptService);
    ASSERT_TRUE(rom.pXcSHAInit != XcSHAInit);
    ASSERT_TRUE(rom.pXcKeyTable != XcKeyTable);
    return true;
}

/* Installing a vector diverts the exports it names, return value and all. */
static bool t_update_diverts(void)
{
    CRYPTO_VECTOR nv, rom;
    unsigned char ctx[128], key[8], table[128];
    ULONG service;
    bool ok = true;

    memset(key, 0, sizeof(key));
    read_rom(&rom);
    nv = rom;
    nv.pXcSHAInit = hook_sha_init;
    nv.pXcKeyTable = hook_key_table;
    nv.pXcCryptService = hook_service;

    g_sha_hits = g_key_hits = g_service_hits = 0;
    XcUpdateCrypto(&nv, NULL);

    XcSHAInit(ctx);
    XcKeyTable(0, table, key);
    service = XcCryptService(3, NULL);

    /* Put the console back before reporting, whatever happened. */
    XcUpdateCrypto(&rom, NULL);

    if (g_sha_hits != 1 || g_key_hits != 1 || g_service_hits != 1) {
        test_record_failure(__FILE__, __LINE__,
            "hits sha=%u keytable=%u service=%u, expected one each",
            g_sha_hits, g_key_hits, g_service_hits);
        ok = false;
    }
    if (ok && service != 0x5AA5) {
        test_record_failure(__FILE__, __LINE__,
            "service returned 0x%08x, expected the hook's 0x5AA5",
            (unsigned)service);
        ok = false;
    }

    /* And the restore took: the export is the kernel's again. */
    g_sha_hits = 0;
    XcSHAInit(ctx);
    if (ok && g_sha_hits != 0)
        FAIL_AND_RETURN("the hook still ran after the ROM vector went back");
    return ok;
}

/* The out-parameter is the ROM's table, not the one installed last. */
static bool t_reports_the_rom_vector(void)
{
    CRYPTO_VECTOR nv, rom, again;
    bool ok = true;

    memset(&again, 0, sizeof(again));
    read_rom(&rom);
    nv = rom;
    nv.pXcSHAInit = hook_sha_init;

    XcUpdateCrypto(&nv, NULL);
    XcUpdateCrypto(&rom, &again);

    if (again.pXcSHAInit != rom.pXcSHAInit) {
        test_record_failure(__FILE__, __LINE__,
            "the read gave %p, expected the ROM's %p",
            (void *)again.pXcSHAInit, (void *)rom.pXcSHAInit);
        ok = false;
    }
    if (ok && again.pXcSHAInit == hook_sha_init)
        FAIL_AND_RETURN("the out-parameter reported the installed vector");
    return ok;
}

/* Hashing still works once the vector has been round-tripped. */
static bool t_survives_a_round_trip(void)
{
    static const unsigned char want[20] = {
        0xa9,0x99,0x3e,0x36,0x47,0x06,0x81,0x6a,0xba,0x3e,
        0x25,0x71,0x78,0x50,0xc2,0x6c,0x9c,0xd0,0xd8,0x9d};
    unsigned char ctx[128], digest[20];
    unsigned i;

    XcSHAInit(ctx);
    XcSHAUpdate(ctx, (PUCHAR)"abc", 3);
    XcSHAFinal(ctx, digest);
    for (i = 0; i < sizeof(want); i++) {
        if (digest[i] != want[i])
            FAIL_AND_RETURN("sha1(abc) byte %u is 0x%02x, expected 0x%02x",
                            i, digest[i], want[i]);
    }
    return true;
}

/* A slot the caller leaves empty keeps whatever is installed. */
static bool t_empty_slots_keep(void)
{
    CRYPTO_VECTOR rom, hooked, nulled;
    unsigned char ctx[128];
    unsigned hits;

    read_rom(&rom);
    hooked = rom;
    hooked.pXcSHAInit = hook_sha_init;
    XcUpdateCrypto(&hooked, NULL);

    /* The hash slot is empty here, every other one is the ROM's.  Taking
     * the vector wholesale would leave the export pointing at nothing. */
    nulled = rom;
    nulled.pXcSHAInit = NULL;
    XcUpdateCrypto(&nulled, NULL);

    g_sha_hits = 0;
    XcSHAInit(ctx);
    hits = g_sha_hits;
    XcUpdateCrypto(&rom, NULL);

    if (hits != 1)
        FAIL_AND_RETURN("the empty slot replaced the installed routine");
    return true;
}

static const test_entry_t xc_vector_entries[] = {
    {"an_export_is_a_thunk", t_export_is_a_thunk},
    {"an_update_diverts_the_exports", t_update_diverts},
    {"the_out_parameter_is_the_rom_vector", t_reports_the_rom_vector},
    {"an_empty_slot_keeps_what_is_installed", t_empty_slots_keep},
    {"hashing_survives_a_round_trip", t_survives_a_round_trip},
};

DEFINE_GROUP(xc_vector, "xc/vector");
