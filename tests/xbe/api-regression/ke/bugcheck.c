/*
 * KiBugCheckData: the five-word record a bugcheck leaves behind, which
 * the console publishes to titles as a DATA export.
 *
 * What can be covered in-suite is the record at rest -- the ordinal
 * hands back the address of the live array in kernel memory, and it
 * reads as five zero words until something bugchecks.  The other half,
 * that KeBugCheckEx writes its five arguments there verbatim, cannot
 * live here because it does not return: tests/xbe/bugcheck is a one-off
 * XBE for it, read back over the QEMU monitor.
 */

#include "../harness.h"

/* The array itself, not a pointer to it. */
static bool t_the_record_is_the_arrays_own_address(void)
{
    const void *p = (const void *)KiBugCheckData;

    ASSERT_NOT_NULL(p);
    ASSERT_TRUE((ULONG_PTR)p >= 0x80000000);
    ASSERT_TRUE(MmIsAddressValid((PVOID)p));

    /* Two reads of a DATA export are the same address -- it is a
     * variable, not a call that allocates. */
    ASSERT_EQ_PTR((const void *)KiBugCheckData, p);
    return true;
}

/* Nothing has bugchecked, so all five words are zero. */
static bool t_the_record_is_zero_at_rest(void)
{
    unsigned i;

    for (i = 0; i < 5; i++)
        ASSERT_EQ_U32(KiBugCheckData[i], 0);
    return true;
}

static const test_entry_t ke_bugcheck_entries[] = {
    { "the_record_is_the_arrays_own_address",
      t_the_record_is_the_arrays_own_address, NULL },
    { "the_record_is_zero_at_rest", t_the_record_is_zero_at_rest, NULL },
};

DEFINE_GROUP(ke_bugcheck, "ke/bugcheck");
