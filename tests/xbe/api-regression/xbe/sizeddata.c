/*
 * Data exports that are a structure rather than a word.
 *
 * The console publishes these at a size its own headers declare, and a
 * title both reads and writes them -- Halo 2 hooks the IDE channel
 * object for DVD streaming, installing routines partway into it.  A
 * kernel that publishes one of these too small does not merely hand
 * back the wrong content: the title's writes land on whatever follows
 * it in the kernel's data.
 *
 * The content is not checkable across kernels -- the console has a real
 * public key and a live channel object where this kernel deliberately
 * has zeros.  The extent is, and it is the part that bites: nothing
 * else may live inside one of these.
 */

#include "../harness.h"
#include <stddef.h>

/* Every other data export whose address we can take.  If one of these
 * falls inside one of the structures, that structure is too small and
 * the title is about to write over its neighbour. */
typedef struct { const char *name; const void *at; } neighbour_t;

/* Filled at run time: these resolve through import thunks, so their
 * addresses are not compile-time constants. */
static ULONG collect_neighbours(neighbour_t *out)
{
    ULONG n = 0;

    out[n].name = "XboxHardwareInfo";           out[n++].at = &XboxHardwareInfo;
    out[n].name = "XboxKrnlVersion";            out[n++].at = &XboxKrnlVersion;
    out[n].name = "XboxHDKey";                  out[n++].at = XboxHDKey;
    out[n].name = "XboxEEPROMKey";              out[n++].at = XboxEEPROMKey;
    out[n].name = "XboxSignatureKey";           out[n++].at = XboxSignatureKey;
    out[n].name = "XboxLANKey";                 out[n++].at = XboxLANKey;
    out[n].name = "XboxAlternateSignatureKeys";
    out[n++].at = XboxAlternateSignatureKeys;
    out[n].name = "XeImageFileName";            out[n++].at = &XeImageFileName;
    out[n].name = "MmGlobalData";               out[n++].at = &MmGlobalData;
    out[n].name = "ObpObjectHandleTable";
    out[n++].at = &ObpObjectHandleTable;
    return n;
}

/* Every data export the console publishes as a structure rather than a
 * word, and the size it publishes it at. */
typedef struct { const char *name; const void *at; ULONG size; } sized_t;

static ULONG collect_sized(sized_t *out)
{
    ULONG n = 0;

    out[n].name = "XePublicKeyData";      out[n].at = XePublicKeyData;
    out[n++].size = sizeof(XePublicKeyData);
    out[n].name = "IdexChannelObject";    out[n].at = &IdexChannelObject;
    out[n++].size = sizeof(IdexChannelObject);
    out[n].name = "XboxEEPROMKey";        out[n].at = XboxEEPROMKey;
    out[n++].size = sizeof(XboxEEPROMKey);
    out[n].name = "MmGlobalData";         out[n].at = &MmGlobalData;
    out[n++].size = sizeof(MmGlobalData);
    out[n].name = "ObpObjectHandleTable"; out[n].at = &ObpObjectHandleTable;
    out[n++].size = sizeof(ObpObjectHandleTable);
    out[n].name = "XboxHDKey";            out[n].at = XboxHDKey;
    out[n++].size = sizeof(XBOX_KEY_DATA);
    out[n].name = "XboxSignatureKey";     out[n].at = XboxSignatureKey;
    out[n++].size = sizeof(XBOX_KEY_DATA);
    out[n].name = "XboxLANKey";           out[n].at = XboxLANKey;
    out[n++].size = sizeof(XBOX_KEY_DATA);
    return n;
}

/* No two of them may occupy the same bytes.  A kernel that publishes
 * one too small shows up here as an overlap with whatever the linker
 * put after it. */
static bool t_no_sized_export_overlaps_another(void)
{
    sized_t s[16];
    ULONG i, j, n = collect_sized(s);

    for (i = 0; i < n; i++) {
        for (j = i + 1; j < n; j++) {
            const UCHAR *a = (const UCHAR *)s[i].at;
            const UCHAR *b = (const UCHAR *)s[j].at;
            if (a < b + s[j].size && b < a + s[i].size)
                FAIL_AND_RETURN("%s (%u at %p) overlaps %s (%u at %p)",
                                s[i].name, (unsigned)s[i].size, a,
                                s[j].name, (unsigned)s[j].size, b);
        }
    }
    return true;
}

static bool nothing_lives_inside(const char *name, const void *base,
                                 ULONG size)
{
    neighbour_t neighbours[16];
    const UCHAR *start = (const UCHAR *)base;
    const UCHAR *end = start + size;
    ULONG i, count = collect_neighbours(neighbours);

    for (i = 0; i < count; i++) {
        const UCHAR *at = (const UCHAR *)neighbours[i].at;
        if (at > start && at < end)
            FAIL_AND_RETURN("%s (%u bytes at %p) has %s inside it at %p",
                            name, (unsigned)size, base, neighbours[i].name,
                            at);
    }
    return true;
}

/* Microsoft's signing key: 284 bytes, whatever is in them. */
static bool t_the_public_key_has_room_for_a_key(void)
{
    return nothing_lives_inside("XePublicKeyData", XePublicKeyData,
                                sizeof(XePublicKeyData));
}

/* The IDE channel object: the one a title writes into. */
static bool t_the_ide_channel_object_has_room_for_a_hook(void)
{
    return nothing_lives_inside("IdexChannelObject", &IdexChannelObject,
                                sizeof(IdexChannelObject));
}

/* And they do not sit on top of each other. */
static bool t_the_two_do_not_overlap(void)
{
    const UCHAR *key = XePublicKeyData;
    const UCHAR *ide = (const UCHAR *)&IdexChannelObject;

    if (key < ide + sizeof(IdexChannelObject) &&
        ide < key + sizeof(XePublicKeyData))
        FAIL_AND_RETURN("XePublicKeyData at %p (%u) overlaps "
                        "IdexChannelObject at %p (%u)",
                        key, (unsigned)sizeof(XePublicKeyData),
                        ide, (unsigned)sizeof(IdexChannelObject));
    return true;
}

/* The offsets Halo 2 installs its DVD-streaming hook at have to be
 * inside the object, not past it. */
static bool t_a_titles_hook_offsets_are_inside_the_object(void)
{
    ASSERT_TRUE(sizeof(IdexChannelObject) > 0x20);
    ASSERT_TRUE(offsetof(IDE_CHANNEL_OBJECT, StartPacketRoutine) == 0x10);
    ASSERT_TRUE(offsetof(IDE_CHANNEL_OBJECT, StartNextPacketRoutine) == 0x14);
    ASSERT_TRUE(offsetof(IDE_CHANNEL_OBJECT, CurrentIrp) == 0x20);
    return true;
}

static const test_entry_t xbe_sizeddata_entries[] = {
    { "the_public_key_has_room_for_a_key",
      t_the_public_key_has_room_for_a_key, NULL },
    { "the_ide_channel_object_has_room_for_a_hook",
      t_the_ide_channel_object_has_room_for_a_hook, NULL },
    { "the_two_do_not_overlap", t_the_two_do_not_overlap, NULL },
    { "a_titles_hook_offsets_are_inside_the_object",
      t_a_titles_hook_offsets_are_inside_the_object, NULL },
    { "no_sized_export_overlaps_another",
      t_no_sized_export_overlaps_another, NULL },
};

DEFINE_GROUP(xbe_sizeddata, "xbe/sizeddata");
