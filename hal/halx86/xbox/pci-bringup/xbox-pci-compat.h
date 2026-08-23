/*
 * PROJECT:     Xbox HAL
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Compat shim for the ported cromwell PCI bring-up driver.
 *
 * Provides cromwell's u8/u16/u32 typedefs, port IO, SMBus shim
 * (I2CTransmitWord), wait_us/wait_ms, malloc/free, BUS/DEV/FUNC constants
 * and serial-port constants so cromwell pci.c can compile verbatim.
 */

#ifndef _XBOX_PCI_COMPAT_H_
#define _XBOX_PCI_COMPAT_H_

#include <ntdef.h>
#include <ntifs.h>
#include <string.h>

#include "halxbox.h"

/* Cromwell-style typedefs.  Guarded so they don't collide with the same
 * typedefs in the video compat header when both end up in one TU. */
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

/* Cromwell consts.h selections.  Only what cromwell pci.c references. */
#ifndef BUS_0
#define BUS_0 0
#endif
#ifndef BUS_1
#define BUS_1 1
#endif

#ifndef DEV_0
#define DEV_0  0
#define DEV_1  1
#define DEV_2  2
#define DEV_3  3
#define DEV_4  4
#define DEV_5  5
#define DEV_6  6
#define DEV_7  7
#define DEV_8  8
#define DEV_9  9
#define DEV_1e 0x1e
#endif

#ifndef FUNC_0
#define FUNC_0 0
#endif

#ifndef SERIAL_PORT
#define SERIAL_PORT 0x3F8
#define SERIAL_IRQ  4
#define SERIAL_THR  0
#define SERIAL_LSR  5
#endif

/* IO port helpers -- thin wrappers over the WRITE_PORT_x / READ_PORT_x
 * HAL intrinsics. */
static __inline void IoOutputByte(u16 port, u8 val)
{
    WRITE_PORT_UCHAR((PUCHAR)(ULONG_PTR)port, val);
}

static __inline void IoOutputWord(u16 port, u16 val)
{
    WRITE_PORT_USHORT((PUSHORT)(ULONG_PTR)port, val);
}

static __inline void IoOutputDword(u16 port, u32 val)
{
    WRITE_PORT_ULONG((PULONG)(ULONG_PTR)port, val);
}

static __inline u8 IoInputByte(u16 port)
{
    return READ_PORT_UCHAR((PUCHAR)(ULONG_PTR)port);
}

static __inline u16 IoInputWord(u16 port)
{
    return READ_PORT_USHORT((PUSHORT)(ULONG_PTR)port);
}

static __inline u32 IoInputDword(u16 port)
{
    return READ_PORT_ULONG((PULONG)(ULONG_PTR)port);
}

/* Cromwell packs reg index in the LOW byte and the value in the HIGH byte
 * of the 16-bit word -- see I2CTransmitWord callers in pci.c
 * (I2CTransmitWord(0x10, 0x0b00) writes 0x00 to SMC register 0x0b). */
static __inline int I2CTransmitWord(u8 slave, u16 word)
{
    return NT_SUCCESS(HalpXboxSmBusWriteByte(slave,
                                             (UCHAR)(word >> 8),
                                             (UCHAR)(word & 0xFF)))
        ? 0 : -1;
}

/* Microsecond / millisecond stalls.  KeStallExecutionProcessor takes
 * microseconds, so wait_ms is just a busy-wait loop with a ceiling of one
 * second per call to keep the watchdog quiet. */
static __inline void wait_us(unsigned int us)
{
    KeStallExecutionProcessor(us);
}

static __inline void wait_ms(unsigned int ms)
{
    while (ms--)
        KeStallExecutionProcessor(1000);
}

/* Non-paged pool allocator; cromwell pci.c uses one ~512-byte buffer for
 * the memory-size test pattern. */
#define NXK_PCI_POOL_TAG 'iPxN'

static __inline void *xbp_malloc(unsigned long size)
{
    return ExAllocatePoolWithTag(NonPagedPool, size, NXK_PCI_POOL_TAG);
}

static __inline void xbp_free(void *p)
{
    if (p != NULL)
        ExFreePool(p);
}

#define malloc(sz) xbp_malloc((unsigned long)(sz))
#define free(p)    xbp_free(p)

/* Cromwell pci.c calls bprintf / serial_putchar / console_putchar from a
 * couple of debug paths.  We keep its own definitions of those (they live
 * verbatim in cromwell-pci.c) but vsprintf needs to resolve to something --
 * the kernel exports _vsnprintf, so route through it.  We never expect any
 * of the bprintf paths to be hit at runtime in the ported code; the calls
 * are kept only so the file compiles unchanged. */
int __cdecl _vsnprintf(char *, size_t, const char *, va_list);
#define vsprintf(buf, fmt, ap)  _vsnprintf((buf), 256, (fmt), (ap))

#endif /* _XBOX_PCI_COMPAT_H_ */
