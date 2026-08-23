/*
 * PROJECT:     nxkrnl -- a free kernel for the original Xbox
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Av -- Xbox display / TV-encoder kernel API.
 *
 * API surface clean-room from the public Av API (nxdk's xboxkrnl.h and
 * hal/video.c); hardware bring-up derived from cromwell's
 * GPL-2.0-or-later drivers/video/BootVgaInitialization.c.
 *
 * Display path, as nxdk's XVideoSetMode drives it:
 *   AvSendTVEncoderOption(GET_SETTINGS) -- query the encoder / AV cable
 *   AvGetSavedDataAddress               -- a prior framebuffer to reclaim?
 *   MmAllocateContiguousMemoryEx        -- the framebuffer
 *   AvSetDisplayMode                    -- program the NV2A CRTC + encoder
 *   AvSendTVEncoderOption(VIDEOENABLE)  -- enable scanout
 *
 * The TV encoder hangs off the SMBus (Conexant CX25871 at 0x45 or Focus at
 * 0x6A): GET_SETTINGS reads the SMC's AV-pack register and the EEPROM's
 * video-standard byte; VIDEOENABLE drives the encoder's output DACs and the
 * ACPI video gate.  AvSetDisplayMode points the NV2A CRTC scan-out at the
 * title's framebuffer.
 */

#include <ntdef.h>
#include <ntifs.h>
#include <intrin.h>
#include <xb-debug.h>


#include "halxbox.h"

/* SMBus slave addresses. */
#define SMBUS_SMC               0x10   /* system management controller    */
#define SMBUS_ENCODER_CONEXANT  0x45   /* Conexant CX25871 TV encoder      */
#define SMBUS_ENCODER_FOCUS     0x6A   /* Focus FS454 TV encoder           */
#define SMBUS_EEPROM            0x54   /* 256-byte serial EEPROM           */

#define SMC_REG_AVPACK          0x04   /* SMC: the detected AV cable/pack  */
#define EEPROM_VIDEO_STANDARD   0x5A   /* EEPROM: 0x40 == NTSC, else PAL   */

/* NV2A display engine: byte offset from RegisterBase (nxdk's VIDEO_BASE,
 * 0xFD000000).  PCRTC_START is the physical RAM address the CRTC scans out.
 *
 * PRMCIO at 0x601000 aliases the VGA CRTC ioports; the display path consults
 * the line-offset spread across CR[0x13]/[0x19]/[0x25] (xemu nv2a.c:177) for
 * its texCoord.y scale.  If we never write these, cromwell's text-mode values
 * persist and the display surface gets sampled with a wrong Y mapping --
 * the symptom is a horizontal gradient (the bottom row of the framebuffer
 * stretched to fill the screen) where pmemsave-of-the-same-surface shows
 * the title's actual triangle. */
#define NV2A_PCRTC_START        0x600800
#define NV2A_PRMCIO_CR_INDEX    0x6013D4
#define NV2A_PRMCIO_CR_DATA     0x6013D5

/* ACPI I/O port gating video output (cromwell BootVgaInitialization.c). */
#define ACPI_VIDEO_GATE         0x80D3
#define ACPI_VIDEO_ON           4
#define ACPI_VIDEO_OFF          5

/* AvSendTVEncoderOption Option codes (nxdk hal/video.h). */
#define AV_OPTION_GET_SETTINGS      6
#define AV_OPTION_VIDEO_ENABLE      9
#define AV_OPTION_FLICKER_FILTER    11
#define AV_OPTION_SOFTEN_FILTER     14

/*
 * GET_SETTINGS result: the AV pack / adapter in the low byte (nxdk's
 * AV_PACK_*), the video standard in the next.  nxdk's XVideoListModes matches
 * a title's requested resolution against this.
 */
#define AV_PACK_STANDARD        1      /* composite                        */
#define AV_PACK_RFU             2
#define AV_PACK_SCART           3
#define AV_PACK_HDTV            4
#define AV_PACK_VGA             5
#define AV_PACK_SVIDEO          6

#define AV_REGION_NTSCM         0x0100
#define AV_REGION_PAL           0x0300

/* Capability flags occupying the high word of GET_SETTINGS.  The title's
 * mode-validation table at .D3D xrefs these against the entry's flags to
 * decide which display modes the AV connection can carry: refresh-rate,
 * HD scan formats, presentation aspect, scan type.  A title rejects every
 * mode whose flags share no bits with these.  Bit positions per the
 * public Xbox SDK / cxbx-reloaded reference. */
#define AV_FLAGS_NORMAL         0x00000000   /* 4:3 (no bit set)          */
#define AV_FLAGS_WIDESCREEN     0x00010000   /* 16:9                      */
#define AV_FLAGS_HDTV_720p      0x00020000
#define AV_FLAGS_HDTV_1080i     0x00040000
#define AV_FLAGS_HDTV_480p      0x00080000
#define AV_FLAGS_LETTERBOX      0x00100000   /* 4:3 letterbox             */
#define AV_FLAGS_INTERLACED     0x00200000
#define AV_FLAGS_60Hz           0x00400000
#define AV_FLAGS_50Hz           0x00800000
#define AV_FLAGS_FIELD          0x01000000   /* per-field scan            */
#define AV_FLAGS_10x11PAR       0x02000000   /* 10:11 pixel aspect ratio  */

