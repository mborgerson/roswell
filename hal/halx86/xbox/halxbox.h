/*
 * PROJECT:         Xbox HAL
 * LICENSE:         GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:         Xbox specific routines
 * COPYRIGHT:       Copyright 2004 Gé van Geldorp (gvg@reactos.com)
 *                  Copyright 2019-2020 Stanislav Motylkov (x86corez@gmail.com)
 *
 * REFERENCES:      https://xboxdevwiki.net/SMBus
 *                  https://github.com/XboxDev/cromwell/blob/master/drivers/pci/i2cio.c
 *                  https://github.com/torvalds/linux/blob/master/drivers/i2c/busses/i2c-amd756.c
 *                  https://github.com/xqemu/xqemu/blob/master/hw/xbox/smbus_xbox_smc.c
 */

#ifndef HALXBOX_H_INCLUDED
#define HALXBOX_H_INCLUDED

#include <hal.h>
#include <ntdddisk.h>

#define TAG_HAL_XBOX 'XlaH'

#define SMB_IO_BASE 0xC000

#define SMB_GLOBAL_STATUS  (0 + SMB_IO_BASE)
#define SMB_GLOBAL_ENABLE  (2 + SMB_IO_BASE)
#define SMB_HOST_ADDRESS   (4 + SMB_IO_BASE)
#define SMB_HOST_DATA      (6 + SMB_IO_BASE)
#define SMB_HOST_COMMAND   (8 + SMB_IO_BASE)

#define SMB_DEVICE_SMC_PIC16LC  0x10

#define SMC_REG_POWER           0x02
#define SMC_REG_POWER_RESET     0x01
#define SMC_REG_POWER_CYCLE     0x40
#define SMC_REG_POWER_SHUTDOWN  0x80

VOID HalpXboxInitPciBus(PBUS_HANDLER BusHandler);
VOID NxConfigurePciDevices(VOID);

/*
 * SMBus byte/word transaction primitives.  Slaves include the SMC (0x10),
 * AV/TV encoders (0x45 / 0x6A), EEPROM (0x54), and temperature sensor
 * (0x4C).  Returns STATUS_IO_DEVICE_ERROR on timeout.
 */
NTSTATUS HalpXboxSmBusReadByte(_In_ UCHAR Address, _In_ UCHAR Register,
                               _Out_ PUCHAR Value);
NTSTATUS HalpXboxSmBusWriteByte(_In_ UCHAR Address, _In_ UCHAR Register,
                                _In_ UCHAR Value);
NTSTATUS HalpXboxSmBusReadWord(_In_ UCHAR Address, _In_ UCHAR Register,
                               _Out_ PUSHORT Value);
NTSTATUS HalpXboxSmBusWriteWord(_In_ UCHAR Address, _In_ UCHAR Register,
                                _In_ USHORT Value);

#endif /* HALXBOX_H_INCLUDED */

/* EOF */
