/*
 * KeInitializeApc: the fields the console stamps into a caller-owned KAPC.
 *
 * The structure belongs to the title, so the exact bytes matter.  The
 * console's KAPC is 40 bytes -- eight smaller than NT's, which carries a
 * Size/Spare pair up front and the mode flags at the tail -- and the
 * routine leaves the list entry and both system arguments alone.  Every
 * case fills the buffer with a sentinel first, so an untouched field is
 * distinguishable from a zeroed one, and guards the word behind it.
 */

#include "../harness.h"
#include <string.h>

#define APC_SENTINEL 0xCC
#define GUARD 0xA5A5A5A5u

/* KOBJECTS: the value the retail kernel stamps into an APC. */
#define APC_OBJECT 0x12

typedef struct {
    KAPC  apc;
    ULONG guard;
} guarded_apc_t;

static UCHAR g_before[sizeof(KAPC)];
static char g_dump[2 * sizeof(KAPC) + 1];

static void NTAPI kernel_routine(PKAPC Apc, PKNORMAL_ROUTINE *NormalRoutine,
                                 PVOID *NormalContext, PVOID *SystemArgument1,
                                 PVOID *SystemArgument2)
{
    (void)Apc; (void)NormalRoutine; (void)NormalContext;
    (void)SystemArgument1; (void)SystemArgument2;
}

static void NTAPI rundown_routine(PKAPC Apc)
{
    (void)Apc;
}

static void NTAPI normal_routine(PVOID NormalContext, PVOID SystemArgument1,
                                 PVOID SystemArgument2)
{
    (void)NormalContext; (void)SystemArgument1; (void)SystemArgument2;
}

/* Hex of the whole structure, so one failing run reports every byte. */
static const char *dump(const KAPC *apc)
{
    static const char digits[] = "0123456789abcdef";
    const UCHAR *p = (const UCHAR *)apc;
    size_t i;

    for (i = 0; i < sizeof(KAPC); i++) {
        g_dump[i * 2] = digits[p[i] >> 4];
        g_dump[i * 2 + 1] = digits[p[i] & 0xF];
    }
    g_dump[sizeof(KAPC) * 2] = '\0';
    return g_dump;
}

static void fill(guarded_apc_t *g)
{
    memset(g, APC_SENTINEL, sizeof(*g));
    g->guard = GUARD;
    memcpy(g_before, &g->apc, sizeof(g->apc));
}

/* A field the routine must not touch still holds the sentinel. */
static bool untouched(const KAPC *apc, size_t offset, size_t length)
{
    return memcmp((const UCHAR *)apc + offset, g_before + offset,
                  length) == 0;
}

static bool t_console_layout(void)
{
    guarded_apc_t g;
    PKTHREAD thread = KeGetCurrentThread();

    ASSERT_EQ_U32(sizeof(KAPC), 40);

    fill(&g);
    KeInitializeApc(&g.apc, thread, kernel_routine, rundown_routine,
                    normal_routine, UserMode, (PVOID)0xA1A1A1A1);

    if (g.apc.Type != APC_OBJECT || g.apc.ApcMode != UserMode ||
        g.apc.Inserted != FALSE)
        FAIL_AND_RETURN("header: %s", dump(&g.apc));
    ASSERT_EQ_PTR(g.apc.Thread, thread);
    ASSERT_EQ_PTR(g.apc.KernelRoutine, (PVOID)kernel_routine);
    ASSERT_EQ_PTR(g.apc.RundownRoutine, (PVOID)rundown_routine);
    ASSERT_EQ_PTR(g.apc.NormalRoutine, (PVOID)normal_routine);
    ASSERT_EQ_PTR(g.apc.NormalContext, (PVOID)0xA1A1A1A1);

    /* Nothing may be written past the title's 40-byte structure. */
    ASSERT_EQ_U32(g.guard, GUARD);
    return true;
}

/* The list entry and both system arguments are KeInsertQueueApc's to
 * write; initialisation leaves them as it found them. */
static bool t_leaves_the_tail_alone(void)
{
    guarded_apc_t g;

    fill(&g);
    KeInitializeApc(&g.apc, KeGetCurrentThread(), kernel_routine,
                    rundown_routine, normal_routine, KernelMode, NULL);

    if (!untouched(&g.apc, offsetof(KAPC, ApcListEntry),
                   sizeof(LIST_ENTRY)))
        FAIL_AND_RETURN("list entry written: %s", dump(&g.apc));
    if (!untouched(&g.apc, offsetof(KAPC, SystemArgument1),
                   2 * sizeof(PVOID)))
        FAIL_AND_RETURN("system arguments written: %s", dump(&g.apc));
    return true;
}

/* An APC with no normal routine has nothing to run in user mode, so the
 * requested mode is overridden rather than honoured. */
static bool t_null_normal_routine_forces_kernel_mode(void)
{
    guarded_apc_t g;

    fill(&g);
    KeInitializeApc(&g.apc, KeGetCurrentThread(), kernel_routine, NULL, NULL,
                    UserMode, (PVOID)0xB2B2B2B2);

    if (g.apc.ApcMode != KernelMode)
        FAIL_AND_RETURN("mode %d: %s", (int)g.apc.ApcMode, dump(&g.apc));
    ASSERT_EQ_PTR(g.apc.NormalRoutine, NULL);
    ASSERT_EQ_PTR(g.apc.RundownRoutine, NULL);
    /* With no routine to receive it the context is dropped, not kept. */
    ASSERT_EQ_PTR(g.apc.NormalContext, NULL);
    ASSERT_EQ_U32(g.guard, GUARD);
    return true;
}

/* Re-initialising an APC overwrites every stamped field; nothing from the
 * previous round survives. */
static bool t_reinitialise_overwrites(void)
{
    guarded_apc_t g;

    fill(&g);
    KeInitializeApc(&g.apc, KeGetCurrentThread(), kernel_routine,
                    rundown_routine, normal_routine, UserMode,
                    (PVOID)0xA1A1A1A1);
    KeInitializeApc(&g.apc, KeGetCurrentThread(), kernel_routine, NULL, NULL,
                    KernelMode, NULL);

    ASSERT_EQ_PTR(g.apc.RundownRoutine, NULL);
    ASSERT_EQ_PTR(g.apc.NormalRoutine, NULL);
    ASSERT_EQ_PTR(g.apc.NormalContext, NULL);
    ASSERT_EQ_U32(g.apc.ApcMode, KernelMode);
    return true;
}

static const test_entry_t ke_apcinit_entries[] = {
    { "console_layout", t_console_layout, NULL },
    { "leaves_the_tail_alone", t_leaves_the_tail_alone, NULL },
    { "null_normal_routine_forces_kernel_mode",
      t_null_normal_routine_forces_kernel_mode, NULL },
    { "reinitialise_overwrites", t_reinitialise_overwrites, NULL },
};

DEFINE_GROUP(ke_apcinit, "ke/apcinit");
