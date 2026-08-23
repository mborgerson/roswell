/*
 * PROJECT:     Xbox HAL
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Shared globals + boot-time entry point for the ported
 *              cromwell video driver.
 */

#include "xbox-video-compat.h"
#include "BootVideo.h"

/* Definitions for the globals exported by xbox-video-compat.h. */
unsigned int video_encoder = 0;
u8 VIDEO_AV_MODE = 0;

/* 128 MiB devkit (kernel IMAGEBASE 0x84000000, PA 0x04000000 = the 64 MiB
 * mark, only valid on a 128 MiB box).  Cromwell branches on this in
 * BootVgaInitialization to pick PFB
 * timing (0x03070103 for 128 MiB vs 0x03070003 for 64 MiB).  When the
 * kernel later shrinks back to PA 0x10000 and the build can run on the
 * 64 MiB retail config, this becomes runtime-detected -- see
 * BootDetectMemorySize in xbox/pci-bringup/cromwell-pci.c. */
unsigned int xbox_ram = 128;

/* 256-byte EEPROM shadow.  BootVgaInitializationKernelNG fills this from
 * SMBus slave 0x54 before referencing eeprom[0x96] (widescreen flag). */
DATA_SEG("INITDATA_RW")
u8 eeprom[256] = { 0 };

static VOID
NxkVideoReadEeprom(VOID)
{
    UCHAR i;
    UCHAR b;

    for (i = 0; i < 0xFF; i++)
    {
        if (NT_SUCCESS(HalpXboxSmBusReadByte(0x54, i, &b)))
            eeprom[i] = b;
        else
            eeprom[i] = 0;
    }
}

/* Kernel-side entry: programs the NV2A CRTC + TV encoder at boot.
 * Called from HalpInitPhase0 after NxConfigurePciDevices(). */
VOID
NxkInitializeVideo(VOID)
{
    /* Transient: only read inside this boot-time routine, so keep it on
     * the (INIT) stack rather than burning a resident .bss slot. */
    CURRENT_VIDEO_MODE_DETAILS NxkVideoMode;

    DbgPrint("nxkrnl-video: BootVgaInitializationKernelNG enter\n");

    NxkVideoReadEeprom();

    BootVgaInitializationKernelNG(&NxkVideoMode);

    DbgPrint("nxkrnl-video: BootVgaInitializationKernelNG done "
             "(encoder=%u av_mode=%u w=%lu h=%lu)\n",
             video_encoder, (unsigned)VIDEO_AV_MODE,
             (unsigned long)NxkVideoMode.width,
             (unsigned long)NxkVideoMode.height);
}
