/*
 * Raw IRP allocation, as a title driver does it.
 *
 * IoAllocateIrp hands back a packet sized for a stack depth; the caller
 * fills in a stack location and dispatches it itself.  IoInitializeIrp
 * is the same shaping applied to memory the caller already owns, which
 * is how a driver puts a packet in its device extension.  Both write the
 * header fields the title then reads, so the sizes and the initial
 * CurrentLocation are contract.
 */

#include "../harness.h"
#include <string.h>

/* IO_TYPE_IRP: the type code the kernel stamps into a packet. */
#define IRP_TYPE 6

static bool check_shape(PIRP irp, CCHAR stack_size, const char *what)
{
    if (irp->Type != IRP_TYPE)
        FAIL_AND_RETURN("%s: Type %d expected %d", what, (int)irp->Type,
                        IRP_TYPE);
    if (irp->StackCount != stack_size)
        FAIL_AND_RETURN("%s: StackCount %d expected %d", what,
                        (int)irp->StackCount, (int)stack_size);
    /* The current location starts one past the deepest, so the first
     * IoSetNextIrpStackLocation lands on the last slot. */
    if (irp->CurrentLocation != (CHAR)(stack_size + 1))
        FAIL_AND_RETURN("%s: CurrentLocation %d expected %d", what,
                        (int)irp->CurrentLocation, (int)(stack_size + 1));
    if (irp->Size == 0)
        FAIL_AND_RETURN("%s: Size 0", what);
    return true;
}

static bool t_allocate_one_stack(void)
{
    PIRP irp = IoAllocateIrp(1);

    ASSERT_NOT_NULL(irp);
    if (!check_shape(irp, 1, "allocate(1)")) {
        IoFreeIrp(irp);
        return false;
    }
    IoFreeIrp(irp);
    return true;
}

static bool t_allocate_grows_with_stack(void)
{
    PIRP one = IoAllocateIrp(1);
    PIRP four;
    USHORT size_one, size_four;

    ASSERT_NOT_NULL(one);
    size_one = one->Size;
    IoFreeIrp(one);

    four = IoAllocateIrp(4);
    ASSERT_NOT_NULL(four);
    if (!check_shape(four, 4, "allocate(4)")) {
        IoFreeIrp(four);
        return false;
    }
    size_four = four->Size;
    IoFreeIrp(four);

    /* Three more stack locations, at a fixed size each. */
    if (size_four <= size_one)
        FAIL_AND_RETURN("Size did not grow: %u stacks=1, %u stacks=4",
                        (unsigned)size_one, (unsigned)size_four);
    if ((size_four - size_one) % 3 != 0)
        FAIL_AND_RETURN("stack growth %u not divisible by 3",
                        (unsigned)(size_four - size_one));
    return true;
}

static bool t_reuse_after_free(void)
{
    PIRP a = IoAllocateIrp(2);
    PIRP b;

    ASSERT_NOT_NULL(a);
    IoFreeIrp(a);

    /* A freed packet goes back to the pool and comes out shaped again,
     * not carrying the previous caller's fields. */
    b = IoAllocateIrp(2);
    ASSERT_NOT_NULL(b);
    if (!check_shape(b, 2, "reuse")) {
        IoFreeIrp(b);
        return false;
    }
    IoFreeIrp(b);
    return true;
}

static bool t_initialize_caller_memory(void)
{
    static UCHAR buffer[512];
    PIRP irp = (PIRP)buffer;
    USHORT packet_size;

    /* Size the packet the way the allocator would, by asking it. */
    {
        PIRP probe = IoAllocateIrp(2);
        ASSERT_NOT_NULL(probe);
        packet_size = probe->Size;
        IoFreeIrp(probe);
    }
    ASSERT_TRUE(packet_size <= sizeof(buffer));

    memset(buffer, 0xCC, sizeof(buffer));
    IoInitializeIrp(irp, packet_size, 2);

    if (!check_shape(irp, 2, "initialize"))
        return false;
    ASSERT_EQ_U32(irp->Size, packet_size);

    /* Initialization clears the fields a stale buffer would otherwise
     * carry into the IO manager. */
    ASSERT_EQ_U32(irp->Flags, 0);
    ASSERT_EQ_U32(irp->Cancel, 0);
    ASSERT_EQ_U32(irp->PendingReturned, 0);
    ASSERT_EQ_PTR(irp->UserIosb, NULL);
    ASSERT_EQ_PTR(irp->UserEvent, NULL);
    return true;
}

static const test_entry_t io_irpalloc_entries[] = {
    {"allocate_one_stack",       t_allocate_one_stack},
    {"allocate_grows_with_stack", t_allocate_grows_with_stack},
    {"reuse_after_free",         t_reuse_after_free},
    {"initialize_caller_memory", t_initialize_caller_memory},
};

DEFINE_GROUP(io_irpalloc, "io/irpalloc");
