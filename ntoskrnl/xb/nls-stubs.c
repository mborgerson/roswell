/*
 * Xbox NLS replacement: ASCII fast-path for codepage routines + real
 * Unicode case-table lookup.
 *
 * Filesystem paths on Xbox flow through ANSI_STRING (byte buffers) so the
 * kernel never converts title-supplied path bytes through these routines.
 * The retail kernel ships one MultiByte codepage per regional build
 * (CP-1252 for NTSC-U/PAL, CP-932 for J).  With both 66 KB codepage tables
 * dropped, we treat the 8-bit side as Latin-1: bytes zero-extend on
 * Mb->Uni and codepoints <= 0xFF truncate back on Uni->Mb, so every
 * title-supplied byte round-trips losslessly (FATX stores the title's
 * raw bytes on retail; '?' substitution would collapse distinct names
 * with bytes >= 0x80 into colliding '?'-runs).  Only codepoints above
 * 0xFF -- unreachable from any title-supplied name -- fold to '?'.
 *
 * The 4.8 KB Unicode case table (l_intl.nls -> NxkrnlNlsCase) stays embedded
 * and drives RtlpUpcaseUnicodeChar / RtlpDowncaseUnicodeChar.  Object
 * namespace, FATX case-insensitive lookup, FsRtl wildcard, module-name
 * compare all depend on it.
 *
 * Symbols match sdk/lib/rtl/nls.c so it can be REMOVE_ITEM'd from the rtl
 * build on SARCH=xbox.  Wrapper routines in sdk/lib/rtl/unicode.c
 * (RtlUnicodeStringToOemString etc.) route through the underlying routines
 * here and need no edit.
 */

#include <ntoskrnl.h>
#define NDEBUG
#include <debug.h>

extern const unsigned char NxkrnlNlsCase[];

/* Globals exported by vendor nls.c.  Definitions here so rtl/unicode.c and
 * rtl/dos8dot3.c continue to link.  All zero/NULL: every kept caller in
 * unicode.c gates real-table access behind NlsMb*CodePageTag, takes the
 * simple branch, and the work falls into our routines below. */
PUSHORT NlsUnicodeUpcaseTable = NULL;
PUSHORT NlsUnicodeLowercaseTable = NULL;

USHORT NlsAnsiCodePage = 1252;
BOOLEAN NlsMbCodePageTag = FALSE;
PUSHORT NlsAnsiToUnicodeTable = NULL;
PCHAR NlsUnicodeToAnsiTable = NULL;
PUSHORT NlsUnicodeToMbAnsiTable = NULL;
PUSHORT NlsLeadByteInfo = NULL;

USHORT NlsOemCodePage = 437;
BOOLEAN NlsMbOemCodePageTag = FALSE;
PUSHORT NlsOemToUnicodeTable = NULL;
PCHAR NlsUnicodeToOemTable = NULL;
PUSHORT NlsUnicodeToMbOemTable = NULL;
PUSHORT NlsOemLeadByteInfo = NULL;

USHORT NlsOemDefaultChar = '?';
USHORT NlsUnicodeDefaultChar = L'?';

/* --- Case-table walk -------------------------------------------------------
 *
 * l_intl.nls layout (matches RtlInitNlsTables in vendor nls.c):
 *   base[0] = case-pair header size (unused here)
 *   base[1] = upcase table size (in USHORTs)
 *   base + 2                   = upcase trie root
 *   base + 2 + base[1]         = downcase trie root
 */

static PUSHORT NxkCaseUpper;
static PUSHORT NxkCaseLower;

static VOID NxkCaseInit(VOID)
{
    PUSHORT base = (PUSHORT)NxkrnlNlsCase;
    NxkCaseUpper = base + 2;
    NxkCaseLower = base + 2 + base[1];
}

WCHAR NTAPI
RtlpUpcaseUnicodeChar(IN WCHAR Source)
{
    USHORT Offset;

    if (Source < L'a') return Source;
    if (Source <= L'z') return Source - (L'a' - L'A');

    if (!NxkCaseUpper) NxkCaseInit();

    Offset = ((USHORT)Source >> 8) & 0xFF;
    Offset = NxkCaseUpper[Offset];
    Offset += ((USHORT)Source >> 4) & 0xF;
    Offset = NxkCaseUpper[Offset];
    Offset += ((USHORT)Source & 0xF);
    Offset = NxkCaseUpper[Offset];

    return Source + (SHORT)Offset;
}

