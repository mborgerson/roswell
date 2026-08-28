/*
 * PROJECT:     nxkrnl -- a free kernel for the original Xbox
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     The console's non-volatile settings, out of the EEPROM.
 *
 * The EEPROM is a 256-byte serial part on the SMBus.  Its first 48
 * bytes are the encrypted section; the factory section that follows
 * holds the serial number, the MAC and the region, and the user section
 * from 0x60 holds everything a title can change -- clock, language,
 * video and audio flags, parental control, the network addresses.
 *
 * Index-to-offset comes from the retail kernel: every setting was read
 * through the ordinal and matched against the same EEPROM read back
 * over SMBus from a title.  The api-regression coverage does that
 * correlation again on both kernels, so the table is checked rather
 * than trusted.
 *
 * The game region is the one setting missing here.  It answers on the
 * console but matches no plaintext byte of the part, so it comes out of
 * the encrypted section and waits on the key that section is sealed
 * with -- as do the refurbishment record and the per-console key blobs.
 */

#include <ntdef.h>
#include <ntifs.h>

/* SMBus primitives (hal/halx86/xbox/smbus.c). */
NTSTATUS HalpXboxSmBusReadByte(_In_ UCHAR Address, _In_ UCHAR Register,
                               _Out_ PUCHAR Value);

#define EEPROM_SLAVE_ADDRESS  0x54      /* 0xA8 as the console writes it */
#define EEPROM_SIZE           256

#define NVS_TYPE_BINARY       3         /* REG_BINARY */
#define NVS_TYPE_DWORD        4         /* REG_DWORD */

static const struct
{
    USHORT Index;
    UCHAR  Offset;
    UCHAR  Length;
    UCHAR  Type;
} NxkEepromSettings[] = {
    /* User section: the clock first, then what the dashboard sets. */
    { 0x0000, 0x64,  4, NVS_TYPE_DWORD  },   /* time-zone bias */
    { 0x0001, 0x68,  4, NVS_TYPE_BINARY },   /* standard-time name */
    { 0x0002, 0x78,  4, NVS_TYPE_BINARY },   /* standard-time date */
    { 0x0003, 0x88,  4, NVS_TYPE_DWORD  },   /* standard-time bias */
    { 0x0004, 0x6C,  4, NVS_TYPE_BINARY },   /* daylight-time name */
    { 0x0005, 0x7C,  4, NVS_TYPE_BINARY },   /* daylight-time date */
    { 0x0006, 0x8C,  4, NVS_TYPE_DWORD  },   /* daylight-time bias */
    { 0x0007, 0x90,  4, NVS_TYPE_DWORD  },   /* language */
    { 0x0008, 0x94,  4, NVS_TYPE_DWORD  },   /* video flags */
    { 0x0009, 0x98,  4, NVS_TYPE_DWORD  },   /* audio flags */
    { 0x000A, 0x9C,  4, NVS_TYPE_DWORD  },   /* parental control: games */
    { 0x000B, 0xA0,  4, NVS_TYPE_DWORD  },   /* parental control: password */
    { 0x000C, 0xA4,  4, NVS_TYPE_DWORD  },   /* parental control: movies */
    { 0x000D, 0xA8,  4, NVS_TYPE_DWORD  },   /* IP address */
    { 0x000E, 0xAC,  4, NVS_TYPE_DWORD  },   /* DNS address */
    { 0x000F, 0xB0,  4, NVS_TYPE_DWORD  },   /* default gateway */
    { 0x0010, 0xB4,  4, NVS_TYPE_DWORD  },   /* subnet mask */
    { 0x0011, 0xB8,  4, NVS_TYPE_DWORD  },   /* misc flags */
    { 0x0012, 0xBC,  4, NVS_TYPE_DWORD  },   /* DVD region */
    /* Factory section: written once, before the console left. */
    { 0x0100, 0x34, 12, NVS_TYPE_BINARY },   /* serial number */
    { 0x0101, 0x40,  6, NVS_TYPE_BINARY },   /* ethernet address */
    { 0x0102, 0x48, 16, NVS_TYPE_BINARY },   /* online key */
    { 0x0103, 0x58,  4, NVS_TYPE_DWORD  },   /* AV region */
};