/* A framebuffer a prior display owner asked the kernel to preserve across a
 * mode change (AvSetSavedDataAddress).  NULL on a cold boot -- none saved. */
static PVOID NxkAvSavedDataAddress = NULL;

PVOID NTAPI
AvGetSavedDataAddress(VOID)
{
    return NxkAvSavedDataAddress;
}

VOID NTAPI
AvSetSavedDataAddress(_In_opt_ PVOID Address)
{
    NxkAvSavedDataAddress = Address;
}

/* Probe the SMBus for the TV encoder; 0 if neither known encoder answers
 * (an Xcalibur box, not handled here). */
static UCHAR
NxkAvEncoderAddress(VOID)
{
    UCHAR probe;

    if (NT_SUCCESS(HalpXboxSmBusReadByte(SMBUS_ENCODER_CONEXANT, 0x00, &probe)))
        return SMBUS_ENCODER_CONEXANT;
    if (NT_SUCCESS(HalpXboxSmBusReadByte(SMBUS_ENCODER_FOCUS, 0x00, &probe)))
        return SMBUS_ENCODER_FOCUS;
    return 0;
}

/*
 * Compose GET_SETTINGS from the SMC's AV-pack register and the EEPROM's
 * video-standard byte.  Defaults (composite + NTSC-M) keep titles on a valid
 * 640x480 mode when the SMBus is unreachable.
 */
static ULONG
NxkAvGetEncoderSettings(VOID)
{
    UCHAR avpack = 6;       /* default: composite                   */
    UCHAR standard = 0x40;  /* default: NTSC                        */
    ULONG adapter;

    HalpXboxSmBusReadByte(SMBUS_SMC, SMC_REG_AVPACK, &avpack);
    HalpXboxSmBusReadByte(SMBUS_EEPROM, EEPROM_VIDEO_STANDARD, &standard);

    /* SMC AV-pack code -> nxdk AV_PACK_*. */
    switch (avpack)
    {
        case 0:  adapter = AV_PACK_SCART;  break;  /* SCART RGB         */
        case 1:  adapter = AV_PACK_HDTV;   break;  /* HDTV component    */
        case 2:  adapter = AV_PACK_VGA;    break;  /* VGA, sync-on-green */
        case 4:  adapter = AV_PACK_SVIDEO; break;  /* S-Video           */
        case 7:  adapter = AV_PACK_VGA;    break;  /* VGA               */
        case 6:                                    /* composite         */
        default: adapter = AV_PACK_STANDARD; break;
    }

    /* EEPROM video standard: 0x40 == NTSC (NTSC-J is not distinguished from
     * NTSC-M here -- a title's mode list has both).  Composed result:
     *   low byte  : AV_PACK_* (encoder/cable detected)
     *   byte 1    : AV_REGION_*
     *   high word : capability flags the title checks against its mode list
     *
     * The capability flags advertise refresh rate (60/50 Hz) and the HD
     * scan formats present on the HDTV pack; without them a title sees
     * its 640x480 NTSC mode entries as not-presentable and aborts
     * D3D init. */
    {
        ULONG flags = (standard == 0x40) ? AV_FLAGS_60Hz : AV_FLAGS_50Hz;
        flags |= AV_FLAGS_NORMAL;       /* 4:3 always available           */
        if (adapter == AV_PACK_HDTV)
            flags |= AV_FLAGS_HDTV_480p | AV_FLAGS_HDTV_720p | AV_FLAGS_HDTV_1080i;
        return adapter | (standard == 0x40 ? AV_REGION_NTSCM : AV_REGION_PAL) | flags;
    }
}

