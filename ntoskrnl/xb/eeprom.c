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
 * The first 48 bytes are sealed rather than plain, so the game region
 * that lives in them is opened here before the table can read it.
 */

#include <ntdef.h>
#include <ntifs.h>

#include <rc4.h>

/* SMBus primitives (hal/halx86/xbox/smbus.c). */
NTSTATUS HalpXboxSmBusReadByte(_In_ UCHAR Address, _In_ UCHAR Register,
                               _Out_ PUCHAR Value);
NTSTATUS HalpXboxSmBusWriteByte(_In_ UCHAR Address, _In_ UCHAR Register,
                                _In_ UCHAR Value);

/* The console's doubled SHA-1 with settable start states (xb/crypto.c). */
VOID NxkShaKeyedDouble(_In_reads_(5) const ULONG *First,
                       _In_reads_(5) const ULONG *Second,
                       _In_reads_bytes_(Length) const VOID *Data,
                       _In_ ULONG Length,
                       _Out_writes_(20) PUCHAR Digest);

#define EEPROM_SLAVE_ADDRESS  0x54      /* 0xA8 as the console writes it */
#define EEPROM_SIZE           256

#define NVS_TYPE_BINARY       3         /* REG_BINARY */
#define NVS_TYPE_DWORD        4         /* REG_DWORD */

#define EEPROM_SEALED_OFFSET  0x14      /* first sealed byte */
#define EEPROM_SEALED_LENGTH  0x1C      /* confounder, HDD key, region */

/*
 * A setting is either a run of the part as it is stored or a run of the
 * sealed section once opened; `Sealed` says which, and its offset is
 * then relative to the first sealed byte.
 */