/*
 * Four indices hand back a run of the image rather than one setting,
 * and they come out of two different paths.  The two section markers
 * behave like a setting: the rest of the caller's buffer is zeroed, and
 * a buffer too small is refused without saying how much to bring.  The
 * two that name the part itself do neither -- they copy exactly the run
 * and report the length they wanted when it does not fit.
 */
static const struct
{
    USHORT  Index;
    UCHAR   Offset;
    USHORT  Length;
    BOOLEAN WholePart;
} NxkEepromSections[] = {
    { 0x00FF, 0x60, 0x60, FALSE },   /* the user section */
    { 0x01FF, 0x30, 0x30, FALSE },   /* the factory section */
    { 0xFFFE, 0x00, 0x30, TRUE  },   /* the encrypted section */
    { 0xFFFF, 0x00, 0x100, TRUE },   /* the whole part */
};

static UCHAR   NxkEepromImage[EEPROM_SIZE];
static BOOLEAN NxkEepromLoaded = FALSE;

/*
 * The part is read once and kept.  Nothing else on this console writes
 * it behind our back, and a title that saves a setting updates the copy
 * along with the part.
 */
static BOOLEAN
NxkEepromLoad(VOID)
{
    ULONG i;

    if (NxkEepromLoaded)
        return TRUE;

    for (i = 0; i < EEPROM_SIZE; i++)
    {
        UCHAR value = 0;
        if (!NT_SUCCESS(HalpXboxSmBusReadByte(EEPROM_SLAVE_ADDRESS,
                                              (UCHAR)i, &value)))
            return FALSE;
        NxkEepromImage[i] = value;
    }

    NxkEepromLoaded = TRUE;
    return TRUE;
}

NTSTATUS NTAPI
ExQueryNonVolatileSetting(ULONG ValueIndex, PULONG Type, PVOID Value,
                          ULONG ValueLength, PULONG ResultLength)
{
    ULONG i;

    if (!NxkEepromLoad())
        return STATUS_DEVICE_NOT_READY;

    for (i = 0; i < RTL_NUMBER_OF(NxkEepromSettings); i++)
    {
        if (NxkEepromSettings[i].Index != ValueIndex)
            continue;

        /* A buffer that cannot take the whole setting is refused, and
         * the caller is told nothing else: neither out-parameter is
         * written, so there is no length to retry with. */
        if (Value == NULL || ValueLength < NxkEepromSettings[i].Length)
            return STATUS_BUFFER_TOO_SMALL;

        /* What is handed back is the whole buffer, not just the
         * setting: the rest of it comes back zeroed. */
        RtlZeroMemory(Value, ValueLength);
        RtlCopyMemory(Value, NxkEepromImage + NxkEepromSettings[i].Offset,
                      NxkEepromSettings[i].Length);
        if (Type != NULL)
            *Type = NxkEepromSettings[i].Type;
        if (ResultLength != NULL)
            *ResultLength = NxkEepromSettings[i].Length;
        return STATUS_SUCCESS;
    }

    for (i = 0; i < RTL_NUMBER_OF(NxkEepromSections); i++)
    {
        if (NxkEepromSections[i].Index != ValueIndex)
            continue;

        if (Value == NULL || ValueLength < NxkEepromSections[i].Length)
        {
            if (NxkEepromSections[i].WholePart && ResultLength != NULL)
                *ResultLength = NxkEepromSections[i].Length;
            return STATUS_BUFFER_TOO_SMALL;
        }

        if (!NxkEepromSections[i].WholePart)
            RtlZeroMemory(Value, ValueLength);
        RtlCopyMemory(Value, NxkEepromImage + NxkEepromSections[i].Offset,
                      NxkEepromSections[i].Length);
        if (Type != NULL)
            *Type = NVS_TYPE_BINARY;
        if (ResultLength != NULL)
            *ResultLength = NxkEepromSections[i].Length;
        return STATUS_SUCCESS;
    }

    return STATUS_OBJECT_NAME_NOT_FOUND;
}

/* EOF */
