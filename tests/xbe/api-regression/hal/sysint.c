/*
 * HalDisableSystemInterrupt / HalEnableSystemInterrupt: the mask a
 * driver puts on its own interrupt line.
 *
 * The console names the line by its bus-relative level and carries no
 * IRQL, where NT's pair takes the IDT vector the level was mapped to.
 * What the mask really lives in is the HAL's own record, not the PIC:
 * the mask register is recomputed on the next IRQL transition, so a
 * caller that pokes port 0x21 back the way it was does not stay poked,
 * and the only way to undo a disable is to enable.
 *
 * Every case reads the mask register straight after the call and puts
 * the line back the way it found it before returning.  It works on a
 * line the console leaves masked and nothing is wired to, so an
 * interrupt cannot arrive while it is briefly open.
 */

#include "../harness.h"

/* The coprocessor-error line: masked at rest, and nothing on this
 * console drives it. */
#define TEST_IRQ  13

#define PIC1_MASK 0x21
#define PIC2_MASK 0xA1

static inline uint8_t io_inb(uint16_t port)
{
    uint8_t v;
    __asm__ __volatile__("inb %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}

static bool irq_masked(unsigned irq)
{
    uint8_t mask = (irq < 8) ? io_inb(PIC1_MASK) : io_inb(PIC2_MASK);

    return (mask & (1 << (irq & 7))) != 0;
}

/* Enabling clears the line's mask bit; disabling sets it again. */
static bool t_the_mask_bit_follows_the_call(void)
{
    bool was_masked = irq_masked(TEST_IRQ);

    HalEnableSystemInterrupt(TEST_IRQ, LevelSensitive);
    ASSERT_TRUE(!irq_masked(TEST_IRQ));

    HalDisableSystemInterrupt(TEST_IRQ);
    ASSERT_TRUE(irq_masked(TEST_IRQ));

    if (!was_masked)
        HalEnableSystemInterrupt(TEST_IRQ, LevelSensitive);
    return true;
}

/* Either call twice over is the same as once. */
static bool t_the_calls_are_idempotent(void)
{
    bool was_masked = irq_masked(TEST_IRQ);

    HalEnableSystemInterrupt(TEST_IRQ, LevelSensitive);
    HalEnableSystemInterrupt(TEST_IRQ, LevelSensitive);
    ASSERT_TRUE(!irq_masked(TEST_IRQ));

    HalDisableSystemInterrupt(TEST_IRQ);
    HalDisableSystemInterrupt(TEST_IRQ);
    ASSERT_TRUE(irq_masked(TEST_IRQ));

    if (!was_masked)
        HalEnableSystemInterrupt(TEST_IRQ, LevelSensitive);
    return true;
}

/* Both interrupt modes reach the same mask; the mode is about how the
 * line is dismissed, not whether it is open. */
static bool t_either_mode_opens_the_line(void)
{
    bool was_masked = irq_masked(TEST_IRQ);

    HalEnableSystemInterrupt(TEST_IRQ, Latched);
    ASSERT_TRUE(!irq_masked(TEST_IRQ));
    HalDisableSystemInterrupt(TEST_IRQ);

    HalEnableSystemInterrupt(TEST_IRQ, LevelSensitive);
    ASSERT_TRUE(!irq_masked(TEST_IRQ));
    HalDisableSystemInterrupt(TEST_IRQ);

    if (!was_masked)
        HalEnableSystemInterrupt(TEST_IRQ, LevelSensitive);
    return true;
}

/* A level with no line behind it is ignored rather than folded into
 * the mask of some other one. */
static bool t_a_level_past_the_last_line_is_ignored(void)
{
    uint8_t master = io_inb(PIC1_MASK), slave = io_inb(PIC2_MASK);

    HalDisableSystemInterrupt(16);
    HalDisableSystemInterrupt(31);
    HalDisableSystemInterrupt(0x30 + 7);
    ASSERT_EQ_U32(io_inb(PIC1_MASK), master);
    ASSERT_EQ_U32(io_inb(PIC2_MASK), slave);
    return true;
}

/* The cascade and the clock stay where they are: the pair works on one
 * line, and the rest of the mask is untouched. */
static bool t_the_other_lines_are_untouched(void)
{
    uint8_t master = io_inb(PIC1_MASK);
    uint8_t slave = io_inb(PIC2_MASK);
    bool was_masked = irq_masked(TEST_IRQ);

    HalEnableSystemInterrupt(TEST_IRQ, LevelSensitive);
    ASSERT_EQ_U32(io_inb(PIC1_MASK), master);
    ASSERT_EQ_U32(io_inb(PIC2_MASK) | (1 << (TEST_IRQ & 7)), slave | (1 << (TEST_IRQ & 7)));

    HalDisableSystemInterrupt(TEST_IRQ);
    ASSERT_EQ_U32(io_inb(PIC1_MASK), master);
    ASSERT_EQ_U32(io_inb(PIC2_MASK), slave | (1 << (TEST_IRQ & 7)));

    if (!was_masked)
        HalEnableSystemInterrupt(TEST_IRQ, LevelSensitive);
    return true;
}

static const test_entry_t hal_sysint_entries[] = {
    { "the_mask_bit_follows_the_call", t_the_mask_bit_follows_the_call, NULL },
    { "the_calls_are_idempotent", t_the_calls_are_idempotent, NULL },
    { "either_mode_opens_the_line", t_either_mode_opens_the_line, NULL },
    { "a_level_past_the_last_line_is_ignored",
      t_a_level_past_the_last_line_is_ignored, NULL },
    { "the_other_lines_are_untouched", t_the_other_lines_are_untouched, NULL },
};

DEFINE_GROUP(hal_sysint, "hal/sysint");
