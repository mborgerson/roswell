/*
 * TAP 14 emitter.  Bytes go out the LPC47M157 SuperIO's COM1 at 115200 8N1
 * (I/O 0x3F8).  We program the UART ourselves at first call so the suite
 * works against both nxkrnl (which already programs it for DbgPrint) and
 * the retail kernel (which doesn't).  isa-debugcon (port 0xE9) was the
 * historical target but the upstream xemu AppImage builds without it.
 */

#include "tap.h"
#include <stdio.h>
#include <stdarg.h>

#define COM1     0x3F8
#define COM1_THR (COM1 + 0)
#define COM1_DLL (COM1 + 0)
#define COM1_IER (COM1 + 1)
#define COM1_DLH (COM1 + 1)
#define COM1_FCR (COM1 + 2)
#define COM1_LCR (COM1 + 3)
#define COM1_MCR (COM1 + 4)
#define COM1_LSR (COM1 + 5)

#define LSR_THRE 0x20  /* transmit holding register empty */
#define LSR_TEMT 0x40  /* transmitter empty (shift + holding both drained) */
#define LCR_DLAB 0x80
#define LCR_8N1  0x03

static inline void outb(unsigned short port, unsigned char val)
{
    __asm__ __volatile__("outb %0, %1" : : "a"(val), "Nd"(port));
}

static inline unsigned char inb(unsigned short port)
{
    unsigned char ret;
    __asm__ __volatile__("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

static void com_init(void)
{
    static int initialized = 0;
    if (initialized) return;
    initialized = 1;
    outb(COM1_IER, 0x00);             /* disable all interrupts */
    outb(COM1_LCR, LCR_DLAB);         /* DLAB=1 to set divisor */
    outb(COM1_DLL, 0x01);             /* 115200 baud (1.8432 MHz / 16 / 1) */
    outb(COM1_DLH, 0x00);
    outb(COM1_LCR, LCR_8N1);          /* DLAB=0, 8N1 */
    outb(COM1_FCR, 0xC7);             /* FIFOs enable + clear, 14-byte trigger */
    outb(COM1_MCR, 0x0B);             /* DTR + RTS + OUT2 */
}

static void com_putc(char c)
{
    while ((inb(COM1_LSR) & LSR_THRE) == 0) { }
    outb(COM1_THR, (unsigned char)c);
}

void tap_drain(void)
{
    while ((inb(COM1_LSR) & LSR_TEMT) == 0) { }
}

static void com_puts(const char *s)
{
    com_init();
    while (*s) com_putc(*s++);
}

static void com_vprintf(const char *fmt, va_list ap)
{
    char buf[256];
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    if (n < 0) return;
    if (n > (int)sizeof(buf) - 1) n = sizeof(buf) - 1;
    buf[n] = 0;
    com_puts(buf);
}

static void com_printf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    com_vprintf(fmt, ap);
    va_end(ap);
}

void tap_puts(const char *s)
{
    com_puts(s);
}

void tap_version(unsigned int v)
{
    com_printf("TAP version %u\n", v);
}

void tap_plan(unsigned int n)
{
    com_printf("1..%u\n", n);
}

void tap_ok(unsigned int n, const char *desc)
{
    com_printf("ok %u - %s\n", n, desc);
}

void tap_not_ok(unsigned int n, const char *desc)
{
    com_printf("not ok %u - %s\n", n, desc);
}

void tap_diag_begin(void)
{
    com_puts("  ---\n");
}

void tap_diag_end(void)
{
    com_puts("  ...\n");
}

void tap_diag_kv(const char *key, const char *fmt, ...)
{
    char buf[192];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if (n > (int)sizeof(buf) - 1) n = sizeof(buf) - 1;
    buf[n] = 0;
    com_printf("  %s: %s\n", key, buf);
}

void tap_comment(const char *fmt, ...)
{
    char buf[192];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if (n > (int)sizeof(buf) - 1) n = sizeof(buf) - 1;
    buf[n] = 0;
    com_printf("# %s\n", buf);
}