static const struct
{
    USHORT  Index;
    UCHAR   Offset;
    UCHAR   Length;
    UCHAR   Type;
    BOOLEAN Sealed;
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
    /* Sealed section, offsets from its first byte. */
    { 0x0104, 0x18,  4, NVS_TYPE_DWORD, TRUE },   /* game region */
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

/*
 * Opening the sealed section.
 *
 * The 20-byte hash at the front is both the integrity check over the
 * section and, hashed a second time, the RC4 key that covers it.  RC4
 * is its own inverse, so sealing run backwards opens it:
 *
 *     key   = XboxSha1(stored hash, 20)
 *     plain = RC4(key) over the 0x1C bytes from 0x14
 *     check: XboxSha1(plain, 0x1C) == stored hash
 *
 * The start states are version-specific and nothing in the part records
 * which version sealed it, so all four are tried and the one whose
 * recomputed hash matches is the right one.  These are published
 * reverse-engineering constants, the same class as the SMC register
 * numbers the HAL already carries.
 */
static const struct
{
    ULONG First[5];
    ULONG Second[5];
} NxkEepromKeys[] = {
    /* debug */
    { { 0x85F9E51A, 0xE04613D2, 0x6D86A50C, 0x77C32E3C, 0x4BD717A4 },
      { 0x5D7A9C6B, 0xE1922BEB, 0xB82CCDBC, 0x3137AB34, 0x486B52B3 } },
    /* retail 1.0 */
    { { 0x72127625, 0x336472B9, 0xBE609BEA, 0xF55E226B, 0x99958DAC },
      { 0x76441D41, 0x4DE82659, 0x2E8EF85E, 0xB256FACA, 0xC4FE2DE8 } },
    /* retail 1.1 - 1.4 */
    { { 0x39B06E79, 0xC9BD25E8, 0xDBC6B498, 0x40B4389D, 0x86BBD7ED },
      { 0x9B49BED3, 0x84B430FC, 0x6B8749CD, 0xEBFE5FE5, 0xD96E7393 } },
    /* retail 1.6 */
    { { 0x8058763A, 0xF97D4E0E, 0x865A9762, 0x8A3D920D, 0x08995B2C },
      { 0x01075307, 0xA2F1E037, 0x1186EEEA, 0x88DA9992, 0x168A5609 } },
};

static UCHAR   NxkEepromPlain[EEPROM_SEALED_LENGTH];
static BOOLEAN NxkEepromOpened = FALSE;

/*
 * The console's hard-disk key, a DATA export.  A title reads the datum
 * itself rather than calling for it, so it has to hold the key before
 * any title runs -- see NxkInitializeEeprom below.
 */
UCHAR XboxHDKey[16];

static BOOLEAN
NxkEepromOpen(VOID)
{
    UCHAR Key[20], Check[20];
    RC4_CONTEXT Rc4;
    ULONG i;

    if (NxkEepromOpened)
        return TRUE;
    if (!NxkEepromLoad())
        return FALSE;

    for (i = 0; i < RTL_NUMBER_OF(NxkEepromKeys); i++)
    {
        NxkShaKeyedDouble(NxkEepromKeys[i].First, NxkEepromKeys[i].Second,
                          NxkEepromImage, 20, Key);

        RtlCopyMemory(NxkEepromPlain, NxkEepromImage + EEPROM_SEALED_OFFSET,
                      EEPROM_SEALED_LENGTH);
        rc4_init(&Rc4, Key, sizeof(Key));
        rc4_crypt(&Rc4, NxkEepromPlain, EEPROM_SEALED_LENGTH);

        NxkShaKeyedDouble(NxkEepromKeys[i].First, NxkEepromKeys[i].Second,
                          NxkEepromPlain, EEPROM_SEALED_LENGTH, Check);
        if (RtlCompareMemory(Check, NxkEepromImage, sizeof(Check)) ==
            sizeof(Check))
        {
            /* Section layout: confounder[8], HDD key[16], region. */
            RtlCopyMemory(XboxHDKey, NxkEepromPlain + 8, sizeof(XboxHDKey));
            NxkEepromOpened = TRUE;
            return TRUE;
        }
    }

    /* No version's hash matched: the section is not one we can read, so
     * leave nothing of a wrong guess behind. */
    RtlZeroMemory(NxkEepromPlain, sizeof(NxkEepromPlain));
    return FALSE;
}

/*
 * Each section carries a running 32-bit sum of the words behind it, the
 * carry out folded back in and the result complemented.  A part whose
 * sums do not match what follows them is one the dashboard rejects, so
 * a write has to leave them right.
 */
static ULONG
NxkEepromChecksum(const UCHAR *Data, ULONG Length)
{
    ULONGLONG Sum = 0;
    ULONG i;

    for (i = 0; i < Length / sizeof(ULONG); i++)
    {
        ULONG Word;
        RtlCopyMemory(&Word, Data + i * sizeof(ULONG), sizeof(Word));
        Sum += Word;
    }

    return ~(ULONG)((Sum >> 32) + (ULONG)Sum);
}

#define EEPROM_USER_CHECKSUM  0x60
#define EEPROM_USER_COVERED   0x5C      /* 0x64 .. 0xBF */

/* Push a run of the kept image out to the part. */
static NTSTATUS
NxkEepromStore(ULONG Offset, ULONG Length)
{
    ULONG i;

    for (i = 0; i < Length; i++)
    {
        NTSTATUS Status =
            HalpXboxSmBusWriteByte(EEPROM_SLAVE_ADDRESS, (UCHAR)(Offset + i),
                                   NxkEepromImage[Offset + i]);
        if (!NT_SUCCESS(Status))
            return Status;
    }
    return STATUS_SUCCESS;
}

/*
 * Writing a setting.  Only the user section takes one: a factory index
 * is refused as a name, the same as an index that is not a setting at
 * all, and the sealed section has no index on this side.
 *
 * A value shorter than the setting is allowed and clears the rest of it
 * -- the whole setting is zeroed before the caller's bytes go in.  A
 * value longer than the setting is refused.  The declared type is not
 * kept: a later query still reports the setting's own.
 */
NTSTATUS NTAPI
ExSaveNonVolatileSetting(ULONG ValueIndex, ULONG Type, PVOID Value,
                         ULONG ValueLength)
{
    ULONG Offset, Length, i;

    UNREFERENCED_PARAMETER(Type);

    if (!NxkEepromLoad())
        return STATUS_DEVICE_NOT_READY;

    for (i = 0; i < RTL_NUMBER_OF(NxkEepromSettings); i++)
    {
        if (NxkEepromSettings[i].Index != ValueIndex)
            continue;
        /* The factory settings and the sealed one are read-only. */
        if (NxkEepromSettings[i].Sealed || NxkEepromSettings[i].Offset < 0x60)
            return STATUS_OBJECT_NAME_NOT_FOUND;

        Offset = NxkEepromSettings[i].Offset;
        Length = NxkEepromSettings[i].Length;
        goto found;
    }

    /* Only the user section is writable as a whole; the rest are
     * recognised and refused rather than treated as absent. */
    for (i = 0; i < RTL_NUMBER_OF(NxkEepromSections); i++)
    {
        if (NxkEepromSections[i].Index != ValueIndex)
            continue;
        if (NxkEepromSections[i].Index != 0x00FF)
            return STATUS_INVALID_PARAMETER;

        Offset = NxkEepromSections[i].Offset;
        Length = NxkEepromSections[i].Length;
        goto found;
    }

    return STATUS_OBJECT_NAME_NOT_FOUND;

found:
    if (Value == NULL || ValueLength > Length)
        return STATUS_INVALID_PARAMETER;

    RtlZeroMemory(NxkEepromImage + Offset, Length);
    RtlCopyMemory(NxkEepromImage + Offset, Value, ValueLength);

    /* The sum covers what follows it, so it is recomputed after the
     * setting lands and before either reaches the part. */
    {
        ULONG Sum = NxkEepromChecksum(
            NxkEepromImage + EEPROM_USER_CHECKSUM + sizeof(ULONG),
            EEPROM_USER_COVERED);
        RtlCopyMemory(NxkEepromImage + EEPROM_USER_CHECKSUM, &Sum,
                      sizeof(Sum));
    }

    {
        NTSTATUS Status = NxkEepromStore(EEPROM_USER_CHECKSUM, sizeof(ULONG));
        if (NT_SUCCESS(Status))
            Status = NxkEepromStore(Offset, Length);
        return Status;
    }
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

        if (NxkEepromSettings[i].Sealed && !NxkEepromOpen())
            return STATUS_DEVICE_NOT_READY;

        /* What is handed back is the whole buffer, not just the
         * setting: the rest of it comes back zeroed.  A setting read out
         * of the sealed section is the exception -- it fills only its
         * own bytes and leaves the rest of the buffer alone. */
        if (!NxkEepromSettings[i].Sealed)
            RtlZeroMemory(Value, ValueLength);
        RtlCopyMemory(Value,
                      (NxkEepromSettings[i].Sealed ? NxkEepromPlain
                                                   : NxkEepromImage) +
                          NxkEepromSettings[i].Offset,
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

/*
 * Opened at boot rather than on the first query: the HDD key is read as
 * a datum and there is no call on that path to open it lazily.  A part
 * that will not open leaves the key zeroed and every stored setting
 * still answering.
 */
VOID
NxkInitializeEeprom(VOID)
{
    (VOID)NxkEepromOpen();
}

/* EOF */