VOID NTAPI
AvSendTVEncoderOption(
    _In_ PVOID RegisterBase,
    _In_ ULONG Option,
    _In_ ULONG Param,
    _Out_opt_ PULONG Result)
{
    UCHAR encoder;

    UNREFERENCED_PARAMETER(RegisterBase);   /* the encoder is on the SMBus */

    XbDbg("AvSendTVEncoderOption: opt=%lu param=%08lx result=%p\n",
          Option, Param, Result);

    switch (Option)
    {
        case AV_OPTION_GET_SETTINGS:
            if (Result != NULL)
                *Result = NxkAvGetEncoderSettings();
            XbDbg("AvSendTVEncoderOption(GET_SETTINGS) -> %08lx\n",
                  Result ? *Result : 0);
            break;

        case AV_OPTION_VIDEO_ENABLE:
            /* nxdk's XVideoSetVideoEnable passes Param = !enable: Param 0
             * turns scanout on, non-zero off. */
            encoder = NxkAvEncoderAddress();
            if (Param == 0)
            {
                WRITE_PORT_UCHAR((PUCHAR)ACPI_VIDEO_GATE, ACPI_VIDEO_ON);
                if (encoder == SMBUS_ENCODER_CONEXANT)
                {
                    /* Drive the Conexant luma/chroma output DACs. */
                    HalpXboxSmBusWriteByte(encoder, 0xA8, 0x81);
                    HalpXboxSmBusWriteByte(encoder, 0xAA, 0x49);
                    HalpXboxSmBusWriteByte(encoder, 0xAC, 0x8C);
                }
            }
            else
            {
                if (encoder == SMBUS_ENCODER_CONEXANT)
                {
                    /* Dim the Conexant DACs before gating the output. */
                    HalpXboxSmBusWriteByte(encoder, 0xA8, 0x00);
                    HalpXboxSmBusWriteByte(encoder, 0xAA, 0x00);
                    HalpXboxSmBusWriteByte(encoder, 0xAC, 0x00);
                }
                WRITE_PORT_UCHAR((PUCHAR)ACPI_VIDEO_GATE, ACPI_VIDEO_OFF);
            }
            break;

        case AV_OPTION_FLICKER_FILTER:
        case AV_OPTION_SOFTEN_FILTER:
            /* Picture-quality encoder tweaks -- not on the scanout-enable
             * path; the exact Conexant register encoding is still TODO. */
            break;

        default:
            XbDbg("AvSendTVEncoderOption: option %lu param=%lu ignored\n",
                  Option, Param);
            break;
    }
}

/*
 * Program the NV2A CRTC for a display mode.  Title drives this in a
 * do/while loop -- the function returns the next Step to run, 0 when done.
 * Retail staggers PLL relock and encoder handoff across steps; on the
 * emulated NV2A that collapses to a single step: point PCRTC_START at the
 * freshly-allocated framebuffer.  pbkit re-points PCRTC_START on every
 * buffer flip; AvSendTVEncoderOption owns the encoder.  RegisterBase is
 * nxdk's VIDEO_BASE (0xFD000000), reached through the MMIO identity window.
 */
ULONG NTAPI
AvSetDisplayMode(
    _In_ PVOID RegisterBase,
    _In_ ULONG Step,
    _In_ ULONG DisplayMode,
    _In_ ULONG SourceColorFormat,
    _In_ ULONG Pitch,
    _In_ ULONG FrameBuffer)
{
    /* xemu's display fragment shader uses
     *   line_offset = (CR[0x13] | ((CR[0x19]&0xE0)<<3) | ((CR[0x25]&0x20)<<6)) << 3
     * as the byte pitch.  Build that 12-bit raw value from Pitch/8 and write
     * it back through PRMCIO so the display path samples the right scanline. */
    volatile UCHAR *cr_index =
        (volatile UCHAR *)((ULONG_PTR)RegisterBase + NV2A_PRMCIO_CR_INDEX);
    volatile UCHAR *cr_data =
        (volatile UCHAR *)((ULONG_PTR)RegisterBase + NV2A_PRMCIO_CR_DATA);
    ULONG raw = Pitch >> 3;     /* xemu shifts by 3 */
    UCHAR cr19, cr25;

    UNREFERENCED_PARAMETER(Step);
    UNREFERENCED_PARAMETER(DisplayMode);
    UNREFERENCED_PARAMETER(SourceColorFormat);

    /* PCRTC_START scans out physical RAM; FrameBuffer arrives pre-masked to
     * 0x7FFFFFFF by nxdk -- reduce it to the 64 MB physical address space. */
    *(volatile ULONG *)((ULONG_PTR)RegisterBase + NV2A_PCRTC_START) =
        FrameBuffer & 0x03FFFFFF;

    /* CR[0x13] holds raw bits 7:0. */
    *cr_index = 0x13;
    *cr_data  = (UCHAR)(raw & 0xFF);

    /* CR[0x19] bits 7:5 hold raw bits 10:8; preserve the other bits. */
    *cr_index = 0x19;
    cr19 = (*cr_data & ~0xE0) | (UCHAR)(((raw >> 8) & 0x07) << 5);
    *cr_data = cr19;

    /* CR[0x25] bit 5 holds raw bit 11; preserve the other bits.  640x480x32
     * needs raw=0x140 so bit 11 is 0 -- still write to defang any cromwell
     * leftover that happens to have it set. */
    *cr_index = 0x25;
    cr25 = (*cr_data & ~0x20) | (UCHAR)(((raw >> 11) & 0x01) << 5);
    *cr_data = cr25;

    /* CR[0x39] = 0xFF disables interlace mode (xemu treats any other value
     * as interlaced and doubles the display height, producing two stacked
     * copies of the framebuffer on screen).  Progressive 480p / 480i without
     * interlace from the GPU's side is what nxdk's triangle expects. */
    *cr_index = 0x39;
    *cr_data  = 0xFF;

    return 0;   /* single step -- done */
}
