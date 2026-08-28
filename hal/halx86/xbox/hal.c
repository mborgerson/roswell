/*
 * PROJECT:     nxkrnl -- a free kernel for the original Xbox
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Hal* kernel ordinals.
 *
 * Clean-room from cxbx-reloaded's EmuKrnlHal.cpp and xemu's SMC device
 * model (hw/xbox/smbus_xbox_smc.c) which gives the authoritative register
 * numbers and value space.
 */

#include <ntdef.h>
#include <ntifs.h>

#include "halxbox.h"

/* SMBus slave addresses (cromwell drivers/video, drivers/pci). */
#define SMBUS_SMC                   0x10

/*
 * SMC tray-state register.  Only the high nibble is meaningful; 0x70 is
 * the mask retail uses before comparing against TRAY_CLOSED_*.
 */
#define SMC_REG_TRAYSTATE           0x03
#define SMC_TRAYSTATE_MASK          0x70

/*
 * Retail collapses every transient tray state (ejecting, loading, button
 * held) to TRAY_OPEN; only the two "closed" states are reported as-is.
 */
#define TRAY_CLOSED_MEDIA_PRESENT   0x60
#define TRAY_CLOSED_NO_MEDIA        0x40
#define TRAY_OPEN                   0x10

/*
 * Incremented by the SMC ISR each time the tray opens (PIC IRQ 12,
 * SMC code 5).  No SMC IRQ wired up yet, so this stays at 0.
 */
static volatile ULONG NxkHalTrayCount = 0;

/*
 * On SMBus error leave *State as the "treat as open" fallback -- retail
 * prefers that over a spurious "media present".  NULL Count is tolerated.
 */
NTSTATUS NTAPI
HalReadSMCTrayState(_Out_ PULONG State, _Out_opt_ PULONG Count)
{
    UCHAR    raw = 0;
    NTSTATUS status;

    status = HalpXboxSmBusReadByte(SMBUS_SMC, SMC_REG_TRAYSTATE, &raw);
    if (NT_SUCCESS(status))
    {
        ULONG masked = raw & SMC_TRAYSTATE_MASK;
        if (masked == TRAY_CLOSED_MEDIA_PRESENT ||
            masked == TRAY_CLOSED_NO_MEDIA)
        {
            *State = masked;
        }
        else
        {
            *State = TRAY_OPEN;
        }
    }
    else
    {
        *State = TRAY_OPEN;
    }

    if (Count != NULL)
        *Count = NxkHalTrayCount;

    return status;
}

/*
 * A title that means to handle the eject button itself asks for the
 * secure form first; the console then leaves the tray to the title
 * instead of rebooting to the dashboard behind its back.  There is no
 * eject-driven reboot here to switch off -- the SMC interrupt is not
 * wired up, which is why NxkHalTrayCount never moves -- so the request
 * is recorded for the ISR that will consult it, and the ordinal is
 * safe to call in the meantime.
 */
static volatile BOOLEAN NxkHalSecureTrayEject = FALSE;

VOID NTAPI HalEnableSecureTrayEject(VOID) { NxkHalSecureTrayEject = TRUE; }

/* Titles poll HalIsResetOrShutdownPending to skip background work after a
 * user-initiated shutdown was requested via HalInitiateShutdown.  The actual
 * power-off comes later through HalReturnToFirmware. */
static BOOLEAN HalpShutdownPending = FALSE;

BOOLEAN NTAPI HalIsResetOrShutdownPending(VOID) { return HalpShutdownPending; }

VOID NTAPI HalInitiateShutdown(VOID) { HalpShutdownPending = TRUE; }

/* --- DATA exports ------------------------------------------------------- *
 * Ordinals that resolve to the *address* of a datum -- title reads through
 * the thunk slot.  The default `ULONG_PTR = 0` scaffold is 4 bytes and
 * understates any struct-shaped DATA; sized + initialised here for real.
 */

/* XBOX_HARDWARE_INFO {Flags, GpuRev, McpRev, [2]reserved}.
 *
 * XBOX_HW_FLAG_INTERNAL_USB_HUB(0x01) tells the title that USB0 port 1
 * fronts the daughterboard hub the controller ports hang off -- the
 * topology xemu models.  Without the bit, XAPI USBD takes a different
 * enumeration code path.  GPU rev A1 / MCP rev B1 match the host PCI REV.
 */
typedef struct _XBOX_HARDWARE_INFO_LAYOUT
{
    ULONG Flags;
    UCHAR GpuRevision;
    UCHAR McpRevision;
    UCHAR Reserved[2];
} XBOX_HARDWARE_INFO_LAYOUT;

XBOX_HARDWARE_INFO_LAYOUT XboxHardwareInfo = {
    /* .Flags        = */ 0x00000001UL,
    /* .GpuRevision  = */ 0xA1,
    /* .McpRevision  = */ 0xB1,
    /* .Reserved     = */ {0, 0}
};

/* AV-pack the SMC observed at boot.  1 = AV_PACK_HDTV. */
ULONG HalBootSMCVideoMode = 1;

/* 8-byte XBOX_KRNL_VERSION {Major, Minor, Build, Qfe}.  The default
 * 4-byte scaffold pegs Major to 0, which trips title compat probes
 * that gate on the kernel revision. 1.0.5530.0 matches the public
 * retail kernel that shipped with the Xbox dashboard. */
typedef struct _XBOX_KRNL_VERSION_LAYOUT
{
    USHORT Major;
    USHORT Minor;
    USHORT Build;
    USHORT Qfe;
} XBOX_KRNL_VERSION_LAYOUT;

XBOX_KRNL_VERSION_LAYOUT XboxKrnlVersion = {
    /* .Major = */ 1,
    /* .Minor = */ 0,
    /* .Build = */ 5530,
    /* .Qfe   = */ 0
};

/* `extern STRING` (8 B: USHORT Length, MaxLen, PCHAR Buffer); the default
 * 4-byte scaffold would leak adjacent .data garbage on a Buffer read. */
static CHAR NxkHalDiskModelBuffer[] = "QEMU HARDDISK";
ANSI_STRING HalDiskModelNumber = {
    sizeof(NxkHalDiskModelBuffer) - 1,
    sizeof(NxkHalDiskModelBuffer),
    NxkHalDiskModelBuffer
};
static CHAR NxkHalDiskSerialBuffer[] = "0123456789";
ANSI_STRING HalDiskSerialNumber = {
    sizeof(NxkHalDiskSerialBuffer) - 1,
    sizeof(NxkHalDiskSerialBuffer),
    NxkHalDiskSerialBuffer
};