WCHAR NTAPI
RtlUpcaseUnicodeChar(IN WCHAR Source)
{
    return RtlpUpcaseUnicodeChar(Source);
}

WCHAR NTAPI
RtlpDowncaseUnicodeChar(IN WCHAR Source)
{
    USHORT Offset;

    if (Source < L'A') return Source;
    if (Source <= L'Z') return Source + (L'a' - L'A');
    if (Source < 0x80) return Source;

    if (!NxkCaseLower) NxkCaseInit();

    Offset = ((USHORT)Source >> 8) & 0xFF;
    Offset = NxkCaseLower[Offset];
    Offset += ((USHORT)Source >> 4) & 0xF;
    Offset = NxkCaseLower[Offset];
    Offset += ((USHORT)Source & 0xF);
    Offset = NxkCaseLower[Offset];

    return Source + (SHORT)Offset;
}

WCHAR NTAPI
RtlDowncaseUnicodeChar(IN WCHAR Source)
{
    return RtlpDowncaseUnicodeChar(Source);
}

CHAR NTAPI
RtlUpperChar(IN CHAR Source)
{
    if (Source >= 'a' && Source <= 'z')
        return Source - ('a' - 'A');
    return Source;
}

/* --- Codepage conversion (ASCII fast-path) -------------------------------- */

NTSTATUS NTAPI
RtlMultiByteToUnicodeN(OUT PWCHAR UnicodeString,
                       IN ULONG UnicodeSize,
                       OUT PULONG ResultSize OPTIONAL,
                       IN PCSTR MbString,
                       IN ULONG MbSize)
{
    ULONG cap = UnicodeSize / sizeof(WCHAR);
    ULONG n = (MbSize > cap) ? cap : MbSize;
    ULONG i;

    for (i = 0; i < n; i++)
        UnicodeString[i] = (UCHAR)MbString[i];

    if (ResultSize) *ResultSize = n * sizeof(WCHAR);
    return STATUS_SUCCESS;
}

NTSTATUS NTAPI
RtlMultiByteToUnicodeSize(OUT PULONG UnicodeSize,
                          IN PCSTR MbString,
                          IN ULONG MbSize)
{
    (void)MbString;
    *UnicodeSize = MbSize * sizeof(WCHAR);
    return STATUS_SUCCESS;
}

NTSTATUS NTAPI
RtlUnicodeToMultiByteN(OUT PCHAR MbString,
                       IN ULONG MbSize,
                       OUT PULONG ResultSize OPTIONAL,
                       IN PCWCH UnicodeString,
                       IN ULONG UnicodeSize)
{
    ULONG src = UnicodeSize / sizeof(WCHAR);
    ULONG n = (src > MbSize) ? MbSize : src;
    ULONG i;

    for (i = 0; i < n; i++)
    {
        WCHAR w = UnicodeString[i];
        MbString[i] = (w < 0x100) ? (CHAR)w : '?';
    }

    if (ResultSize) *ResultSize = n;
    return STATUS_SUCCESS;
}

NTSTATUS NTAPI
RtlUnicodeToMultiByteSize(OUT PULONG MbSize,
                          IN PCWCH UnicodeString,
                          IN ULONG UnicodeSize)
{
    (void)UnicodeString;
    *MbSize = UnicodeSize / sizeof(WCHAR);
    return STATUS_SUCCESS;
}

NTSTATUS NTAPI
RtlOemToUnicodeN(OUT PWCHAR UnicodeString,
                 IN ULONG UnicodeSize,
                 OUT PULONG ResultSize OPTIONAL,
                 IN PCCH OemString,
                 IN ULONG OemSize)
{
    return RtlMultiByteToUnicodeN(UnicodeString, UnicodeSize, ResultSize,
                                  OemString, OemSize);
}

NTSTATUS NTAPI
RtlUnicodeToOemN(OUT PCHAR OemString,
                 IN ULONG OemSize,
                 OUT PULONG ResultSize OPTIONAL,
                 IN PCWCH UnicodeString,
                 IN ULONG UnicodeSize)
{
    return RtlUnicodeToMultiByteN(OemString, OemSize, ResultSize,
                                  UnicodeString, UnicodeSize);
}

