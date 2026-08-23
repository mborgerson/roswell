/*
 * PROJECT:     Xbox HAL
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Compat shim for the ported cromwell video driver.
 *
 * Provides cromwell's u8/u16/u32 typedefs, port IO, I2C
 * helpers, wait_us, malloc/free, fabs and a printk macro -- everything
 * the 13 ported video files reference under one #include.
 */

#ifndef _XBOX_VIDEO_COMPAT_H_
#define _XBOX_VIDEO_COMPAT_H_

#include <ntdef.h>
#include <ntifs.h>
#include <string.h>

#include "halxbox.h"

/* Cromwell-style typedefs -- guarded so they don't collide with stdbool /
 * NT headers. */
#ifndef _CROMWELL_TYPES_DEFINED
#define _CROMWELL_TYPES_DEFINED
typedef unsigned char  u8;
typedef unsigned short u16;
typedef unsigned int   u32;
typedef signed   int   s32;
#endif

#ifndef __cplusplus
#ifndef bool
typedef int bool;
#define true  1
#define false 0
#endif
#endif

typedef struct { u8 r, g, b, a; } RGBA;

/* SMBus error sentinel cromwell returns from I2CTransmitByteGetReturn. */
#ifndef ERR_I2C_ERROR_BUS
#define ERR_I2C_ERROR_BUS (-1)
#endif

/* Cromwell's preferred default video mode is 640x480 (VIDEO_MODE_640x480
 * is defined in BootVideo.h). */
#define VIDEO_PREFERRED_MODE VIDEO_MODE_640x480

/* Cromwell's framebuffer aperture base.  64 MiB - 4 MiB from the NV2A
 * aperture at 0xF0000000 == 0xF3C00000.  TODO: revisit for the 128 MiB
 * devkit configuration. */
#define FB_START (0xf0000000U | (64U * 1024U * 1024U - 0x00400000U))

/* Shared globals across the ported files. */
extern unsigned int video_encoder;
extern u8 VIDEO_AV_MODE;
extern unsigned int xbox_ram;
extern u8 eeprom[256];

/* IO port helpers. */
static __inline void IoOutputByte(u16 port, u8 val)
{
    WRITE_PORT_UCHAR((PUCHAR)(ULONG_PTR)port, val);
}

static __inline void IoOutputDword(u16 port, u32 val)
{
    WRITE_PORT_ULONG((PULONG)(ULONG_PTR)port, val);
}

static __inline u8 IoInputByte(u16 port)
{
    return READ_PORT_UCHAR((PUCHAR)(ULONG_PTR)port);
}

/* SMBus shims wrapping HalpXboxSmBus* (smbus.c).  Cromwell uses 7-bit
 * slave addresses (0x10, 0x45, 0x54, 0x6A, 0x70), which is also what
 * HalpXboxSmBusTransfer expects -- it left-shifts by 1 and ORs in R/W
 * itself.  No address translation needed. */
static __inline int I2CTransmitByteGetReturn(u8 slave, u8 reg)
{
    UCHAR v = 0;
    if (!NT_SUCCESS(HalpXboxSmBusReadByte(slave, reg, &v)))
        return ERR_I2C_ERROR_BUS;
    return (int)v;
}

static __inline int I2CWriteBytetoRegister(u8 slave, u8 reg, u8 val)
{
    return NT_SUCCESS(HalpXboxSmBusWriteByte(slave, reg, val)) ? 0 : -1;
}

/* Cromwell packs reg in the high byte of a 16-bit word and the value
 * in the low byte. */
static __inline int I2CTransmitWord(u8 slave, u16 word)
{
    return NT_SUCCESS(HalpXboxSmBusWriteByte(slave,
                                             (UCHAR)(word >> 8),
                                             (UCHAR)(word & 0xFF)))
        ? 0 : -1;
}

/* Xcalibur 32-bit SMBus read/write -- four sequential byte transactions. */
static __inline int ReadfromSMBus(u8 slave, u8 reg, u8 size, void *out)
{
    u8 *p = (u8 *)out;
    UCHAR b;
    u8 i;

    for (i = 0; i < size; i++)
    {
        if (!NT_SUCCESS(HalpXboxSmBusReadByte(slave, (UCHAR)(reg + i), &b)))
            return ERR_I2C_ERROR_BUS;
        p[i] = b;
    }
    return 0;
}

static __inline int WriteToSMBus(u8 slave, u8 reg, u8 size, unsigned long val)
{
    u8 i;
    for (i = 0; i < size; i++)
    {
        UCHAR b = (UCHAR)((val >> (i * 8)) & 0xFF);
        if (!NT_SUCCESS(HalpXboxSmBusWriteByte(slave, (UCHAR)(reg + i), b)))
            return ERR_I2C_ERROR_BUS;
    }
    return 0;
}

/* Microsecond delay -- KeStallExecutionProcessor takes microseconds. */
static __inline void wait_us(unsigned int us)
{
    KeStallExecutionProcessor(us);
}

/* Cromwell allocates a ~256-byte encoder-reg buffer; route to non-paged
 * pool with the same tag the rest of the HAL Xbox code uses. */
#define NXK_VIDEO_POOL_TAG 'oedV'

static __inline void *xbv_malloc(unsigned long size)
{
    return ExAllocatePoolWithTag(NonPagedPool, size, NXK_VIDEO_POOL_TAG);
}

static __inline void xbv_free(void *p)
{
    if (p != NULL)
        ExFreePool(p);
}

#define malloc(sz) xbv_malloc((unsigned long)(sz))
#define free(p)    xbv_free(p)

/* Debug print -- cromwell's printk maps to DbgPrint (HAL is built into
 * the kernel image so DbgPrint resolves directly). */
#define printk(...) DbgPrint(__VA_ARGS__)

/* Cromwell relies on min/max being available as ordinary macros, not
 * Linux's typecheck variants.  Define unless something else already
 * supplied them. */
#ifndef min
#define min(a, b) (((a) < (b)) ? (a) : (b))
#endif
#ifndef max
#define max(a, b) (((a) > (b)) ? (a) : (b))
#endif

/* Math helpers.  Only fabs is actually used by VideoInitialization.c +
 * conexant.c; floor/ceil/pow are not.  Avoid pulling <math.h> (would
 * require libm and conflicts with the per-file static double fabs). */
#define _XBOX_VIDEO_HAS_FABS 1   /* tells .c files not to redefine */

#endif /* _XBOX_VIDEO_COMPAT_H_ */
