/*
 * HalWriteSMCScratchRegister: the byte in the SMC that survives a warm
 * reboot, and is how the dashboard is told why it was launched.
 *
 * It is readable back through HalReadSMBusValue, so the write is
 * directly observable.  Every case restores what it found -- the flag
 * bits in there decide what the next boot does, and the short-animation
 * bit is normally set.
 */

#include "../harness.h"

#define SMC_ADDRESS  0x20
#define SMC_SCRATCH  0x1B

static bool scratch(ULONG *out)
{
    ASSERT_NTSTATUS(HalReadSMBusValue(SMC_ADDRESS, SMC_SCRATCH, FALSE, out),
                    STATUS_SUCCESS);
    return true;
}

/* What goes in comes back out of the SMC. */
static bool t_the_byte_written_reads_back(void)
{
    ULONG original, v;

    if (!scratch(&original)) return false;

    ASSERT_NTSTATUS(HalWriteSMCScratchRegister(0x05), STATUS_SUCCESS);
    if (!scratch(&v)) return false;
    ASSERT_EQ_U32(v, 0x05);

    ASSERT_NTSTATUS(HalWriteSMCScratchRegister(0), STATUS_SUCCESS);
    if (!scratch(&v)) return false;
    ASSERT_EQ_U32(v, 0);

    ASSERT_NTSTATUS(HalWriteSMCScratchRegister(original), STATUS_SUCCESS);
    if (!scratch(&v)) return false;
    ASSERT_EQ_U32(v, original);
    return true;
}

/* The register is one byte wide: the rest of the caller's word is
 * dropped rather than refused. */
static bool t_only_the_low_byte_reaches_the_smc(void)
{
    ULONG original, v;

    if (!scratch(&original)) return false;

    ASSERT_NTSTATUS(HalWriteSMCScratchRegister(0x1234FF04), STATUS_SUCCESS);
    if (!scratch(&v)) return false;
    ASSERT_EQ_U32(v, 0x04);

    ASSERT_NTSTATUS(HalWriteSMCScratchRegister(0xFFFFFF00), STATUS_SUCCESS);
    if (!scratch(&v)) return false;
    ASSERT_EQ_U32(v, 0);

    ASSERT_NTSTATUS(HalWriteSMCScratchRegister(original), STATUS_SUCCESS);
    return true;
}

/* The same byte is what a plain SMBus write to that register leaves --
 * the ordinal is that write, not a different path to it. */
static bool t_it_is_the_smbus_write(void)
{
    ULONG original, v;

    if (!scratch(&original)) return false;

    ASSERT_NTSTATUS(HalWriteSMBusValue(SMC_ADDRESS, SMC_SCRATCH, FALSE, 0x21),
                    STATUS_SUCCESS);
    if (!scratch(&v)) return false;
    ASSERT_EQ_U32(v, 0x21);

    ASSERT_NTSTATUS(HalWriteSMCScratchRegister(0x21), STATUS_SUCCESS);
    if (!scratch(&v)) return false;
    ASSERT_EQ_U32(v, 0x21);

    ASSERT_NTSTATUS(HalWriteSMCScratchRegister(original), STATUS_SUCCESS);
    return true;
}

static const test_entry_t hal_smc_entries[] = {
    { "the_byte_written_reads_back", t_the_byte_written_reads_back, NULL },
    { "only_the_low_byte_reaches_the_smc",
      t_only_the_low_byte_reaches_the_smc, NULL },
    { "it_is_the_smbus_write", t_it_is_the_smbus_write, NULL },
};

DEFINE_GROUP(hal_smc, "hal/smc");