NTSTATUS NTAPI
RtlUpcaseUnicodeToMultiByteN(OUT PCHAR MbString,
                             IN ULONG MbSize,
                             OUT PULONG ResultSize OPTIONAL,
                             IN PCWCH UnicodeString,
                             IN ULONG UnicodeSize)
{
    ULONG src = UnicodeSize / sizeof(WCHAR);
    ULONG n = (src > MbSize) ? MbSize : src;
    ULONG i;

    for (i = 0; i < n; i++)
    {
        WCHAR w = RtlpUpcaseUnicodeChar(UnicodeString[i]);
        MbString[i] = (w < 0x100) ? (CHAR)w : '?';
    }

    if (ResultSize) *ResultSize = n;
    return STATUS_SUCCESS;
}

NTSTATUS NTAPI
RtlUpcaseUnicodeToOemN(OUT PCHAR OemString,
                       IN ULONG OemSize,
                       OUT PULONG ResultSize OPTIONAL,
                       IN PCWCH UnicodeString,
                       IN ULONG UnicodeSize)
{
    return RtlUpcaseUnicodeToMultiByteN(OemString, OemSize, ResultSize,
                                        UnicodeString, UnicodeSize);
}

/* --- Init plumbing -------------------------------------------------------- */

VOID NTAPI
RtlInitCodePageTable(IN PUSHORT TableBase,
                     OUT PCPTABLEINFO CodePageTable)
{
    (void)TableBase;
    (void)CodePageTable;
}

VOID NTAPI
RtlInitNlsTables(IN PUSHORT AnsiTableBase,
                 IN PUSHORT OemTableBase,
                 IN PUSHORT CaseTableBase,
                 OUT PNLSTABLEINFO NlsTable)
{
    (void)AnsiTableBase;
    (void)OemTableBase;
    (void)CaseTableBase;
    (void)NlsTable;
    NxkCaseInit();
}

VOID NTAPI
RtlResetRtlTranslations(IN PNLSTABLEINFO NlsTable)
{
    (void)NlsTable;
}

VOID NTAPI
RtlGetDefaultCodePage(OUT PUSHORT AnsiCodePage,
                      OUT PUSHORT OemCodePage)
{
    *AnsiCodePage = NlsAnsiCodePage;
    *OemCodePage = NlsOemCodePage;
}

/* --- CustomCP / Console variants ------------------------------------------ */

NTSTATUS NTAPI
RtlCustomCPToUnicodeN(IN PCPTABLEINFO CustomCP,
                      OUT PWCHAR UnicodeString,
                      IN ULONG UnicodeSize,
                      OUT PULONG ResultSize OPTIONAL,
                      IN PCHAR MbString,
                      IN ULONG MbSize)
{
    (void)CustomCP;
    return RtlMultiByteToUnicodeN(UnicodeString, UnicodeSize, ResultSize,
                                  MbString, MbSize);
}

NTSTATUS NTAPI
RtlUnicodeToCustomCPN(IN PCPTABLEINFO CustomCP,
                      OUT PCHAR MbString,
                      IN ULONG MbSize,
                      OUT PULONG ResultSize OPTIONAL,
                      IN PWCHAR UnicodeString,
                      IN ULONG UnicodeSize)
{
    (void)CustomCP;
    return RtlUnicodeToMultiByteN(MbString, MbSize, ResultSize,
                                  UnicodeString, UnicodeSize);
}

NTSTATUS NTAPI
RtlUpcaseUnicodeToCustomCPN(IN PCPTABLEINFO CustomCP,
                            OUT PCHAR MbString,
                            IN ULONG MbSize,
                            OUT PULONG ResultSize OPTIONAL,
                            IN PWCHAR UnicodeString,
                            IN ULONG UnicodeSize)
{
    (void)CustomCP;
    return RtlUpcaseUnicodeToMultiByteN(MbString, MbSize, ResultSize,
                                        UnicodeString, UnicodeSize);
}

NTSTATUS NTAPI
RtlConsoleMultiByteToUnicodeN(OUT PWCHAR UnicodeString,
                              IN ULONG UnicodeSize,
                              OUT PULONG ResultSize OPTIONAL,
                              IN PCSTR MbString,
                              IN ULONG MbSize,
                              OUT PULONG Unknown)
{
    if (Unknown) *Unknown = 1;
    return RtlMultiByteToUnicodeN(UnicodeString, UnicodeSize, ResultSize,
                                  MbString, MbSize);
}
