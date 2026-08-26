/*
 * Buffered port I/O (READ/WRITE_PORT_BUFFER_{UCHAR,USHORT,ULONG}).
 *
 * These stream Count elements between a fixed I/O port and a memory buffer
 * via the x86 rep ins/outs instructions.  We exercise them against the PCI
 * type-1 configuration mechanism (ports 0xCF8/0xCFC), which is present and
 * behaves identically on retail silicon and under emulation:
 *
 *   - 0xCF8 is a 32-bit address latch.  Reading it returns the last value
 *     written at every access width, so a known sentinel drives the read
 *     tests deterministically with no hardware side effect.
 *   - 0xCFC is the config-data window.  The per-device Interrupt Line byte
 *     (offset 0x3C) is software-writable scratch with no hardware effect,
 *     so the sub-dword write tests round-trip through it and restore it.
 */

#include "../harness.h"

#define CFG_ADDR 0xCF8
#define CFG_DATA 0xCFC

#define CFG_SEL(dev, off) \
    (0x80000000u | ((dev) << 11) | ((off) & 0xFCu))

static inline uint32_t io_inl(uint16_t port)
{
    uint32_t v;
    __asm__ __volatile__("inl %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}

static inline uint16_t io_inw(uint16_t port)
{
    uint16_t v;
    __asm__ __volatile__("inw %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}

static inline uint8_t io_inb(uint16_t port)
{
    uint8_t v;
    __asm__ __volatile__("inb %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}

static inline void io_outl(uint16_t port, uint32_t v)
{
    __asm__ __volatile__("outl %0, %1" : : "a"(v), "Nd"(port));
}

static inline void io_outb(uint16_t port, uint8_t v)
{
    __asm__ __volatile__("outb %0, %1" : : "a"(v), "Nd"(port));
}

/* Find a bus-0 device whose Interrupt Line byte (0x3C) is writable, and
 * return the config-address selector for that byte.  Restores every byte it
 * touches.  Returns 0 if no such device is present. */
static uint32_t find_scratch_line(void)
{
    for (unsigned dev = 0; dev < 32; dev++) {
        io_outl(CFG_ADDR, CFG_SEL(dev, 0x00));
        if (io_inw(CFG_DATA) == 0xFFFF)
            continue;

        uint32_t sel = CFG_SEL(dev, 0x3C);
        io_outl(CFG_ADDR, sel);
        uint8_t orig = io_inb(CFG_DATA);
        io_outb(CFG_DATA, (uint8_t)(orig ^ 0xFF));
        uint8_t probed = io_inb(CFG_DATA);
        io_outb(CFG_DATA, orig);
        if (probed == (uint8_t)(orig ^ 0xFF))
            return sel;
    }
    return 0;
}

static bool t_read_ulong(void)
{
    uint32_t saved = io_inl(CFG_ADDR);
    io_outl(CFG_ADDR, 0x11223344u);

    ULONG buf[4] = { 0, 0, 0, 0 };
    READ_PORT_BUFFER_ULONG((PULONG)(uintptr_t)CFG_ADDR, buf, 4);
    io_outl(CFG_ADDR, saved);

    for (unsigned i = 0; i < 4; i++)
        ASSERT_EQ_U32(buf[i], 0x11223344u);
    return true;
}

static bool t_read_ushort(void)
{
    uint32_t saved = io_inl(CFG_ADDR);
    io_outl(CFG_ADDR, 0x11223344u);

    uint16_t buf[4] = { 0, 0, 0, 0 };
    READ_PORT_BUFFER_USHORT((PUSHORT)(uintptr_t)CFG_ADDR, buf, 4);
    io_outl(CFG_ADDR, saved);

    for (unsigned i = 0; i < 4; i++)
        ASSERT_EQ_U32(buf[i], 0x3344u);
    return true;
}

static bool t_read_uchar(void)
{
    uint32_t saved = io_inl(CFG_ADDR);
    io_outl(CFG_ADDR, 0x11223344u);

    uint8_t buf[4] = { 0, 0, 0, 0 };
    READ_PORT_BUFFER_UCHAR((PUCHAR)(uintptr_t)CFG_ADDR, buf, 4);
    io_outl(CFG_ADDR, saved);

    for (unsigned i = 0; i < 4; i++)
        ASSERT_EQ_U32(buf[i], 0x44u);
    return true;
}

static bool t_write_ulong(void)
{
    uint32_t saved = io_inl(CFG_ADDR);

    /* The last element is the value that must remain latched.  Its bit 31 is
     * clear so config space stays disabled; we never touch 0xCFC here. */
    ULONG vals[4] = { 0x0BADF00Du, 0xCAFEBABEu, 0xDEADBEEFu, 0x12345678u };
    WRITE_PORT_BUFFER_ULONG((PULONG)(uintptr_t)CFG_ADDR, vals, 4);
    uint32_t got = io_inl(CFG_ADDR);
    io_outl(CFG_ADDR, saved);

    ASSERT_EQ_U32(got, 0x12345678u);
    return true;
}

static bool t_write_uchar(void)
{
    uint32_t sel = find_scratch_line();
    ASSERT_TRUE(sel != 0);

    io_outl(CFG_ADDR, sel);
    uint8_t orig = io_inb(CFG_DATA);

    uint8_t vals[3] = { 0xA5, 0x5A, 0x3C };
    WRITE_PORT_BUFFER_UCHAR((PUCHAR)(uintptr_t)CFG_DATA, vals, 3);
    uint8_t got = io_inb(CFG_DATA);
    io_outb(CFG_DATA, orig);

    ASSERT_EQ_U32(got, 0x3Cu);
    return true;
}

static bool t_write_ushort(void)
{
    uint32_t sel = find_scratch_line();
    ASSERT_TRUE(sel != 0);

    io_outl(CFG_ADDR, sel);
    uint16_t orig = io_inw(CFG_DATA);
    /* Interrupt Pin (byte 0x3D) is read-only; keep it in the written words so
     * the 16-bit read-back is exactly what we wrote. */
    uint16_t hi = (uint16_t)(orig & 0xFF00u);

    uint16_t vals[3] = {
        (uint16_t)(hi | 0xA5u),
        (uint16_t)(hi | 0x5Au),
        (uint16_t)(hi | 0x3Cu),
    };
    WRITE_PORT_BUFFER_USHORT((PUSHORT)(uintptr_t)CFG_DATA, vals, 3);
    uint16_t got = io_inw(CFG_DATA);
    io_outb(CFG_DATA, (uint8_t)(orig & 0xFFu));

    ASSERT_EQ_U32(got, (uint32_t)(hi | 0x3Cu));
    return true;
}

static const test_entry_t hal_portio_entries[] = {
    {"read_buffer_ulong",   t_read_ulong},
    {"read_buffer_ushort",  t_read_ushort},
    {"read_buffer_uchar",   t_read_uchar},
    {"write_buffer_ulong",  t_write_ulong},
    {"write_buffer_uchar",  t_write_uchar},
    {"write_buffer_ushort", t_write_ushort},
};

DEFINE_GROUP(hal_portio, "hal/portio");
