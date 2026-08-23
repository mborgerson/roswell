/*
 * PROJECT:     Xbox HAL
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Xbox System Management Bus (SMBus / I2C) access.
 *
 * The MCPX SMBus controller is driven through the I/O-port window at
 * SMB_IO_BASE (defined in halxbox.h).  Port of cromwell's
 * GPL-2.0-or-later drivers/pci/i2cio.c into kernel idioms -- bounded
 * waits, NTSTATUS, and read/write byte/word from a single transfer
 * primitive.  Used by
 * HalpReboot/HalReturnToFirmware and by the Xbox SMC/AV ordinals.
 */

#include "halxbox.h"

#define NDEBUG
#include <debug.h>

/* SMB_GLOBAL_STATUS bits */
#define SMB_ST_BUSY       0x0800   /* another bus master holds the bus      */
#define SMB_ST_SETTLED    0x0036   /* poll mask: completion bit + error bits */
#define SMB_ST_DONE       0x0010   /* the transaction completed successfully */

/* SMB_GLOBAL_ENABLE protocol codes -- read/write, with SMB_PROTO_WORD OR'd
 * in for a 16-bit transfer. */
#define SMB_PROTO_READ    0x0A
#define SMB_PROTO_WRITE   0x1A
#define SMB_PROTO_WORD    0x01

#define SMB_RETRIES       50       /* re-issues for a busy / NAKing slave   */
#define SMB_IDLE_SPINS    0x10000  /* bounded wait for the bus to fall idle */
#define SMB_DONE_SPINS    0x20000  /* bounded wait for one transaction      */

/*
 * Run one SMBus byte/word transaction.  For a read the datum is left in
 * *Data; for a write *Data is sent.  All waits are bounded so a dead slave
 * fails the call rather than hanging the kernel.
 */
static NTSTATUS
HalpXboxSmBusTransfer(_In_ UCHAR Address, _In_ UCHAR Command, _In_ BOOLEAN Read,
                     _In_ BOOLEAN Word, _Inout_ PUSHORT Data)
{
    ULONG spin, try;

    /* Park while another master drives the bus. */
    for (spin = 0; spin < SMB_IDLE_SPINS; spin++)
    {
        if ((READ_PORT_USHORT((PUSHORT)SMB_GLOBAL_STATUS) & SMB_ST_BUSY) == 0)
            break;
    }

    for (try = 0; try < SMB_RETRIES; try++)
    {
        UCHAR proto = (UCHAR)((Read ? SMB_PROTO_READ : SMB_PROTO_WRITE)
                              | (Word ? SMB_PROTO_WORD : 0));
        UCHAR status = 0;

        WRITE_PORT_UCHAR((PUCHAR)SMB_HOST_ADDRESS,
                         (UCHAR)((Address << 1) | (Read ? 1 : 0)));
        WRITE_PORT_UCHAR((PUCHAR)SMB_HOST_COMMAND, Command);
        if (!Read)
            WRITE_PORT_USHORT((PUSHORT)SMB_HOST_DATA,
                              Word ? *Data : (USHORT)(*Data & 0xFF));

        /* Clear status latched by an earlier transaction, then start. */
        WRITE_PORT_USHORT((PUSHORT)SMB_GLOBAL_STATUS,
                          READ_PORT_USHORT((PUSHORT)SMB_GLOBAL_STATUS));
        WRITE_PORT_UCHAR((PUCHAR)SMB_GLOBAL_ENABLE, proto);

        for (spin = 0; spin < SMB_DONE_SPINS; spin++)
        {
            status = READ_PORT_UCHAR((PUCHAR)SMB_GLOBAL_STATUS);
            if (status & SMB_ST_SETTLED)
                break;
        }

        if (status & SMB_ST_DONE)
        {
            if (Read)
                *Data = Word ? READ_PORT_USHORT((PUSHORT)SMB_HOST_DATA)
                             : (USHORT)READ_PORT_UCHAR((PUCHAR)SMB_HOST_DATA);
            return STATUS_SUCCESS;
        }

        KeStallExecutionProcessor(1);
    }

    return STATUS_IO_DEVICE_ERROR;
}

NTSTATUS
HalpXboxSmBusReadByte(_In_ UCHAR Address, _In_ UCHAR Register, _Out_ PUCHAR Value)
{
    USHORT v = 0;
    NTSTATUS status = HalpXboxSmBusTransfer(Address, Register, TRUE, FALSE, &v);
    *Value = (UCHAR)v;
    return status;
}

NTSTATUS
HalpXboxSmBusWriteByte(_In_ UCHAR Address, _In_ UCHAR Register, _In_ UCHAR Value)
{
    USHORT v = Value;
    return HalpXboxSmBusTransfer(Address, Register, FALSE, FALSE, &v);
}

NTSTATUS
HalpXboxSmBusReadWord(_In_ UCHAR Address, _In_ UCHAR Register, _Out_ PUSHORT Value)
{
    *Value = 0;
    return HalpXboxSmBusTransfer(Address, Register, TRUE, TRUE, Value);
}

NTSTATUS
HalpXboxSmBusWriteWord(_In_ UCHAR Address, _In_ UCHAR Register, _In_ USHORT Value)
{
    USHORT v = Value;
    return HalpXboxSmBusTransfer(Address, Register, FALSE, TRUE, &v);
}
