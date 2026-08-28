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

/* HalEnableSecureTrayEject hands the eject button to the title.  What
 * it does about the tray is not visible from here -- under emulation
 * nothing reflects it back -- so what is covered is that it returns,
 * costs the caller nothing, and leaves every SMC register a title can
 * read where it was. */
static bool t_secure_tray_eject_disturbs_nothing(void)
{
    ULONG tray0, tray1, count0, count1, avpack0, avpack1, scratch0, scratch1;

    ASSERT_NTSTATUS(HalReadSMCTrayState(&tray0, &count0), STATUS_SUCCESS);
    ASSERT_NTSTATUS(HalReadSMBusValue(SMC_ADDRESS, 0x04, FALSE, &avpack0),
                    STATUS_SUCCESS);
    if (!scratch(&scratch0)) return false;

    HalEnableSecureTrayEject();
    HalEnableSecureTrayEject();

    ASSERT_EQ_U32(KeGetCurrentIrql(), PASSIVE_LEVEL);
    ASSERT_NTSTATUS(HalReadSMCTrayState(&tray1, &count1), STATUS_SUCCESS);
    ASSERT_NTSTATUS(HalReadSMBusValue(SMC_ADDRESS, 0x04, FALSE, &avpack1),
                    STATUS_SUCCESS);
    if (!scratch(&scratch1)) return false;

    ASSERT_EQ_U32(tray1, tray0);
    ASSERT_EQ_U32(count1, count0);
    ASSERT_EQ_U32(avpack1, avpack0);
    ASSERT_EQ_U32(scratch1, scratch0);
    return true;
}

static const test_entry_t hal_smc_entries[] = {
    { "the_byte_written_reads_back", t_the_byte_written_reads_back, NULL },
    { "only_the_low_byte_reaches_the_smc",
      t_only_the_low_byte_reaches_the_smc, NULL },
    { "it_is_the_smbus_write", t_it_is_the_smbus_write, NULL },
    { "secure_tray_eject_disturbs_nothing",
      t_secure_tray_eject_disturbs_nothing, NULL },
};

DEFINE_GROUP(hal_smc, "hal/smc");
