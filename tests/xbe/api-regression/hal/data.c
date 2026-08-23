/*
 * HAL DATA-ordinal exports. Titles crash early if these
 * are the wrong size or contain placeholder zeros, because titles read
 * past short stubs into adjacent memory. Each test below probes a value
 * the kernel must populate at boot.
 */

#include "../harness.h"

static bool t_disk_model_number(void)
{
    ASSERT_TRUE(HalDiskModelNumber.Length > 0);
    ASSERT_TRUE(HalDiskModelNumber.Length <= HalDiskModelNumber.MaximumLength);
    ASSERT_NOT_NULL(HalDiskModelNumber.Buffer);
    /* Buffer should be ASCII -- catches the case where the symbol is sized
     * 4 bytes but the title walks 8 and reads garbage past the end. */
    for (USHORT i = 0; i < HalDiskModelNumber.Length; i++) {
        UCHAR c = (UCHAR)HalDiskModelNumber.Buffer[i];
        if (c < 0x20 || c > 0x7E) FAIL_AND_RETURN("non-ASCII at byte %u: 0x%02x", i, c);
    }
    return true;
}

static bool t_disk_serial_number(void)
{
    ASSERT_TRUE(HalDiskSerialNumber.Length > 0);
    ASSERT_TRUE(HalDiskSerialNumber.Length <= HalDiskSerialNumber.MaximumLength);
    ASSERT_NOT_NULL(HalDiskSerialNumber.Buffer);
    for (USHORT i = 0; i < HalDiskSerialNumber.Length; i++) {
        UCHAR c = (UCHAR)HalDiskSerialNumber.Buffer[i];
        if (c < 0x20 || c > 0x7E) FAIL_AND_RETURN("non-ASCII at byte %u: 0x%02x", i, c);
    }
    return true;
}

static bool t_disk_cache_partition_count(void)
{
    /* Retail has 3 cache partitions; devkit can have more. Anything > 0
     * is OK -- the failure mode we're guarding against is the symbol
     * being half-stubbed and reading as zero. */
    ASSERT_TRUE(HalDiskCachePartitionCount > 0);
    ASSERT_TRUE(HalDiskCachePartitionCount < 32);
    return true;
}

static bool t_xbox_hardware_info_size(void)
{
    /* INTERNAL_USB_HUB is set on every devkit + retail box; if the symbol
     * is only 4 bytes the title's read at offset +4 (GpuRevision) will
     * land on the next symbol's bytes. */
    if (!(XboxHardwareInfo.Flags & XBOX_HW_FLAG_INTERNAL_USB_HUB)) {
        FAIL_AND_RETURN("INTERNAL_USB_HUB not set: Flags=0x%08x",
                        (unsigned)XboxHardwareInfo.Flags);
    }
    /* GpuRevision is the NV2A revision; xemu reports 0xA1 for the retail
     * silicon. Anything nonzero is OK. */
    if (XboxHardwareInfo.GpuRevision == 0)
        FAIL_AND_RETURN("GpuRevision is zero (symbol sized incorrectly?)");
    return true;
}

static bool t_xbox_krnl_version(void)
{
    ASSERT_EQ_U32(XboxKrnlVersion.Major, 1);
    if (XboxKrnlVersion.Build == 0)
        FAIL_AND_RETURN("Build is zero");
    return true;
}

static bool t_boot_smc_video_mode(void)
{
    /* SMC video-mode byte must be a recognized value. 0 means the kernel
     * never queried the SMC or the symbol is stubbed. */
    if (HalBootSMCVideoMode == 0)
        FAIL_AND_RETURN("HalBootSMCVideoMode is zero");
    return true;
}

static const test_entry_t hal_data_entries[] = {
    {"disk_model_number",            t_disk_model_number},
    {"disk_serial_number",           t_disk_serial_number},
    {"disk_cache_partition_count",   t_disk_cache_partition_count},
    {"xbox_hardware_info_size",      t_xbox_hardware_info_size},
    {"xbox_krnl_version",            t_xbox_krnl_version},
    {"boot_smc_video_mode",          t_boot_smc_video_mode},
};

DEFINE_GROUP(hal_data, "hal/data");
