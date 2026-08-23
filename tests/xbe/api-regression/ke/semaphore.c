/*
 * KeInitializeSemaphore + KeReleaseSemaphore + KeWaitForSingleObject.
 */

#include "../harness.h"

static bool t_init_zero(void)
{
    KSEMAPHORE sem;
    KeInitializeSemaphore(&sem, 0, 3);
    LARGE_INTEGER zero = { .QuadPart = 0 };
    NTSTATUS s = KeWaitForSingleObject(&sem, Executive, KernelMode, FALSE, &zero);
    ASSERT_NTSTATUS(s, STATUS_TIMEOUT);
    return true;
}

static bool t_init_two(void)
{
    KSEMAPHORE sem;
    KeInitializeSemaphore(&sem, 2, 3);
    LARGE_INTEGER zero = { .QuadPart = 0 };

    NTSTATUS s = KeWaitForSingleObject(&sem, Executive, KernelMode, FALSE, &zero);
    ASSERT_NTSTATUS(s, STATUS_SUCCESS);
    s = KeWaitForSingleObject(&sem, Executive, KernelMode, FALSE, &zero);
    ASSERT_NTSTATUS(s, STATUS_SUCCESS);
    s = KeWaitForSingleObject(&sem, Executive, KernelMode, FALSE, &zero);
    ASSERT_NTSTATUS(s, STATUS_TIMEOUT);
    return true;
}

static bool t_release_increments(void)
{
    KSEMAPHORE sem;
    KeInitializeSemaphore(&sem, 0, 4);
    LONG prev = KeReleaseSemaphore(&sem, 0, 1, FALSE);
    ASSERT_EQ_U32(prev, 0);

    prev = KeReleaseSemaphore(&sem, 0, 2, FALSE);
    ASSERT_EQ_U32(prev, 1);

    LARGE_INTEGER zero = { .QuadPart = 0 };
    NTSTATUS s = KeWaitForSingleObject(&sem, Executive, KernelMode, FALSE, &zero);
    ASSERT_NTSTATUS(s, STATUS_SUCCESS);
    s = KeWaitForSingleObject(&sem, Executive, KernelMode, FALSE, &zero);
    ASSERT_NTSTATUS(s, STATUS_SUCCESS);
    s = KeWaitForSingleObject(&sem, Executive, KernelMode, FALSE, &zero);
    ASSERT_NTSTATUS(s, STATUS_SUCCESS);
    s = KeWaitForSingleObject(&sem, Executive, KernelMode, FALSE, &zero);
    ASSERT_NTSTATUS(s, STATUS_TIMEOUT);
    return true;
}

static const test_entry_t ke_semaphore_entries[] = {
    {"init_zero",          t_init_zero},
    {"init_two",           t_init_two},
    {"release_increments", t_release_increments},
};

DEFINE_GROUP(ke_semaphore, "ke/semaphore");
