/*
 * OBJECT_TYPE DATA-ordinal exports. Same failure mode as hal/data --
 * a wrong-size stub corrupts reads at offsets >= 4. Each OBJECT_TYPE is
 * 7 pointers (28 bytes); we sanity-check that PoolTag (the last field)
 * is printable ASCII, which it always is for a real kernel-side init.
 */

#include "../harness.h"

static bool tag_is_printable(ULONG tag)
{
    for (int i = 0; i < 4; i++) {
        UCHAR c = (UCHAR)((tag >> (i * 8)) & 0xff);
        if (c < 0x20 || c > 0x7e) return false;
    }
    return true;
}

static bool check_type(const char *name, OBJECT_TYPE *t)
{
    /* PoolTag is the last field of OBJECT_TYPE (offset +0x18) and must be
     * a printable 4-byte ASCII tag. If a DATA-ordinal stub is sized
     * incorrectly the title reads garbage from this offset; that's the
     * failure mode we're guarding against. */
    if (t->PoolTag == 0) {
        test_record_failure(__FILE__, __LINE__, "%s PoolTag is zero", name);
        return false;
    }
    if (!tag_is_printable(t->PoolTag)) {
        test_record_failure(__FILE__, __LINE__,
            "%s PoolTag=0x%08x not printable", name, (unsigned)t->PoolTag);
        return false;
    }
    return true;
}

static bool t_thread(void)    { return check_type("PsThreadObjectType",   &PsThreadObjectType); }
static bool t_event(void)     { return check_type("ExEventObjectType",    &ExEventObjectType); }
static bool t_semaphore(void) { return check_type("ExSemaphoreObjectType",&ExSemaphoreObjectType); }
static bool t_mutant(void)    { return check_type("ExMutantObjectType",   &ExMutantObjectType); }
static bool t_timer(void)     { return check_type("ExTimerObjectType",    &ExTimerObjectType); }
static bool t_file(void)      { return check_type("IoFileObjectType",     &IoFileObjectType); }
static bool t_device(void)    { return check_type("IoDeviceObjectType",   &IoDeviceObjectType); }

static const test_entry_t ob_types_entries[] = {
    {"thread",    t_thread},
    {"event",     t_event},
    {"semaphore", t_semaphore},
    {"mutant",    t_mutant},
    {"timer",     t_timer},
    {"file",      t_file},
    {"device",    t_device},
};

DEFINE_GROUP(ob_types, "ob/types");
