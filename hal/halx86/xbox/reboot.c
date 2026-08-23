/*
 * PROJECT:         Xbox HAL
 * LICENSE:         GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:         Xbox reboot functions
 * COPYRIGHT:       Copyright 2004 Lehner Franz (franz@caos.at)
 *                  Copyright 2019 Stanislav Motylkov (x86corez@gmail.com)
 *
 * REFERENCES:      https://xboxdevwiki.net/SMBus
 *                  https://github.com/XboxDev/cromwell/blob/master/drivers/pci/i2cio.c
 *                  https://github.com/torvalds/linux/blob/master/drivers/i2c/busses/i2c-amd756.c
 *                  https://github.com/xqemu/xqemu/blob/master/hw/xbox/smbus_xbox_smc.c
 */

/* INCLUDES ******************************************************************/

#include "halxbox.h"
#include <xb-debug.h>


/* PRIVATE FUNCTIONS *********************************************************/

static DECLSPEC_NORETURN
VOID
HalpXboxPowerAction(
    _In_ UCHAR Action)
{
    /* Disable interrupts */
    _disable();

    /* Send the command */
    HalpXboxSmBusWriteByte(SMB_DEVICE_SMC_PIC16LC, SMC_REG_POWER, Action);

    /* Halt the CPU */
    __halt();
    UNREACHABLE;
}

DECLSPEC_NORETURN
VOID
HalpReboot(VOID)
{
    HalpXboxPowerAction(SMC_REG_POWER_RESET);
}

/* PUBLIC FUNCTIONS **********************************************************/

#ifndef _MINIHAL_
/*
 * @implemented
 */
VOID
NTAPI
HalReturnToFirmware(
    _In_ FIRMWARE_REENTRY Action)
{
    /* Check what kind of action this is */
    switch (Action)
    {
        /* All recognized actions: call the internal power function */
        case HalHaltRoutine:
        case HalPowerDownRoutine:
        {
            HalpXboxPowerAction(SMC_REG_POWER_SHUTDOWN);
        }
        case HalRestartRoutine:
        {
            HalpXboxPowerAction(SMC_REG_POWER_CYCLE);
        }
        case HalRebootRoutine:
        {
            HalpXboxPowerAction(SMC_REG_POWER_RESET);
        }

        /* Anything else */
        default:
        {
            /* Print message and break */
            XbDbg("HalReturnToFirmware(%d) called!\n", Action);
            DbgBreakPoint();
        }
    }
}
#endif // _MINIHAL_

/* EOF */
