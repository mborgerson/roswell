/*
 * Serial debug (COM1 via the xemu LPC47M157 SuperIO) + dprintf
 * implementation.  Output reaches whatever xemu attaches with -serial;
 * formatting goes through nanoprintf to keep the loader's I/O surface tiny.
 *
 * Only compiled + linked into DBG builds (see CMakeLists.txt).
 */

#include <stdarg.h>

#include "log.h"
#include "nanoprintf.h"
#include "nxldr.h"

#define COM1_BASE 0x3f8
#define COM1_THR (COM1_BASE + 0)
#define COM1_DLL (COM1_BASE + 0)
#define COM1_DLM (COM1_BASE + 1)
#define COM1_FCR (COM1_BASE + 2)
#define COM1_LCR (COM1_BASE + 3)
#define COM1_MCR (COM1_BASE + 4)
#define COM1_LSR (COM1_BASE + 5)

static void
outb(USHORT Port, UCHAR Value)
{
    __asm__ __volatile__("outb %0, %1" ::"a"(Value), "Nd"(Port));
}

static UCHAR
inb(USHORT Port)
{
    UCHAR Value;
    __asm__ __volatile__("inb %1, %0" : "=a"(Value) : "Nd"(Port));
    return Value;
}

/* The xemu LPC47M157 SuperIO config port is at 0x2e (sysopt=0).  COM
 * ports ship disabled; we have to walk the standard SMSC config-mode
 * dance to enable COM1 at 0x3F8 before touching the UART. */
#define SIO_CFG_PORT 0x2e
#define SIO_DATA_PORT 0x2f

static void
SioWrite(UCHAR Index, UCHAR Value)
{
    outb(SIO_CFG_PORT, Index);
    outb(SIO_DATA_PORT, Value);
}

static void
SerialPutChar(char c)
{
    while ((inb(COM1_LSR) & 0x20) == 0)
    {
    }
    outb(COM1_THR, (UCHAR)c);
}

/* nanoprintf putc callback: translate `\n` -> `\r\n` to match xemu's
 * serial expectations. */
static void
SerialPutCharNpf(int c, void *Ctx)
{
    (void)Ctx;
    if (c == '\n')
        SerialPutChar('\r');
    SerialPutChar((char)c);
}

void
LogInit(void)
{
    /* Enter SuperIO config mode. */
    outb(SIO_CFG_PORT, 0x55);

    /* Select logical device 4 (DEVICE_SERIAL_PORT_1), point it at COM1,
     * give it IRQ 4, activate.  Field offsets per xemu lpc47m157.c. */
    SioWrite(0x07, 0x04);
    SioWrite(0x60, 0x03);
    SioWrite(0x61, 0xf8);
    SioWrite(0x70, 0x04);
    SioWrite(0x30, 0x01);

    /* Exit config mode -- update_devices() runs here. */
    outb(SIO_CFG_PORT, 0xaa);

    /* Now COM1 is decoded.  Program the 16550 for 115200 8N1. */
    outb(COM1_LCR, 0x80); /* DLAB on */
    outb(COM1_DLL, 1);    /* divisor 1 -> 115200 baud */
    outb(COM1_DLM, 0);
    outb(COM1_LCR, 0x03); /* 8N1, DLAB off */
    outb(COM1_FCR, 0xC7); /* FIFO enable + clear */
    outb(COM1_MCR, 0x0B); /* DTR + RTS + OUT2 */
}

void
LogDrain(void)
{
    /* Wait until the transmit shift register is empty too -- not just
     * FIFO has room.  Lets us see the true last-print before a fault
     * without later FIFO bytes getting lost to xemu's reset. */
    while ((inb(COM1_LSR) & 0x40) == 0)
    {
    }
}

void
Log(const char *Format, ...)
{
    va_list ap;
    va_start(ap, Format);
    npf_vpprintf(SerialPutCharNpf, NULL, Format, ap);
    va_end(ap);
    LogDrain();
}
